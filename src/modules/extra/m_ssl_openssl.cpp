/*       +------------------------------------+
 *       | Inspire Internet Relay Chat Daemon |
 *       +------------------------------------+
 *
 *  InspIRCd: (C) 2002-2008 InspIRCd Development Team
 * See: http://www.inspircd.org/wiki/index.php/Credits
 *
 * This program is free but copyrighted software; see
 *            the file COPYING for details.
 *
 * ---------------------------------------------------
 */

#include "inspircd.h"

#include <openssl/ssl.h>
#include <openssl/err.h>

#include "configreader.h"
#include "users.h"
#include "channels.h"
#include "modules.h"

#include "socket.h"
#include "hashcomp.h"

#include "transport.h"

#ifdef WINDOWS
#pragma comment(lib, "libeay32MTd")
#pragma comment(lib, "ssleay32MTd")
#undef MAX_DESCRIPTORS
#define MAX_DESCRIPTORS 10000
#endif

/* $ModDesc: Provides SSL support for clients */

/* $LinkerFlags: if("USE_FREEBSD_BASE_SSL") -lssl -lcrypto */
/* $CompileFlags: if(!"USE_FREEBSD_BASE_SSL") pkgconfversion("openssl","0.9.7") pkgconfincludes("openssl","/openssl/ssl.h","") */
/* $LinkerFlags: if(!"USE_FREEBSD_BASE_SSL") rpath("pkg-config --libs openssl") pkgconflibs("openssl","/libssl.so","-lssl -lcrypto -ldl") */

/* $ModDep: transport.h */
/* $NoPedantic */
/* $CopyInstall: conf/key.pem $(CONPATH) */
/* $CopyInstall: conf/cert.pem $(CONPATH) */


enum issl_status { ISSL_NONE, ISSL_HANDSHAKING, ISSL_OPEN };
enum issl_io_status { ISSL_WRITE, ISSL_READ };

static bool SelfSigned = false;

bool isin(const std::string &host, int port, const std::vector<std::string> &portlist)
{
	if (std::find(portlist.begin(), portlist.end(), "*:" + ConvToStr(port)) != portlist.end())
		return true;

	if (std::find(portlist.begin(), portlist.end(), ":" + ConvToStr(port)) != portlist.end())
		return true;

	return std::find(portlist.begin(), portlist.end(), host + ":" + ConvToStr(port)) != portlist.end();
}

char* get_error()
{
	return ERR_error_string(ERR_get_error(), NULL);
}

static int error_callback(const char *str, size_t len, void *u);

/** Represents an SSL user's extra data
 */
class issl_session : public classbase
{
public:
	SSL* sess;
	issl_status status;
	issl_io_status rstat;
	issl_io_status wstat;

	unsigned int inbufoffset;
	char* inbuf;			// Buffer OpenSSL reads into.
	std::string outbuf;	// Buffer for outgoing data that OpenSSL will not take.
	int fd;
	bool outbound;

	issl_session()
	{
		outbound = false;
		rstat = ISSL_READ;
		wstat = ISSL_WRITE;
	}
};

static int OnVerify(int preverify_ok, X509_STORE_CTX *ctx)
{
	/* XXX: This will allow self signed certificates.
	 * In the future if we want an option to not allow this,
	 * we can just return preverify_ok here, and openssl
	 * will boot off self-signed and invalid peer certs.
	 */
	int ve = X509_STORE_CTX_get_error(ctx);

	SelfSigned = (ve == X509_V_ERR_DEPTH_ZERO_SELF_SIGNED_CERT);

	return 1;
}

class ModuleSSLOpenSSL : public Module
{
	std::vector<std::string> listenports;

	int inbufsize;
	issl_session* sessions;

	SSL_CTX* ctx;
	SSL_CTX* clictx;

	char* dummy;
	char cipher[MAXBUF];

	std::string keyfile;
	std::string certfile;
	std::string cafile;
	// std::string crlfile;
	std::string dhfile;
	std::string sslports;

	int clientactive;

 public:

	InspIRCd* PublicInstance;

