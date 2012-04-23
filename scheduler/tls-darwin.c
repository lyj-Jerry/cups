/*
 * "$Id$"
 *
 *   TLS support code for the CUPS scheduler on OS X.
 *
 *   Copyright 2007-2012 by Apple Inc.
 *   Copyright 1997-2007 by Easy Software Products, all rights reserved.
 *
 *   These coded instructions, statements, and computer programs are the
 *   property of Apple Inc. and are protected by Federal copyright
 *   law.  Distribution and use rights are outlined in the file "LICENSE.txt"
 *   which should have been included with this file.  If this file is
 *   file is missing or damaged, see the license at "http://www.cups.org/".
 *
 * Contents:
 *
 *   cupsdEndTLS()	     - Shutdown a secure session with the client.
 *   cupsdStartTLS()	     - Start a secure session with the client.
 *   copy_cdsa_certificate() - Copy a SSL/TLS certificate from the System
 *			       keychain.
 *   make_certificate()      - Make a self-signed SSL/TLS certificate.
 */


/*
 * Local functions...
 */

static CFArrayRef	copy_cdsa_certificate(cupsd_client_t *con);
static int		make_certificate(cupsd_client_t *con);


/*
 * 'cupsdEndTLS()' - Shutdown a secure session with the client.
 */

int					/* O - 1 on success, 0 on error */
cupsdEndTLS(cupsd_client_t *con)	/* I - Client connection */
{
  while (SSLClose(con->http.tls) == errSSLWouldBlock)
    usleep(1000);

  SSLDisposeContext(con->http.tls);
  con->http.tls = NULL;

  if (con->http.tls_credentials)
    CFRelease(con->http.tls_credentials);

  return (1);
}


/*
 * 'cupsdStartTLS()' - Start a secure session with the client.
 */

int					/* O - 1 on success, 0 on error */
cupsdStartTLS(cupsd_client_t *con)	/* I - Client connection */
{
  OSStatus	error = 0;		/* Error code */
  CFArrayRef	peerCerts;		/* Peer certificates */


  cupsdLogMessage(CUPSD_LOG_DEBUG, "[Client %d] Encrypting connection.",
                  con->http.fd);

  con->http.tls_credentials = copy_cdsa_certificate(con);

  if (!con->http.tls_credentials)
  {
   /*
    * No keychain (yet), make a self-signed certificate...
    */

    if (make_certificate(con))
      con->http.tls_credentials = copy_cdsa_certificate(con);
  }

  if (!con->http.tls_credentials)
  {
    cupsdLogMessage(CUPSD_LOG_ERROR,
        	    "Could not find signing key in keychain \"%s\"",
		    ServerCertificate);
    error = errSSLBadConfiguration;
  }

  if (!error)
    error = SSLNewContext(true, &con->http.tls);

  if (!error)
    error = SSLSetIOFuncs(con->http.tls, _httpReadCDSA, _httpWriteCDSA);

  if (!error)
    error = SSLSetConnection(con->http.tls, HTTP(con));

  if (!error)
    error = SSLSetAllowsExpiredCerts(con->http.tls, true);

  if (!error)
    error = SSLSetAllowsAnyRoot(con->http.tls, true);

  if (!error)
    error = SSLSetCertificate(con->http.tls, con->http.tls_credentials);

  if (!error)
  {
   /*
    * Perform SSL/TLS handshake
    */

    while ((error = SSLHandshake(con->http.tls)) == errSSLWouldBlock)
      usleep(1000);
  }

  if (error)
  {
    cupsdLogMessage(CUPSD_LOG_ERROR,
                    "Unable to encrypt connection from %s - %s (%d)",
                    con->http.hostname, cssmErrorString(error), (int)error);

    con->http.error  = error;
    con->http.status = HTTP_ERROR;

    if (con->http.tls)
    {
      SSLDisposeContext(con->http.tls);
      con->http.tls = NULL;
    }

    if (con->http.tls_credentials)
    {
      CFRelease(con->http.tls_credentials);
      con->http.tls_credentials = NULL;
    }

    return (0);
  }

  cupsdLogMessage(CUPSD_LOG_DEBUG, "Connection from %s now encrypted.",
                  con->http.hostname);

  if (!SSLCopyPeerCertificates(con->http.tls, &peerCerts) && peerCerts)
  {
    cupsdLogMessage(CUPSD_LOG_DEBUG, "Received %d peer certificates!",
		    (int)CFArrayGetCount(peerCerts));
    CFRelease(peerCerts);
  }
  else
    cupsdLogMessage(CUPSD_LOG_DEBUG, "Received NO peer certificates!");

  return (1);
}