	ModuleSSLOpenSSL(InspIRCd* Me)
	: Module(Me), PublicInstance(Me)
	{
		ServerInstance->Modules->PublishInterface("BufferedSocketHook", this);

		sessions = new issl_session[ServerInstance->SE->GetMaxFds()];

		// Not rehashable...because I cba to reduce all the sizes of existing buffers.
		inbufsize = ServerInstance->Config->NetBufferSize;

		/* Global SSL library initialization*/
		SSL_library_init();
		SSL_load_error_strings();

		/* Build our SSL contexts:
		 * NOTE: OpenSSL makes us have two contexts, one for servers and one for clients. ICK.
		 */
		ctx = SSL_CTX_new( SSLv23_server_method() );
		clictx = SSL_CTX_new( SSLv23_client_method() );

		SSL_CTX_set_mode(ctx, SSL_MODE_ENABLE_PARTIAL_WRITE | SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);
		SSL_CTX_set_mode(clictx, SSL_MODE_ENABLE_PARTIAL_WRITE | SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);

		SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER | SSL_VERIFY_CLIENT_ONCE, OnVerify);
		SSL_CTX_set_verify(clictx, SSL_VERIFY_PEER | SSL_VERIFY_CLIENT_ONCE, OnVerify);

		// Needs the flag as it ignores a plain /rehash
		OnRehash(NULL,"ssl");
		Implementation eventlist[] = { I_OnRawSocketConnect, I_OnRawSocketAccept, I_OnRawSocketClose, I_OnRawSocketRead, I_OnRawSocketWrite, I_OnCleanup, I_On005Numeric,
			I_OnBufferFlushed, I_OnRequest, I_OnSyncUserMetaData, I_OnDecodeMetaData, I_OnUnloadModule, I_OnRehash, I_OnWhois, I_OnPostConnect, I_OnHookUserIO };
		ServerInstance->Modules->Attach(eventlist, this, 16);
	}

	virtual void OnHookUserIO(User* user, const std::string &targetip)
	{
		if (!user->io && isin(targetip,user->GetPort(), listenports))
		{
			/* Hook the user with our module */
			user->io = this;
		}
	}

	virtual void OnRehash(User* user, const std::string &param)
	{
		ConfigReader Conf(ServerInstance);

		listenports.clear();
		clientactive = 0;
		sslports.clear();

		for(int index = 0; index < Conf.Enumerate("bind"); index++)
		{
			// For each <bind> tag
			std::string x = Conf.ReadValue("bind", "type", index);
			if(((x.empty()) || (x == "clients")) && (Conf.ReadValue("bind", "ssl", index) == "openssl"))
			{
				// Get the port we're meant to be listening on with SSL
				std::string port = Conf.ReadValue("bind", "port", index);
				std::string addr = Conf.ReadValue("bind", "address", index);

				irc::portparser portrange(port, false);
				long portno = -1;
				while ((portno = portrange.GetToken()))
				{
					clientactive++;
					try
					{
						listenports.push_back(addr + ":" + ConvToStr(portno));

						for (size_t i = 0; i < ServerInstance->Config->ports.size(); i++)
							if ((ServerInstance->Config->ports[i]->GetPort() == portno) && (ServerInstance->Config->ports[i]->GetIP() == addr))
								ServerInstance->Config->ports[i]->SetDescription("ssl");
						ServerInstance->Logs->Log("m_ssl_openssl",DEFAULT, "m_ssl_gnutls.so: Enabling SSL for port %ld", portno);

						sslports.append((addr.empty() ? "*" : addr)).append(":").append(ConvToStr(portno)).append(";");
					}
					catch (ModuleException &e)
					{
						ServerInstance->Logs->Log("m_ssl_openssl",DEFAULT, "m_ssl_gnutls.so: FAILED to enable SSL on port %ld: %s. Maybe it's already hooked by the same port on a different IP, or you have an other SSL or similar module loaded?", portno, e.GetReason());
					}
				}
			}
		}

		if (!sslports.empty())
			sslports.erase(sslports.end() - 1);

		if (param != "ssl")
		{
			return;
		}

		std::string confdir(ServerInstance->ConfigFileName);
		// +1 so we the path ends with a /
		confdir = confdir.substr(0, confdir.find_last_of('/') + 1);

		cafile	 = Conf.ReadValue("openssl", "cafile", 0);
		certfile = Conf.ReadValue("openssl", "certfile", 0);
		keyfile	 = Conf.ReadValue("openssl", "keyfile", 0);
		dhfile	 = Conf.ReadValue("openssl", "dhfile", 0);

		// Set all the default values needed.
		if (cafile.empty())
			cafile = "ca.pem";

		if (certfile.empty())
			certfile = "cert.pem";

		if (keyfile.empty())
			keyfile = "key.pem";

		if (dhfile.empty())
			dhfile = "dhparams.pem";

		// Prepend relative paths with the path to the config directory.
		if ((cafile[0] != '/') && (!ServerInstance->Config->StartsWithWindowsDriveLetter(cafile)))
			cafile = confdir + cafile;

		if ((certfile[0] != '/') && (!ServerInstance->Config->StartsWithWindowsDriveLetter(certfile)))
			certfile = confdir + certfile;

		if ((keyfile[0] != '/') && (!ServerInstance->Config->StartsWithWindowsDriveLetter(keyfile)))
			keyfile = confdir + keyfile;

		if ((dhfile[0] != '/') && (!ServerInstance->Config->StartsWithWindowsDriveLetter(dhfile)))
			dhfile = confdir + dhfile;

		/* Load our keys and certificates
		 * NOTE: OpenSSL's error logging API sucks, don't blame us for this clusterfuck.
		 */
		if ((!SSL_CTX_use_certificate_chain_file(ctx, certfile.c_str())) || (!SSL_CTX_use_certificate_chain_file(clictx, certfile.c_str())))
		{
			ServerInstance->Logs->Log("m_ssl_openssl",DEFAULT, "m_ssl_openssl.so: Can't read certificate file %s. %s", certfile.c_str(), strerror(errno));
			ERR_print_errors_cb(error_callback, this);
		}

		if (((!SSL_CTX_use_PrivateKey_file(ctx, keyfile.c_str(), SSL_FILETYPE_PEM))) || (!SSL_CTX_use_PrivateKey_file(clictx, keyfile.c_str(), SSL_FILETYPE_PEM)))
		{
			ServerInstance->Logs->Log("m_ssl_openssl",DEFAULT, "m_ssl_openssl.so: Can't read key file %s. %s", keyfile.c_str(), strerror(errno));
			ERR_print_errors_cb(error_callback, this);
		}

		/* Load the CAs we trust*/
		if (((!SSL_CTX_load_verify_locations(ctx, cafile.c_str(), 0))) || (!SSL_CTX_load_verify_locations(clictx, cafile.c_str(), 0)))
		{
			ServerInstance->Logs->Log("m_ssl_openssl",DEFAULT, "m_ssl_openssl.so: Can't read CA list from %s. %s", cafile.c_str(), strerror(errno));
			ERR_print_errors_cb(error_callback, this);
		}

		FILE* dhpfile = fopen(dhfile.c_str(), "r");
		DH* ret;

		if (dhpfile == NULL)
		{
			ServerInstance->Logs->Log("m_ssl_openssl",DEFAULT, "m_ssl_openssl.so Couldn't open DH file %s: %s", dhfile.c_str(), strerror(errno));
			throw ModuleException("Couldn't open DH file " + dhfile + ": " + strerror(errno));
		}
		else
		{
			ret = PEM_read_DHparams(dhpfile, NULL, NULL, NULL);
			if ((SSL_CTX_set_tmp_dh(ctx, ret) < 0) || (SSL_CTX_set_tmp_dh(clictx, ret) < 0))
			{
				ServerInstance->Logs->Log("m_ssl_openssl",DEFAULT, "m_ssl_openssl.so: Couldn't set DH parameters %s. SSL errors follow:", dhfile.c_str());
				ERR_print_errors_cb(error_callback, this);
			}
		}

		fclose(dhpfile);
	}

	virtual void On005Numeric(std::string &output)
	{
		output.append(" SSL=" + sslports);
	}

	virtual ~ModuleSSLOpenSSL()
	{
		SSL_CTX_free(ctx);
		SSL_CTX_free(clictx);
		ServerInstance->Modules->UnpublishInterface("BufferedSocketHook", this);
		delete[] sessions;
	}

	virtual void OnCleanup(int target_type, void* item)
	{
		if (target_type == TYPE_USER)
		{
			User* user = (User*)item;

			if (user->io == this)
			{
				// User is using SSL, they're a local user, and they're using one of *our* SSL ports.
				// Potentially there could be multiple SSL modules loaded at once on different ports.
				ServerInstance->Users->QuitUser(user, "SSL module unloading");
				user->io = NULL;
			}
			if (user->GetExt("ssl_cert", dummy))
			{
				ssl_cert* tofree;
				user->GetExt("ssl_cert", tofree);
				delete tofree;
				user->Shrink("ssl_cert");
			}
		}
	}

	virtual void OnUnloadModule(Module* mod, const std::string &name)
	{
		if (mod == this)
		{
			for(unsigned int i = 0; i < listenports.size(); i++)
			{
				for (size_t j = 0; j < ServerInstance->Config->ports.size(); j++)
					if (listenports[i] == (ServerInstance->Config->ports[j]->GetIP()+":"+ConvToStr(ServerInstance->Config->ports[j]->GetPort())))
						ServerInstance->Config->ports[j]->SetDescription("plaintext");
			}
		}
	}

	virtual Version GetVersion()
	{
		return Version("$Id$", VF_VENDOR, API_VERSION);
	}


	virtual const char* OnRequest(Request* request)
	{
		ISHRequest* ISR = (ISHRequest*)request;
		if (strcmp("IS_NAME", request->GetId()) == 0)
		{
			return "openssl";
		}
		else if (strcmp("IS_HOOK", request->GetId()) == 0)
		{
			const char* ret = "OK";
			try
			{
				ret = ServerInstance->Config->AddIOHook((Module*)this, (BufferedSocket*)ISR->Sock) ? "OK" : NULL;
			}
			catch (ModuleException &e)
			{
				return NULL;
			}

			return ret;
		}
		else if (strcmp("IS_UNHOOK", request->GetId()) == 0)
		{
			return ServerInstance->Config->DelIOHook((BufferedSocket*)ISR->Sock) ? "OK" : NULL;
		}
		else if (strcmp("IS_HSDONE", request->GetId()) == 0)
		{
			if (ISR->Sock->GetFd() < 0)
				return "OK";

			issl_session* session = &sessions[ISR->Sock->GetFd()];
			return (session->status == ISSL_HANDSHAKING) ? NULL : "OK";
		}
		else if (strcmp("IS_ATTACH", request->GetId()) == 0)
		{
			issl_session* session = &sessions[ISR->Sock->GetFd()];
			if (session->sess)
			{
				VerifyCertificate(session, (BufferedSocket*)ISR->Sock);
				return "OK";
			}
		}
		return NULL;
	}


	virtual void OnRawSocketAccept(int fd, const std::string &ip, int localport)
	{
		/* Are there any possibilities of an out of range fd? Hope not, but lets be paranoid */
		if ((fd < 0) || (fd > ServerInstance->SE->GetMaxFds() - 1))
			return;

		issl_session* session = &sessions[fd];

		session->fd = fd;
		session->inbuf = new char[inbufsize];
		session->inbufoffset = 0;
		session->sess = SSL_new(ctx);
		session->status = ISSL_NONE;
		session->outbound = false;

		if (session->sess == NULL)
			return;

		if (SSL_set_fd(session->sess, fd) == 0)
		{
			ServerInstance->Logs->Log("m_ssl_openssl",DEBUG,"BUG: Can't set fd with SSL_set_fd: %d", fd);
			return;
		}

 		Handshake(session);
	}

	virtual void OnRawSocketConnect(int fd)
	{
		/* Are there any possibilities of an out of range fd? Hope not, but lets be paranoid */
		if ((fd < 0) || (fd > ServerInstance->SE->GetMaxFds() -1))
			return;

		issl_session* session = &sessions[fd];

		session->fd = fd;
		session->inbuf = new char[inbufsize];
		session->inbufoffset = 0;
		session->sess = SSL_new(clictx);
		session->status = ISSL_NONE;
		session->outbound = true;

		if (session->sess == NULL)
			return;

		if (SSL_set_fd(session->sess, fd) == 0)
		{
			ServerInstance->Logs->Log("m_ssl_openssl",DEBUG,"BUG: Can't set fd with SSL_set_fd: %d", fd);
			return;
		}

		Handshake(session);
	}

	virtual void OnRawSocketClose(int fd)
	{
		/* Are there any possibilities of an out of range fd? Hope not, but lets be paranoid */
		if ((fd < 0) || (fd > ServerInstance->SE->GetMaxFds() - 1))
			return;

		CloseSession(&sessions[fd]);

		EventHandler* user = ServerInstance->SE->GetRef(fd);

		if ((user) && (user->GetExt("ssl_cert", dummy)))
		{
			ssl_cert* tofree;
			user->GetExt("ssl_cert", tofree);
			delete tofree;
			user->Shrink("ssl_cert");
		}
	}

	virtual int OnRawSocketRead(int fd, char* buffer, unsigned int count, int &readresult)
	{
		/* Are there any possibilities of an out of range fd? Hope not, but lets be paranoid */
		if ((fd < 0) || (fd > ServerInstance->SE->GetMaxFds() - 1))
			return 0;

		issl_session* session = &sessions[fd];

		if (!session->sess)
		{
			readresult = 0;
			CloseSession(session);
			return 1;
		}

		if (session->status == ISSL_HANDSHAKING)
		{
			if (session->rstat == ISSL_READ || session->wstat == ISSL_READ)
			{
				// The handshake isn't finished and it wants to read, try to finish it.
				if (!Handshake(session))
				{
					// Couldn't resume handshake.
					return -1;
				}
			}
			else
			{
				errno = EAGAIN;
				return -1;
			}
		}

		// If we resumed the handshake then session->status will be ISSL_OPEN

		if (session->status == ISSL_OPEN)
		{
			if (session->wstat == ISSL_READ)
			{
				if(DoWrite(session) == 0)
					return 0;
			}

			if (session->rstat == ISSL_READ)
			{
				int ret = DoRead(session);

				if (ret > 0)
				{
					if (count <= session->inbufoffset)
					{
						memcpy(buffer, session->inbuf, count);
						// Move the stuff left in inbuf to the beginning of it
						memmove(session->inbuf, session->inbuf + count, (session->inbufoffset - count));
						// Now we need to set session->inbufoffset to the amount of data still waiting to be handed to insp.
						session->inbufoffset -= count;
						// Insp uses readresult as the count of how much data there is in buffer, so:
						readresult = count;
					}
					else
					{
						// There's not as much in the inbuf as there is space in the buffer, so just copy the whole thing.
						memcpy(buffer, session->inbuf, session->inbufoffset);

						readresult = session->inbufoffset;
						// Zero the offset, as there's nothing there..
						session->inbufoffset = 0;
					}
					return 1;
				}
				return ret;
			}
		}

		return -1;
	}

	virtual int OnRawSocketWrite(int fd, const char* buffer, int count)
	{
		/* Are there any possibilities of an out of range fd? Hope not, but lets be paranoid */
		if ((fd < 0) || (fd > ServerInstance->SE->GetMaxFds() - 1))
			return 0;

		issl_session* session = &sessions[fd];

		if (!session->sess)
		{
			CloseSession(session);
			return -1;
		}

		session->outbuf.append(buffer, count);
		MakePollWrite(session);

		if (session->status == ISSL_HANDSHAKING)
		{
			// The handshake isn't finished, try to finish it.
			if (session->rstat == ISSL_WRITE || session->wstat == ISSL_WRITE)
			{
				Handshake(session);
			}
		}

		if (session->status == ISSL_OPEN)
		{
			if (session->rstat == ISSL_WRITE)
			{
				DoRead(session);
			}

			if (session->wstat == ISSL_WRITE)
			{
				return DoWrite(session);
			}
		}

		return 1;
	}

	int DoWrite(issl_session* session)
	{
		if (!session->outbuf.size())
			return -1;

		int ret = SSL_write(session->sess, session->outbuf.data(), session->outbuf.size());

		if (ret == 0)
		{
			CloseSession(session);
			return 0;
		}
		else if (ret < 0)
		{
			int err = SSL_get_error(session->sess, ret);

			if (err == SSL_ERROR_WANT_WRITE)
			{
				session->wstat = ISSL_WRITE;
				return -1;
			}
			else if (err == SSL_ERROR_WANT_READ)
			{
				session->wstat = ISSL_READ;
				return -1;
			}
			else
			{
				CloseSession(session);
				return 0;
			}
		}
		else
		{
			session->outbuf = session->outbuf.substr(ret);
			return ret;
		}
	}

	int DoRead(issl_session* session)
	{
		// Is this right? Not sure if the unencrypted data is garaunteed to be the same length.
		// Read into the inbuffer, offset from the beginning by the amount of data we have that insp hasn't taken yet.

		int ret = SSL_read(session->sess, session->inbuf + session->inbufoffset, inbufsize - session->inbufoffset);

		if (ret == 0)
		{
			// Client closed connection.
			CloseSession(session);
			return 0;
		}
		else if (ret < 0)
		{
			int err = SSL_get_error(session->sess, ret);

			if (err == SSL_ERROR_WANT_READ)
			{
				session->rstat = ISSL_READ;
				return -1;
			}
			else if (err == SSL_ERROR_WANT_WRITE)
			{
				session->rstat = ISSL_WRITE;
				MakePollWrite(session);
				return -1;
			}
			else
			{
				CloseSession(session);
				return 0;
			}
		}
		else
		{
			// Read successfully 'ret' bytes into inbuf + inbufoffset
			// There are 'ret' + 'inbufoffset' bytes of data in 'inbuf'
			// 'buffer' is 'count' long

			session->inbufoffset += ret;

			return ret;
		}
	}

	// :kenny.chatspike.net 320 Om Epy|AFK :is a Secure Connection
	virtual void OnWhois(User* source, User* dest)
	{
		if (!clientactive)
			return;

		// Bugfix, only send this numeric for *our* SSL users
		if (dest->GetExt("ssl", dummy) || ((IS_LOCAL(dest) && dest->io == this)))
		{
			ServerInstance->SendWhoisLine(source, dest, 320, "%s %s :is using a secure connection", source->nick.c_str(), dest->nick.c_str());
		}
	}

	virtual void OnSyncUserMetaData(User* user, Module* proto, void* opaque, const std::string &extname, bool displayable)
	{
		// check if the linking module wants to know about OUR metadata
		if (extname == "ssl")
		{
			// check if this user has an swhois field to send
			if(user->GetExt(extname, dummy))
			{
				// call this function in the linking module, let it format the data how it
				// sees fit, and send it on its way. We dont need or want to know how.
				proto->ProtoSendMetaData(opaque, TYPE_USER, user, extname, displayable ? "Enabled" : "ON");
			}
		}
	}

	virtual void OnDecodeMetaData(int target_type, void* target, const std::string &extname, const std::string &extdata)
	{
		// check if its our metadata key, and its associated with a user
		if ((target_type == TYPE_USER) && (extname == "ssl"))
		{
			User* dest = (User*)target;
			// if they dont already have an ssl flag, accept the remote server's
			if (!dest->GetExt(extname, dummy))
			{
				dest->Extend(extname, "ON");
			}
		}
	}

	bool Handshake(issl_session* session)
	{
		int ret;

		if (session->outbound)
			ret = SSL_connect(session->sess);
		else
			ret = SSL_accept(session->sess);

		if (ret < 0)
		{
			int err = SSL_get_error(session->sess, ret);

			if (err == SSL_ERROR_WANT_READ)
			{
				session->rstat = ISSL_READ;
				session->status = ISSL_HANDSHAKING;
				return true;
			}
			else if (err == SSL_ERROR_WANT_WRITE)
			{
				session->wstat = ISSL_WRITE;
				session->status = ISSL_HANDSHAKING;
				MakePollWrite(session);
				return true;
			}
			else
			{
				CloseSession(session);
			}

			return false;
		}
		else if (ret > 0)
		{
			// Handshake complete.
			// This will do for setting the ssl flag...it could be done earlier if it's needed. But this seems neater.
			User* u = ServerInstance->FindDescriptor(session->fd);
			if (u)
			{
				if (!u->GetExt("ssl", dummy))
					u->Extend("ssl", "ON");
			}

			session->status = ISSL_OPEN;

			MakePollWrite(session);

			return true;
		}
		else if (ret == 0)
		{
			CloseSession(session);
			return true;
		}

		return true;
	}

	virtual void OnPostConnect(User* user)
	{
		// This occurs AFTER OnUserConnect so we can be sure the
		// protocol module has propagated the NICK message.
		if ((user->GetExt("ssl", dummy)) && (IS_LOCAL(user)))
		{
			// Tell whatever protocol module we're using that we need to inform other servers of this metadata NOW.
			ServerInstance->PI->SendMetaData(user, TYPE_USER, "SSL", "on");

			VerifyCertificate(&sessions[user->GetFd()], user);
			if (sessions[user->GetFd()].sess)
				user->WriteServ("NOTICE %s :*** You are connected using SSL cipher \"%s\"", user->nick.c_str(), SSL_get_cipher(sessions[user->GetFd()].sess));
		}
	}

	void MakePollWrite(issl_session* session)
	{
		//OnRawSocketWrite(session->fd, NULL, 0);
		EventHandler* eh = ServerInstance->FindDescriptor(session->fd);
		if (eh)
		{
			ServerInstance->SE->WantWrite(eh);
		}
	}

	virtual void OnBufferFlushed(User* user)
	{
		if (user->GetExt("ssl"))
		{
			issl_session* session = &sessions[user->GetFd()];
			if (session && session->outbuf.size())
				OnRawSocketWrite(user->GetFd(), NULL, 0);
		}
	}

	void CloseSession(issl_session* session)
	{
		if (session->sess)
		{
			SSL_shutdown(session->sess);
			SSL_free(session->sess);
		}

		if (session->inbuf)
		{
			delete[] session->inbuf;
		}

		session->outbuf.clear();
		session->inbuf = NULL;
		session->sess = NULL;
		session->status = ISSL_NONE;
	}

	void VerifyCertificate(issl_session* session, Extensible* user)
	{
		if (!session->sess || !user)
			return;

		X509* cert;
		ssl_cert* certinfo = new ssl_cert;
		unsigned int n;
		unsigned char md[EVP_MAX_MD_SIZE];
		const EVP_MD *digest = EVP_md5();

		user->Extend("ssl_cert",certinfo);

		cert = SSL_get_peer_certificate((SSL*)session->sess);

		if (!cert)
		{
			certinfo->data.insert(std::make_pair("error","Could not get peer certificate: "+std::string(get_error())));
			return;
		}

		certinfo->data.insert(std::make_pair("invalid", SSL_get_verify_result(session->sess) != X509_V_OK ? ConvToStr(1) : ConvToStr(0)));

		if (SelfSigned)
		{
			certinfo->data.insert(std::make_pair("unknownsigner",ConvToStr(0)));
			certinfo->data.insert(std::make_pair("trusted",ConvToStr(1)));
		}
		else
		{
			certinfo->data.insert(std::make_pair("unknownsigner",ConvToStr(1)));
			certinfo->data.insert(std::make_pair("trusted",ConvToStr(0)));
		}

		certinfo->data.insert(std::make_pair("dn",std::string(X509_NAME_oneline(X509_get_subject_name(cert),0,0))));
		certinfo->data.insert(std::make_pair("issuer",std::string(X509_NAME_oneline(X509_get_issuer_name(cert),0,0))));

		if (!X509_digest(cert, digest, md, &n))
		{
			certinfo->data.insert(std::make_pair("error","Out of memory generating fingerprint"));
		}
		else
		{
			certinfo->data.insert(std::make_pair("fingerprint",irc::hex(md, n)));
		}

		if ((ASN1_UTCTIME_cmp_time_t(X509_get_notAfter(cert), time(NULL)) == -1) || (ASN1_UTCTIME_cmp_time_t(X509_get_notBefore(cert), time(NULL)) == 0))
		{
			certinfo->data.insert(std::make_pair("error","Not activated, or expired certificate"));
		}

		X509_free(cert);
	}

	void Prioritize()
	{
		Module* server = ServerInstance->Modules->Find("m_spanningtree.so");
		ServerInstance->Modules->SetPriority(this, I_OnPostConnect, PRIO_AFTER, &server);
	}

};

static int error_callback(const char *str, size_t len, void *u)
{
	ModuleSSLOpenSSL* mssl = (ModuleSSLOpenSSL*)u;
	mssl->PublicInstance->Logs->Log("m_ssl_openssl",DEFAULT, "SSL error: " + std::string(str, len - 1));

	//
	// XXX: Remove this line, it causes valgrind warnings...
	//
	// MD_update(&m, buf, j);
	//
	//
	// ... ONLY JOKING! :-)
	//

	return 0;
}

MODULE_INIT(ModuleSSLOpenSSL)