/*
 * 'copy_cdsa_certificate()' - Copy a SSL/TLS certificate from the System
 *                             keychain.
 */

static CFArrayRef				/* O - Array of certificates */
copy_cdsa_certificate(
    cupsd_client_t *con)			/* I - Client connection */
{
  OSStatus		err;		/* Error info */
  SecKeychainRef	keychain = NULL;/* Keychain reference */
  SecIdentitySearchRef	search = NULL;	/* Search reference */
  SecIdentityRef	identity = NULL;/* Identity */
  CFArrayRef		certificates = NULL;
					/* Certificate array */
#  if HAVE_SECPOLICYCREATESSL
  SecPolicyRef		policy = NULL;	/* Policy ref */
  CFStringRef		servername = NULL;
					/* Server name */
  CFMutableDictionaryRef query = NULL;	/* Query qualifiers */
  CFArrayRef		list = NULL;	/* Keychain list */
  char			localname[1024];/* Local hostname */
#  elif defined(HAVE_SECIDENTITYSEARCHCREATEWITHPOLICY)
  SecPolicyRef		policy = NULL;	/* Policy ref */
  SecPolicySearchRef	policy_search = NULL;
					/* Policy search ref */
  CSSM_DATA		options;	/* Policy options */
  CSSM_APPLE_TP_SSL_OPTIONS
			ssl_options;	/* SSL Option for hostname */
  char			localname[1024];/* Local hostname */
#  endif /* HAVE_SECPOLICYCREATESSL */


  cupsdLogMessage(CUPSD_LOG_DEBUG,
                  "copy_cdsa_certificate: Looking for certs for \"%s\"...",
		  con->servername);

  if ((err = SecKeychainOpen(ServerCertificate, &keychain)))
  {
    cupsdLogMessage(CUPSD_LOG_ERROR, "Cannot open keychain \"%s\" - %s (%d)",
	            ServerCertificate, cssmErrorString(err), (int)err);
    goto cleanup;
  }

#  if HAVE_SECPOLICYCREATESSL
  servername = CFStringCreateWithCString(kCFAllocatorDefault, con->servername,
					 kCFStringEncodingUTF8);

  policy = SecPolicyCreateSSL(1, servername);

  if (servername)
    CFRelease(servername);

  if (!policy)
  {
    cupsdLogMessage(CUPSD_LOG_ERROR, "Cannot create ssl policy reference");
    goto cleanup;
  }

  if (!(query = CFDictionaryCreateMutable(kCFAllocatorDefault, 0,
					  &kCFTypeDictionaryKeyCallBacks,
					  &kCFTypeDictionaryValueCallBacks)))
  {
    cupsdLogMessage(CUPSD_LOG_ERROR, "Cannot create query dictionary");
    goto cleanup;
  }

  list = CFArrayCreate(kCFAllocatorDefault, (const void **)&keychain, 1,
                       &kCFTypeArrayCallBacks);

  CFDictionaryAddValue(query, kSecClass, kSecClassIdentity);
  CFDictionaryAddValue(query, kSecMatchPolicy, policy);
  CFDictionaryAddValue(query, kSecReturnRef, kCFBooleanTrue);
  CFDictionaryAddValue(query, kSecMatchLimit, kSecMatchLimitOne);
  CFDictionaryAddValue(query, kSecMatchSearchList, list);

  CFRelease(list);

  err = SecItemCopyMatching(query, (CFTypeRef *)&identity);

  if (err && DNSSDHostName)
  {
   /*
    * Search for the connection server name failed; try the DNS-SD .local
    * hostname instead...
    */

    snprintf(localname, sizeof(localname), "%s.local", DNSSDHostName);

    cupsdLogMessage(CUPSD_LOG_DEBUG,
		    "copy_cdsa_certificate: Looking for certs for \"%s\"...",
		    localname);

    servername = CFStringCreateWithCString(kCFAllocatorDefault, localname,
					   kCFStringEncodingUTF8);

    CFRelease(policy);

    policy = SecPolicyCreateSSL(1, servername);

    if (servername)
      CFRelease(servername);

    if (!policy)
    {
      cupsdLogMessage(CUPSD_LOG_ERROR, "Cannot create ssl policy reference");
      goto cleanup;
    }

    CFDictionarySetValue(query, kSecMatchPolicy, policy);

    err = SecItemCopyMatching(query, (CFTypeRef *)&identity);
  }

  if (err)
  {
    cupsdLogMessage(CUPSD_LOG_DEBUG,
		    "Cannot find signing key in keychain \"%s\": %s (%d)",
		    ServerCertificate, cssmErrorString(err), (int)err);
    goto cleanup;
  }

#  elif defined(HAVE_SECIDENTITYSEARCHCREATEWITHPOLICY)
 /*
  * Use a policy to search for valid certificates whose common name matches the
  * servername...
  */

  if (SecPolicySearchCreate(CSSM_CERT_X_509v3, &CSSMOID_APPLE_TP_SSL,
			    NULL, &policy_search))
  {
    cupsdLogMessage(CUPSD_LOG_ERROR, "Cannot create a policy search reference");
    goto cleanup;
  }

  if (SecPolicySearchCopyNext(policy_search, &policy))
  {
    cupsdLogMessage(CUPSD_LOG_ERROR,
		    "Cannot find a policy to use for searching");
    goto cleanup;
  }

  memset(&ssl_options, 0, sizeof(ssl_options));
  ssl_options.Version = CSSM_APPLE_TP_SSL_OPTS_VERSION;
  ssl_options.ServerName = con->servername;
  ssl_options.ServerNameLen = strlen(con->servername);

  options.Data = (uint8 *)&ssl_options;
  options.Length = sizeof(ssl_options);

  if (SecPolicySetValue(policy, &options))
  {
    cupsdLogMessage(CUPSD_LOG_ERROR,
		    "Cannot set policy value to use for searching");
    goto cleanup;
  }

  if ((err = SecIdentitySearchCreateWithPolicy(policy, NULL, CSSM_KEYUSE_SIGN,
					       keychain, FALSE, &search)))
  {
    cupsdLogMessage(CUPSD_LOG_ERROR,
		    "Cannot create identity search reference: %s (%d)",
		    cssmErrorString(err), (int)err);
    goto cleanup;
  }

  err = SecIdentitySearchCopyNext(search, &identity);

  if (err && DNSSDHostName)
  {
   /*
    * Search for the connection server name failed; try the DNS-SD .local
    * hostname instead...
    */

    snprintf(localname, sizeof(localname), "%s.local", DNSSDHostName);

    ssl_options.ServerName    = localname;
    ssl_options.ServerNameLen = strlen(localname);

    cupsdLogMessage(CUPSD_LOG_DEBUG,
		    "copy_cdsa_certificate: Looking for certs for \"%s\"...",
		    localname);

    if (SecPolicySetValue(policy, &options))
    {
      cupsdLogMessage(CUPSD_LOG_ERROR,
		      "Cannot set policy value to use for searching");
      goto cleanup;
    }

    CFRelease(search);
    search = NULL;
    if ((err = SecIdentitySearchCreateWithPolicy(policy, NULL, CSSM_KEYUSE_SIGN,
					       keychain, FALSE, &search)))
    {
      cupsdLogMessage(CUPSD_LOG_ERROR,
		      "Cannot create identity search reference: %s (%d)",
		      cssmErrorString(err), (int)err);
      goto cleanup;
    }

    err = SecIdentitySearchCopyNext(search, &identity);

  }

  if (err)
  {
    cupsdLogMessage(CUPSD_LOG_DEBUG,
		    "Cannot find signing key in keychain \"%s\": %s (%d)",
		    ServerCertificate, cssmErrorString(err), (int)err);
    goto cleanup;
  }

#  else
 /*
  * Assume there is exactly one SecIdentity in the keychain...
  */

  if ((err = SecIdentitySearchCreate(keychain, CSSM_KEYUSE_SIGN, &search)))
  {
    cupsdLogMessage(CUPSD_LOG_DEBUG,
		    "Cannot create identity search reference (%d)", (int)err);
    goto cleanup;
  }

  if ((err = SecIdentitySearchCopyNext(search, &identity)))
  {
    cupsdLogMessage(CUPSD_LOG_DEBUG,
		    "Cannot find signing key in keychain \"%s\": %s (%d)",
		    ServerCertificate, cssmErrorString(err), (int)err);
    goto cleanup;
  }
#  endif /* HAVE_SECPOLICYCREATESSL */

  if (CFGetTypeID(identity) != SecIdentityGetTypeID())
  {
    cupsdLogMessage(CUPSD_LOG_ERROR, "SecIdentity CFTypeID failure!");
    goto cleanup;
  }

  if ((certificates = CFArrayCreate(NULL, (const void **)&identity,
				  1, &kCFTypeArrayCallBacks)) == NULL)
  {
    cupsdLogMessage(CUPSD_LOG_ERROR, "Cannot create certificate array");
    goto cleanup;
  }

  cleanup :

  if (keychain)
    CFRelease(keychain);
  if (search)
    CFRelease(search);
  if (identity)
    CFRelease(identity);

#  if HAVE_SECPOLICYCREATESSL
  if (policy)
    CFRelease(policy);
  if (query)
    CFRelease(query);
#  elif defined(HAVE_SECIDENTITYSEARCHCREATEWITHPOLICY)
  if (policy)
    CFRelease(policy);
  if (policy_search)
    CFRelease(policy_search);
#  endif /* HAVE_SECPOLICYCREATESSL */

  return (certificates);
}


/*
 * 'make_certificate()' - Make a self-signed SSL/TLS certificate.
 */

static int				/* O - 1 on success, 0 on failure */
make_certificate(cupsd_client_t *con)	/* I - Client connection */
{
  int		pid,			/* Process ID of command */
		status;			/* Status of command */
  char		command[1024],		/* Command */
		*argv[4],		/* Command-line arguments */
		*envp[MAX_ENV + 1],	/* Environment variables */
		keychain[1024],		/* Keychain argument */
		infofile[1024],		/* Type-in information for cert */
		localname[1024],	/* Local hostname */
		*servername;		/* Name of server in cert */
  cups_file_t	*fp;			/* Seed/info file */
  int		infofd;			/* Info file descriptor */


  if (con->servername && isdigit(con->servername[0] & 255) && DNSSDHostName)
  {
    snprintf(localname, sizeof(localname), "%s.local", DNSSDHostName);
    servername = localname;
  }
  else
    servername = con->servername;

 /*
  * Run the "certtool" command to generate a self-signed certificate...
  */

  if (!cupsFileFind("certtool", getenv("PATH"), 1, command, sizeof(command)))
  {
    cupsdLogMessage(CUPSD_LOG_ERROR,
                    "No SSL certificate and certtool command not found!");
    return (0);
  }

 /*
  * Create a file with the certificate information fields...
  *
  * Note: This assumes that the default questions are asked by the certtool
  * command...
  */

  if ((fp = cupsTempFile2(infofile, sizeof(infofile))) == NULL)
  {
    cupsdLogMessage(CUPSD_LOG_ERROR,
                    "Unable to create certificate information file %s - %s",
                    infofile, strerror(errno));
    return (0);
  }

  cupsFilePrintf(fp,
                 "%s\n"			/* Enter key and certificate label */
                 "r\n"			/* Generate RSA key pair */
                 "2048\n"		/* Key size in bits */
                 "y\n"			/* OK (y = yes) */
                 "b\n"			/* Usage (b=signing/encryption) */
                 "s\n"			/* Sign with SHA1 */
                 "y\n"			/* OK (y = yes) */
                 "%s\n"			/* Common name */
                 "\n"			/* Country (default) */
                 "\n"			/* Organization (default) */
                 "\n"			/* Organizational unit (default) */
                 "\n"			/* State/Province (default) */
                 "%s\n"			/* Email address */
                 "y\n",			/* OK (y = yes) */
        	 servername, servername, ServerAdmin);
  cupsFileClose(fp);

  cupsdLogMessage(CUPSD_LOG_INFO,
                  "Generating SSL server key and certificate...");

  snprintf(keychain, sizeof(keychain), "k=%s", ServerCertificate);

  argv[0] = "certtool";
  argv[1] = "c";
  argv[2] = keychain;
  argv[3] = NULL;

  cupsdLoadEnv(envp, MAX_ENV);

  infofd = open(infofile, O_RDONLY);

  if (!cupsdStartProcess(command, argv, envp, infofd, -1, -1, -1, -1, 1, NULL,
                         NULL, &pid))
  {
    close(infofd);
    unlink(infofile);
    return (0);
  }

  close(infofd);
  unlink(infofile);

  while (waitpid(pid, &status, 0) < 0)
    if (errno != EINTR)
    {
      status = 1;
      break;
    }

  cupsdFinishProcess(pid, command, sizeof(command), NULL);

  if (status)
  {
    if (WIFEXITED(status))
      cupsdLogMessage(CUPSD_LOG_ERROR,
                      "Unable to create SSL server key and certificate - "
		      "the certtool command stopped with status %d!",
	              WEXITSTATUS(status));
    else
      cupsdLogMessage(CUPSD_LOG_ERROR,
                      "Unable to create SSL server key and certificate - "
		      "the certtool command crashed on signal %d!",
	              WTERMSIG(status));
  }
  else
  {
    cupsdLogMessage(CUPSD_LOG_INFO,
                    "Created SSL server certificate file \"%s\"...",
		    ServerCertificate);
  }

  return (!status);
}


/*
 * End of "$Id$".
 */