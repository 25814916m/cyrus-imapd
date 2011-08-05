/* httpd.c -- HTTP/WebDAV/CalDAV server protocol parsing
 *
 * Copyright (c) 1994-2011 Carnegie Mellon University.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 *
 * 3. The name "Carnegie Mellon University" must not be used to
 *    endorse or promote products derived from this software without
 *    prior written permission. For permission or any legal
 *    details, please contact
 *      Carnegie Mellon University
 *      Center for Technology Transfer and Enterprise Creation
 *      4615 Forbes Avenue
 *      Suite 302
 *      Pittsburgh, PA  15213
 *      (412) 268-7393, fax: (412) 268-7395
 *      innovation@andrew.cmu.edu
 *
 * 4. Redistributions of any form whatsoever must retain the following
 *    acknowledgment:
 *    "This product includes software developed by Computing Services
 *     at Carnegie Mellon University (http://www.cmu.edu/computing/)."
 *
 * CARNEGIE MELLON UNIVERSITY DISCLAIMS ALL WARRANTIES WITH REGARD TO
 * THIS SOFTWARE, INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY
 * AND FITNESS, IN NO EVENT SHALL CARNEGIE MELLON UNIVERSITY BE LIABLE
 * FOR ANY SPECIAL, INDIRECT OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN
 * AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING
 * OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 */
/*
 * TODO:
 *
 *   - FIX PROPFIND depth logic
 *   - Add more required properties
 *   - GET/HEAD on collections (iCalendar stream of resources)
 *   - MOVE method (Apple will use it)
 *   - calendar-query REPORT filtering (optimize for time range, component type)
 *   - free-busy-query REPORT
 *   - sync-collection REPORT (can probably use MODSEQs -- as CTag too)
 *   - Use UNIX namespace so that '.' can be used in collection names
 *   - Use XML precondition error codes
 *   - Fix handling of dead properties and unknown namespaces
 */

#include <config.h>


#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/param.h>
#include <syslog.h>
#include <netdb.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <ctype.h>
#include "prot.h"

#include <sasl/sasl.h>
#include <sasl/saslutil.h>

#include "httpd.h"
#include "dav_prop.h"
#include "caldav_db.h"

#include "assert.h"
#include "acl.h"
#include "util.h"
#include "auth.h"
#include "iptostring.h"
#include "global.h"
#include "tls.h"
#include "map.h"

#include "append.h"
#include "exitcodes.h"
#include "imapd.h"
#include "imap_err.h"
#include "http_err.h"
#include "version.h"
#include "xmalloc.h"
#include "xstrlcpy.h"
#include "xstrlcat.h"
#include "telemetry.h"
#include "backend.h"
#include "proxy.h"
#include "spool.h"
#include "userdeny.h"
#include "times.h"
#include "message.h"
#include "message_guid.h"
#include "index.h"
#include "idle.h"

#include <libical/ical.h>

#include <libxml/parser.h>
#include <libxml/tree.h>
#include <libxml/uri.h>

#ifdef HAVE_ZLIB
#include <zlib.h>
#endif /* HAVE_ZLIB */


extern int optind;
extern char *optarg;
extern int opterr;


#ifdef HAVE_SSL
static SSL *tls_conn;
#endif /* HAVE_SSL */

sasl_conn_t *httpd_saslconn; /* the sasl connection context */

int httpd_timeout;
char *httpd_userid = 0;
struct auth_state *httpd_authstate = 0;
static int httpd_userisadmin = 0;
static int httpd_userisproxyadmin = 0;
struct sockaddr_storage httpd_localaddr, httpd_remoteaddr;
int httpd_haveaddr = 0;
char httpd_clienthost[NI_MAXHOST*2+1] = "[local]";
struct protstream *httpd_out = NULL;
struct protstream *httpd_in = NULL;
struct protgroup *protin = NULL;
static int httpd_logfd = -1;

static sasl_ssf_t extprops_ssf = 0;
static int https = 0;
int httpd_tls_done = 0;
int httpd_tls_required = 0;

#define SERVERINFO_LEN	1024
static char serverinfo[SERVERINFO_LEN];

struct auth_scheme_t {
    const char *name;		/* HTTP auth scheme name */
    const char *saslmech;	/* Corresponding SASL mech name */
    unsigned is_avail;		/* Is scheme available for use? */
    unsigned need_persist;	/* Need persistent connection? */
    unsigned is_server_first;	/* Is SASL mech server-first? */
    unsigned do_base64;		/* Base64 encode/decode auth data? */
    void (*success)(const char *name,	/* Optional fn to handle success data */
		    const char *data);	/* Default is to use WWW-Auth header */
};

void digest_success(const char *name __attribute__((unused)),
		    const char *data)
{
    prot_printf(httpd_out, "Authentication-Info: %s\r\n", data);
}

/* List of HTTP auth schemes that we support */
static struct auth_scheme_t auth_schemes[] = {
    { "Basic", NULL, 0, 0, 1, 1, NULL },
    { "Digest", "DIGEST-MD5", 0, 0, 1, 0, &digest_success },
/*  { "Negotiate", "GSS-SPNEGO", 0, 0, 0, 1, NULL }, */
    { "NTLM", "NTLM", 0, 1, 0, 1, NULL },
    { NULL, NULL, 0, 0, 0, 0, NULL }
};

/* Index into available schemes */
enum {
    AUTH_BASIC = 0,
    AUTH_DIGEST,
/*  AUTH_NEGOTIATE, */
    AUTH_NTLM
};

/* Transaction flags */
enum {
    HTTP_CLOSE =	(1<<0),
    HTTP_100CONTINUE =	(1<<1),
    HTTP_CHUNKED =	(1<<2),
    HTTP_NOCACHE =	(1<<3)
};


/* Bitmask of features/methods to allow, based on URL */
enum {
    ALLOW_DAV =		(1<<0),
    ALLOW_CAL =		(1<<1),
    ALLOW_CARD =	(1<<2),
    ALLOW_WRITE =	(1<<3),
    ALLOW_ALL =		0x1f
};


/* Auth challenge context */
struct auth_challenge_t {
    struct auth_scheme_t *scheme;	/* Selected AUTH scheme */
    const char *param;	 		/* Server challenge */
};

/* Meta-data for response body (payload & representation headers) */
struct resp_body_t {
    ulong len; 		/* Content-Length   */
    const char *enc;	/* Content-Encoding */
    const char *lang;	/* Content-Language */
    const char *loc;	/* Content-Location */
    const char *type;	/* Content-Type     */
    time_t lastmod;	/* Last-Modified    */
};

/* Transaction context */
struct transaction_t {
    const char *meth;			/* Method to be performed */
    unsigned flags;			/* Flags for this txn */
    struct request_target_t req_tgt;	/* Parsed target URL */
    hdrcache_t req_hdrs;    		/* Cached HTTP headers */
    struct buf req_body;		/* Buffered request body */
    struct auth_challenge_t auth_chal;	/* Authentication challenge */
    const char *loc;	    		/* Location: of resp representation */
    const char *etag;			/* ETag: of response representation */
    const char **errstr;		/* Error string */
    struct resp_body_t resp_body;	/* Response body meta-data */
};

/* the sasl proxy policy context */
static struct proxy_context httpd_proxyctx = {
    0, 1, &httpd_authstate, &httpd_userisadmin, &httpd_userisproxyadmin
};

/* signal to config.c */
const int config_need_data = CONFIG_NEED_PARTITION_DATA;

/* current namespace */
struct namespace httpd_namespace;

/* PROXY stuff */
struct backend *backend_current = NULL;

/* end PROXY stuff */

static void starttls(int https);
void usage(void);
void shut_down(int code) __attribute__ ((noreturn));

extern void setproctitle_init(int argc, char **argv, char **envp);
extern int proc_register(const char *progname, const char *clienthost, 
			 const char *userid, const char *mailbox);
extern void proc_cleanup(void);

extern int saslserver(sasl_conn_t *conn, const char *mech,
		      const char *init_resp, const char *resp_prefix,
		      const char *continuation, const char *empty_chal,
		      struct protstream *pin, struct protstream *pout,
		      int *sasl_result, char **success_data);

/* Enable the resetting of a sasl_conn_t */
static int reset_saslconn(sasl_conn_t **conn);

static void cmdloop(void);
static int parse_target(const char *uri, struct request_target_t *tgt,
			const char **errstr);
static int read_body(struct transaction_t *txn, int dump, const char **errstr);
static void response_header(long code, struct transaction_t *txn);
static void error_response(long code, struct transaction_t *txn,
			   const char *errstr);
static void xml_response(long code, struct transaction_t *txn, xmlDocPtr xml);
static int http_auth(const char *creds, struct auth_challenge_t *chal);

static int meth_copy(struct transaction_t *txn);
static int meth_delete(struct transaction_t *txn);
static int meth_get(struct transaction_t *txn);
static int meth_mkcol(struct transaction_t *txn);
static int meth_options(struct transaction_t *txn);
static int meth_propfind(struct transaction_t *txn);
static int meth_proppatch(struct transaction_t *txn);
static int meth_put(struct transaction_t *txn);
static int meth_report(struct transaction_t *txn);


static struct 
{
    char *ipremoteport;
    char *iplocalport;
    sasl_ssf_t ssf;
    char *authid;
} saslprops = {NULL,NULL,0,NULL};

static struct sasl_callback mysasl_cb[] = {
    { SASL_CB_GETOPT, &mysasl_config, NULL },
    { SASL_CB_PROXY_POLICY, &mysasl_proxy_policy, (void*) &httpd_proxyctx },
    { SASL_CB_CANON_USER, &mysasl_canon_user, NULL },
    { SASL_CB_LIST_END, NULL, NULL }
};

static void httpd_reset(void)
{
    int bytes_in = 0;
    int bytes_out = 0;

    proc_cleanup();

    /* close backend connection */
    if (backend_current) {
	backend_disconnect(backend_current);
	free(backend_current);
	backend_current = NULL;
    }

    if (httpd_in) {
	prot_NONBLOCK(httpd_in);
	prot_fill(httpd_in);
	bytes_in = prot_bytes_in(httpd_in);
	prot_free(httpd_in);
    }

    if (httpd_out) {
	prot_flush(httpd_out);
	bytes_out = prot_bytes_out(httpd_out);
	prot_free(httpd_out);
    }

    if (config_auditlog) {
	syslog(LOG_NOTICE,
	       "auditlog: traffic sessionid=<%s> bytes_in=<%d> bytes_out=<%d>", 
	       session_id(), bytes_in, bytes_out);
    }
    
    httpd_in = httpd_out = NULL;

    if (protin) protgroup_reset(protin);

#ifdef HAVE_SSL
    if (tls_conn) {
	tls_reset_servertls(&tls_conn);
	tls_conn = NULL;
    }
#endif

    cyrus_reset_stdio();

    strcpy(httpd_clienthost, "[local]");
    if (httpd_logfd != -1) {
	close(httpd_logfd);
	httpd_logfd = -1;
    }
    if (httpd_userid != NULL) {
	free(httpd_userid);
	httpd_userid = NULL;
    }
    if (httpd_authstate) {
	auth_freestate(httpd_authstate);
	httpd_authstate = NULL;
    }
    if (httpd_saslconn) {
	sasl_dispose(&httpd_saslconn);
	httpd_saslconn = NULL;
    }
    httpd_tls_done = 0;

    if(saslprops.iplocalport) {
       free(saslprops.iplocalport);
       saslprops.iplocalport = NULL;
    }
    if(saslprops.ipremoteport) {
       free(saslprops.ipremoteport);
       saslprops.ipremoteport = NULL;
    }
    if(saslprops.authid) {
       free(saslprops.authid);
       saslprops.authid = NULL;
    }
    saslprops.ssf = 0;
}

/*
 * run once when process is forked;
 * MUST NOT exit directly; must return with non-zero error code
 */
int service_init(int argc __attribute__((unused)),
		 char **argv __attribute__((unused)),
		 char **envp __attribute__((unused)))
{
    int r, opt;

    initialize_http_error_table();

    if (geteuid() == 0) fatal("must run as the Cyrus user", EC_USAGE);
    setproctitle_init(argc, argv, envp);

    /* set signal handlers */
    signals_set_shutdown(&shut_down);
    signal(SIGPIPE, SIG_IGN);

    /* load the SASL plugins */
    global_sasl_init(1, 1, mysasl_cb);

    /* open the mboxlist, we'll need it for real work */
    mboxlist_init(0);
    mboxlist_open(NULL);

    /* open the quota db, we'll need it for expunge */
    quotadb_init(0);
    quotadb_open(NULL);

    /* open the user deny db */
    denydb_init(0);
    denydb_open(NULL);

    /* open annotations.db, we'll need it for collection properties */
    annotatemore_init(0, NULL, NULL);
    annotatemore_open(NULL);

    /* setup for sending IMAP IDLE notifications */
    idle_enabled();

    /* Set namespace */
    if ((r = mboxname_init_namespace(&httpd_namespace, 1)) != 0) {
	syslog(LOG_ERR, "%s", error_message(r));
	fatal(error_message(r), EC_CONFIG);
    }
    /* External names are in URIs (UNIX sep) */
    httpd_namespace.hier_sep = '/';

    while ((opt = getopt(argc, argv, "sp:")) != EOF) {
	switch(opt) {
	case 's': /* https (do TLS right away) */
	    https = 1;
	    if (!tls_enabled()) {
		syslog(LOG_ERR, "https: required OpenSSL options not present");
		fatal("https: required OpenSSL options not present",
		      EC_CONFIG);
	    }
	    break;

	case 'p': /* external protection */
	    extprops_ssf = atoi(optarg);
	    break;

	default:
	    usage();
	}
    }

    /* Create a protgroup for input from the client and selected backend */
    protin = protgroup_new(2);

    /* Construct serverinfo string */
    if (config_serverinfo == IMAP_ENUM_SERVERINFO_ON) {
	size_t len;

	len = snprintf(serverinfo, SERVERINFO_LEN,
		       "Cyrus/%s Cyrus-SASL/%u.%u.%u", cyrus_version(),
		       SASL_VERSION_MAJOR, SASL_VERSION_MINOR, SASL_VERSION_STEP);
#ifdef HAVE_SSL
	len += snprintf(serverinfo + len, SERVERINFO_LEN - len,
#if 1
			" OpenSSL/%s", SHLIB_VERSION_NUMBER);
#else
			" OpenSSL/%u.%u.%u%c",
			(unsigned) (OPENSSL_VERSION_NUMBER >> 28) & 0xF,
			(unsigned) (OPENSSL_VERSION_NUMBER >> 20) & 0xFF,
			(unsigned) (OPENSSL_VERSION_NUMBER >> 12) & 0xFF,
			(char) ((OPENSSL_VERSION_NUMBER >> 4) & 0xFF) + 'a' - 1);
#endif
#endif
#ifdef HAVE_ZLIB
	len += snprintf(serverinfo + len, SERVERINFO_LEN - len,
			" zlib/%s", ZLIB_VERSION);
#endif
	len += snprintf(serverinfo + len, SERVERINFO_LEN - len,
			" libxml/%s", LIBXML_DOTTED_VERSION);

	len += snprintf(serverinfo + len, SERVERINFO_LEN - len,
			" libical/%s", ICAL_VERSION);
    }

    return 0;
}


/*
 * run for each accepted connection
 */
int service_main(int argc __attribute__((unused)),
		 char **argv __attribute__((unused)),
		 char **envp __attribute__((unused)))
{
    socklen_t salen;
    char hbuf[NI_MAXHOST];
    char localip[60], remoteip[60];
    int niflags;
    sasl_security_properties_t *secprops=NULL;
    const char *mechlist, *mech;
    int mechcount = 0, schemecount;;
    size_t mechlen;
    struct auth_scheme_t *scheme;

    session_new_id();

    signals_poll();

    httpd_in = prot_new(0, 0);
    httpd_out = prot_new(1, 1);
    protgroup_insert(protin, httpd_in);

    /* Find out name of client host */
    salen = sizeof(httpd_remoteaddr);
    if (getpeername(0, (struct sockaddr *)&httpd_remoteaddr, &salen) == 0 &&
	(httpd_remoteaddr.ss_family == AF_INET ||
	 httpd_remoteaddr.ss_family == AF_INET6)) {
	if (getnameinfo((struct sockaddr *)&httpd_remoteaddr, salen,
			hbuf, sizeof(hbuf), NULL, 0, NI_NAMEREQD) == 0) {
    	    strncpy(httpd_clienthost, hbuf, sizeof(hbuf));
	    strlcat(httpd_clienthost, " ", sizeof(httpd_clienthost));
	} else {
	    httpd_clienthost[0] = '\0';
	}
	niflags = NI_NUMERICHOST;
#ifdef NI_WITHSCOPEID
	if (((struct sockaddr *)&httpd_remoteaddr)->sa_family == AF_INET6)
	    niflags |= NI_WITHSCOPEID;
#endif
	if (getnameinfo((struct sockaddr *)&httpd_remoteaddr, salen, hbuf,
			sizeof(hbuf), NULL, 0, niflags) != 0)
	    strlcpy(hbuf, "unknown", sizeof(hbuf));
	strlcat(httpd_clienthost, "[", sizeof(httpd_clienthost));
	strlcat(httpd_clienthost, hbuf, sizeof(httpd_clienthost));
	strlcat(httpd_clienthost, "]", sizeof(httpd_clienthost));
	salen = sizeof(httpd_localaddr);
	if (getsockname(0, (struct sockaddr *)&httpd_localaddr, &salen) == 0) {
	    httpd_haveaddr = 1;
	}

	/* Create pre-authentication telemetry log based on client IP */
	httpd_logfd = telemetry_log(hbuf, httpd_in, httpd_out, 0);
    }

    /* other params should be filled in */
    if (sasl_server_new("http", config_servername, NULL, NULL, NULL, NULL,
			SASL_NEED_HTTP | SASL_SUCCESS_DATA,
			&httpd_saslconn) != SASL_OK)
	fatal("SASL failed initializing: sasl_server_new()",EC_TEMPFAIL); 

    /* will always return something valid */
    secprops = mysasl_secprops(0);

    /* no HTTP clients seem to use "auth-int" */
    secprops->max_ssf = 0;				/* "auth" only */
    secprops->maxbufsize = 0;  			   	/* don't need maxbuf */
    if (sasl_setprop(httpd_saslconn, SASL_SEC_PROPS, secprops) != SASL_OK)
	fatal("Failed to set SASL property", EC_TEMPFAIL);
    if (sasl_setprop(httpd_saslconn, SASL_SSF_EXTERNAL, &extprops_ssf) != SASL_OK)
	fatal("Failed to set SASL property", EC_TEMPFAIL);
    
    if(iptostring((struct sockaddr *)&httpd_localaddr,
		  salen, localip, 60) == 0) {
	sasl_setprop(httpd_saslconn, SASL_IPLOCALPORT, localip);
	saslprops.iplocalport = xstrdup(localip);
    }
    
    if(iptostring((struct sockaddr *)&httpd_remoteaddr,
		  salen, remoteip, 60) == 0) {
	sasl_setprop(httpd_saslconn, SASL_IPREMOTEPORT, remoteip);  
	saslprops.ipremoteport = xstrdup(remoteip);
    }

    /* See which auth schemes are available to us */
    schemecount = auth_schemes[AUTH_BASIC].is_avail =
	(extprops_ssf >= 2) || config_getswitch(IMAPOPT_ALLOWPLAINTEXT);
    sasl_listmech(httpd_saslconn, NULL, NULL, " ", NULL,
		  &mechlist, NULL, &mechcount);
    for (mech = mechlist; mechcount--; mech += ++mechlen) {
	mechlen = strcspn(mech, " \0");
	for (scheme = auth_schemes; scheme->name; scheme++) {
	    if (scheme->saslmech && !strncmp(mech, scheme->saslmech, mechlen)) {
		scheme->is_avail++;
		schemecount++;
		break;
	    }
	}
    }
    httpd_tls_required = !schemecount;

    proc_register("httpd", httpd_clienthost, NULL, NULL);

    /* Set inactivity timer */
    httpd_timeout = config_getint(IMAPOPT_POPTIMEOUT);
    if (httpd_timeout < 10) httpd_timeout = 10;
    httpd_timeout *= 60;
    prot_settimeout(httpd_in, httpd_timeout);
    prot_setflushonread(httpd_in, httpd_out);

    /* we were connected on https port so we should do 
       TLS negotiation immediatly */
    if (https == 1) starttls(1);

    cmdloop();

    /* Closing connection */

    /* cleanup */
    httpd_reset();

    return 0;
}


/* Called by service API to shut down the service */
void service_abort(int error)
{
    shut_down(error);
}


void usage(void)
{
    prot_printf(httpd_out, "%s: usage: httpd [-C <alt_config>] [-s]\r\n",
		error_message(HTTP_SERVER_ERROR));
    prot_flush(httpd_out);
    exit(EC_USAGE);
}


/*
 * Cleanly shut down and exit
 */
void shut_down(int code)
{
    int bytes_in = 0;
    int bytes_out = 0;

    in_shutdown = 1;

    proc_cleanup();

    /* close backend connection */
    if (backend_current) {
	backend_disconnect(backend_current);
	free(backend_current);
    }

    mboxlist_close();
    mboxlist_done();

    quotadb_close();
    quotadb_done();

    denydb_close();
    denydb_done();

    annotatemore_close();
    annotatemore_done();

    if (httpd_in) {
	prot_NONBLOCK(httpd_in);
	prot_fill(httpd_in);
	bytes_in = prot_bytes_in(httpd_in);
	prot_free(httpd_in);
    }

    if (httpd_out) {
	prot_flush(httpd_out);
	bytes_out = prot_bytes_out(httpd_out);
	prot_free(httpd_out);
    }

    if (protin) protgroup_free(protin);

    if (config_auditlog)
	syslog(LOG_NOTICE,
	       "auditlog: traffic sessionid=<%s> bytes_in=<%d> bytes_out=<%d>", 
	       session_id(), bytes_in, bytes_out);

#ifdef HAVE_SSL
    tls_shutdown_serverengine();
#endif

    cyrus_done();

    exit(code);
}


void fatal(const char* s, int code)
{
    static int recurse_code = 0;

    if (recurse_code) {
	/* We were called recursively. Just give up */
	proc_cleanup();
	exit(recurse_code);
    }
    recurse_code = code;
    if (httpd_out) {
	prot_printf(httpd_out, "%s: Fatal error: %s\r\n",
		    error_message(HTTP_SERVER_ERROR), s);
	prot_flush(httpd_out);
    }
    syslog(LOG_ERR, "Fatal error: %s", s);
    shut_down(code);
}




#ifdef HAVE_SSL
/*  XXX  Needs clean up if we are going to support TLS upgrade (RFC 2817) */
static void starttls(int https)
{
    int result;
    int *layerp;
    sasl_ssf_t ssf;
    char *auth_id;

    /* SASL and openssl have different ideas about whether ssf is signed */
    layerp = (int *) &ssf;

    if (httpd_tls_done == 1)
    {
	prot_printf(httpd_out, "-ERR %s\r\n", 
		    "Already successfully executed STLS");
	return;
    }

    result=tls_init_serverengine("http",
				 5,        /* depth to verify */
				 !https,   /* can client auth? */
				 !https);  /* TLS only? */

    if (result == -1) {

	syslog(LOG_ERR, "[httpd] error initializing TLS");

	if (https == 0)
	    prot_printf(httpd_out, "%s: %s\r\n",
			error_message(HTTP_SERVER_ERROR),
			"Error initializing TLS");
	else
	    fatal("tls_init() failed",EC_TEMPFAIL);

	return;
    }

    if (https == 0)
    {
	prot_printf(httpd_out, "+OK %s\r\n", "Begin TLS negotiation now");
	/* must flush our buffers before starting tls */
	prot_flush(httpd_out);
    }
  
    result=tls_start_servertls(0, /* read */
			       1, /* write */
			       https ? 180 : httpd_timeout,
			       layerp,
			       &auth_id,
			       &tls_conn);

    /* if error */
    if (result==-1) {
	if (https == 0) {
	    prot_printf(httpd_out, "-ERR [SYS/PERM] TLS failed\r\n");
	    syslog(LOG_NOTICE, "[httpd] TLS failed: %s", httpd_clienthost);
	} else {
	    syslog(LOG_NOTICE, "https failed: %s", httpd_clienthost);
	    fatal("tls_start_servertls() failed", EC_TEMPFAIL);
	}
	return;
    }

    /* tell SASL about the negotiated layer */
    result = sasl_setprop(httpd_saslconn, SASL_SSF_EXTERNAL, &ssf);
    if (result != SASL_OK) {
	fatal("sasl_setprop() failed: starttls()", EC_TEMPFAIL);
    }
    saslprops.ssf = ssf;

    result = sasl_setprop(httpd_saslconn, SASL_AUTH_EXTERNAL, auth_id);
    if (result != SASL_OK) {
        fatal("sasl_setprop() failed: starttls()", EC_TEMPFAIL);
    }
    if(saslprops.authid) {
	free(saslprops.authid);
	saslprops.authid = NULL;
    }
    if(auth_id)
	saslprops.authid = xstrdup(auth_id);

    /* tell the prot layer about our new layers */
    prot_settls(httpd_in, tls_conn);
    prot_settls(httpd_out, tls_conn);

    httpd_tls_done = 1;
    httpd_tls_required = 0;

    auth_schemes[AUTH_BASIC].is_avail++;
}
#else
static void starttls(int https __attribute__((unused)))
{
    fatal("starttls() called, but no OpenSSL", EC_SOFTWARE);
}
#endif /* HAVE_SSL */


/* Reset the given sasl_conn_t to a sane state */
static int reset_saslconn(sasl_conn_t **conn) 
{
    int ret;
    sasl_security_properties_t *secprops = NULL;

    sasl_dispose(conn);
    /* do initialization typical of service_main */
    ret = sasl_server_new("http", config_servername, NULL, NULL, NULL, NULL,
			  SASL_NEED_HTTP | SASL_SUCCESS_DATA, conn);
    if(ret != SASL_OK) return ret;

    if(saslprops.ipremoteport)
       ret = sasl_setprop(*conn, SASL_IPREMOTEPORT,
                          saslprops.ipremoteport);
    if(ret != SASL_OK) return ret;
    
    if(saslprops.iplocalport)
       ret = sasl_setprop(*conn, SASL_IPLOCALPORT,
                          saslprops.iplocalport);
    if(ret != SASL_OK) return ret;
    secprops = mysasl_secprops(0);

    /* no HTTP clients seem to use "auth-int" */
    secprops->max_ssf = 0;				/* "auth" only */
    secprops->maxbufsize = 0;  			   	/* don't need maxbuf */
    ret = sasl_setprop(*conn, SASL_SEC_PROPS, secprops);
    if(ret != SASL_OK) return ret;
    /* end of service_main initialization excepting SSF */

    /* If we have TLS/SSL info, set it */
    if(saslprops.ssf) {
	ret = sasl_setprop(*conn, SASL_SSF_EXTERNAL, &saslprops.ssf);
    } else {
	ret = sasl_setprop(*conn, SASL_SSF_EXTERNAL, &extprops_ssf);
    }

    if(ret != SASL_OK) return ret;

    if(saslprops.authid) {
       ret = sasl_setprop(*conn, SASL_AUTH_EXTERNAL, saslprops.authid);
       if(ret != SASL_OK) return ret;
    }
    /* End TLS/SSL Info */

    return SASL_OK;
}


struct method_t {
    const char *name;				/* Method name */
    int (*proc)(struct transaction_t *txn);	/* Function to perform method */
};

/* List of HTTP methods that we support */
static struct method_t methods[] = {
/*    { "ACL", meth_acl },*/
/*    { "COPY", meth_copy },*/
    { "DELETE", meth_delete },
    { "GET", meth_get },
    { "HEAD", meth_get },
/*    { "LOCK", meth_lock },*/
    { "MKCALENDAR", meth_mkcol },
    { "MKCOL", meth_mkcol },
/*    { "MOVE", meth_copy },*/
    { "OPTIONS", meth_options },
    { "POST", meth_put },
    { "PROPFIND", meth_propfind },
    { "PROPPATCH", meth_proppatch },
    { "PUT", meth_put },
    { "REPORT", meth_report },
/*    { "UNLOCK", meth_unlock }*/
    { NULL, NULL }
};


/*
 * Top-level command loop parsing
 */
static void cmdloop(void)
{
    int c, ret, r;
    int allowanonymous = config_getswitch(IMAPOPT_ALLOWANONYMOUSLOGIN);
    char reqline[4096], buf[1024], *uristr, *p;
    const char **hdr, *errstr;
    struct transaction_t txn;
    sasl_http_request_t sasl_http_req;
    struct method_t *method = NULL;

    /* Start with an empty (clean) transaction */
    memset(&txn, 0, sizeof(struct transaction_t));

    for (;;) {
	/* Reset state */
	ret = 0;
	errstr = NULL;
	txn.flags = 0;
	txn.auth_chal.param = NULL;
	txn.loc = txn.etag = NULL;
	txn.errstr = NULL;
	memset(&txn.resp_body, 0, sizeof(struct resp_body_t));

	/* Flush any buffered output */
	prot_flush(httpd_out);
	if (backend_current) prot_flush(backend_current->out);

	/* Check for shutdown file */
	if (shutdown_file(buf, sizeof(buf)) ||
	    (httpd_userid &&
	     userdeny(httpd_userid, config_ident, buf, sizeof(buf)))) {
	    txn.flags |= HTTP_CLOSE;
	    error_response(HTTP_UNAVAILABLE, &txn, buf);
	    shut_down(0);
	}

	signals_poll();

	if (!proxy_check_input(protin, httpd_in, httpd_out,
			       backend_current ? backend_current->in : NULL,
			       NULL, 0)) {
	    /* No input from client */
	    continue;
	}

	/* Read Request-Line */
	syslog(LOG_DEBUG, "read Request-Line");
	if (!prot_fgets(reqline, sizeof(reqline), httpd_in)) {
	    errstr = prot_error(httpd_in);
	    if (errstr && strcmp(errstr, PROT_EOF_STRING)) {
		syslog(LOG_WARNING, "%s, closing connection", errstr);
		ret = HTTP_BAD_REQUEST;
		txn.flags |= HTTP_CLOSE;
		goto error;
	    }
	    return;  /* client closed connection */
	}
	syslog(LOG_DEBUG, "%s", reqline);

	/* XXX  TODO: Check for over-length Request-Line */

	/* Trim CRLF */
	p = reqline + strlen(reqline);
	if (p > reqline && p[-1] == '\n') *--p = '\0';
	if (p > reqline && p[-1] == '\r') *--p = '\0';

	/* Parse request into Method SP request-target SP HTTP-Version */
	txn.meth = reqline;
	uristr = NULL;
	for (p = reqline; *p && !Uisspace(*p); p++);  /* find end of method */
	if (*p) {
	    *p++ = '\0';
	    uristr = p;

	    for (; *p && !Uisspace(*p); p++);  /* find end of URL */
	    if (*p) *p++ = '\0';
	}
	if (!*p) {
	    /* Missing request-target or HTTP-Version */
	    ret = HTTP_BAD_REQUEST;
	    if (!uristr) errstr = "Missing request-target";
	    else errstr = "Missing HTTP-Version";
	}

	/* Check HTTP-Version */
	if (!ret && strcmp(p, HTTP_VERSION)) {
	    ret = HTTP_BAD_VERSION;
	    snprintf(buf, sizeof(buf),
		     "This server only speaks %s", HTTP_VERSION);
	    errstr = buf;
	}

	/* Check Method */
	if (!ret) {
	    /* Quick filter based on first letter */
	    if (!strchr("DGHMOPR", txn.meth[0])) ret = HTTP_NOT_ALLOWED;
	    else {
		/* Check our list of supported methods */
		for (method = methods;
		     method->name && strcmp(method->name, txn.meth); method++);
		if (!method->name) ret = HTTP_NOT_ALLOWED;
	    }
	}

	/* Parse request-target URL */
	if (!ret && (r = parse_target(uristr, &txn.req_tgt, &errstr))) {
	    ret = r;
	}

	/* Read and parse headers */
	syslog(LOG_DEBUG, "read & parse headers");
	if (!(txn.req_hdrs = spool_new_hdrcache())) {
	    ret = HTTP_SERVER_ERROR;
	    txn.flags |= HTTP_CLOSE;
	    errstr = "Unable to create header cache";
	    goto error;
	}
	if ((r = spool_fill_hdrcache(httpd_in, NULL, txn.req_hdrs, NULL))) {
	    ret = HTTP_BAD_REQUEST;
	    errstr = "Request contains invalid header";
	}

	/* Read CRLF separating headers and body */
	c = prot_getc(httpd_in);
	if (c == '\r') c = prot_getc(httpd_in);
	if (c != '\n') {
	    ret = HTTP_BAD_REQUEST;
	    txn.flags |= HTTP_CLOSE;
	    errstr = "Missing separator between headers and body";
	    goto error;
	}

	/* Check if this is a non-persistent connection */
	if ((hdr = spool_getheader(txn.req_hdrs, "Connection")) &&
	    !strcmp(hdr[0], "close")) {
	    syslog(LOG_DEBUG, "non-persistent connection");
	    txn.flags |= HTTP_CLOSE;
	}

	/* Check client expectations */
	if ((hdr = spool_getheader(txn.req_hdrs, "Expect"))) {
	    if (!strcasecmp(hdr[0], "100-continue"))
		txn.flags |= HTTP_100CONTINUE;
	    else {
		ret = HTTP_EXPECT_FAILED;
		errstr = "Unsupported Expect";
	    }
	}

	/* Check for mandatory Host header */
	if (!ret && !(hdr = spool_getheader(txn.req_hdrs, "Host"))) {
	    ret = HTTP_BAD_REQUEST;
	    errstr = "Missing Host header";
	}

	/* Read the body, if present */
	syslog(LOG_DEBUG, "read body: %d", ret);
	if ((r = read_body(&txn, ret, &errstr))) {
	    ret = r;
	    txn.flags |= HTTP_CLOSE;
	}

	/* Handle CalDAV bootstrapping */
	if (!strcmp(txn.req_tgt.path, "/.well-known/caldav")) {
	    ret = HTTP_TEMP_REDIRECT;

	    hdr = spool_getheader(txn.req_hdrs, "Host");
	    snprintf(buf, sizeof(buf), "http://%s/calendars/", hdr[0]);
	    txn.loc = buf;
	}

	if (ret) goto error;

	/* Setup SASL HTTP request in case we need it */
	sasl_http_req.method = txn.meth;
	sasl_http_req.uri = uristr;
	sasl_http_req.entity = (u_char *) buf_cstring(&txn.req_body);
	sasl_http_req.elen = buf_len(&txn.req_body);
	sasl_http_req.non_persist = (txn.flags & HTTP_CLOSE);

	if (!httpd_userid && !allowanonymous) {
	    /* Perform authentication, if necessary */
	    if ((hdr = spool_getheader(txn.req_hdrs, "Authorization"))) {

		/* Client is trying to authenticate */
		syslog(LOG_DEBUG, "http_auth(%s)", hdr[0]);
#if 0
		if (httpd_userid) {
		    /* Reauthentication - reinitialize */
		    syslog(LOG_DEBUG, "reauth - reinit");
		    free(httpd_userid);
		    httpd_userid = NULL;
		    reset_saslconn(&httpd_saslconn);
		    txn.auth_chal.scheme = NULL;
		}
#endif
		/* Check the auth credentials */
		sasl_setprop(httpd_saslconn, SASL_HTTP_REQUEST, &sasl_http_req);
		if (http_auth(hdr[0], &txn.auth_chal) < 0) {
		    /* Auth failed - reinitialize */
		    syslog(LOG_DEBUG, "auth failed - reinit");
		    reset_saslconn(&httpd_saslconn);
		    txn.auth_chal.scheme = NULL;
		}
	    }
	}
	else if (!httpd_userid && !allowanonymous && txn.auth_chal.scheme) {
	    /* Started auth exchange, but client didn't engage - reinit */
	    syslog(LOG_DEBUG, "client didn't complete auth - reinit");
	    reset_saslconn(&httpd_saslconn);
	    txn.auth_chal.scheme = NULL;
	}

	/* Request authentication, if necessary */
	if (!httpd_userid && !allowanonymous) {
	    /* User must authenticate */

	    if (httpd_tls_required) {
		/* Redirect the client to https (we only support TLS+Basic) */
		ret = HTTP_TEMP_REDIRECT;
		errstr = "SSL/TLS required to use Basic authentication";

		hdr = spool_getheader(txn.req_hdrs, "Host");
		snprintf(buf, sizeof(buf),
			 "https://%s%s", hdr[0], txn.req_tgt.path);
		txn.loc = buf;
	    }
	    else {
		/* Tell client to authenticate */
		sasl_setprop(httpd_saslconn, SASL_HTTP_REQUEST, &sasl_http_req);
		ret = HTTP_UNAUTHORIZED;
		errstr = "Must authenticate to access the specified target";
	    }
	}

	/* Process the requested method */
	if (!ret) {
	    txn.errstr = &errstr;
	    ret = method->proc(&txn);
	}

	/* Handle errors (success responses handled by method functions */
      error:
	if (ret) {
	    syslog(LOG_DEBUG, "error: %s", error_message(ret));
	    error_response(ret, &txn, errstr);
	}

	/* Memory cleanup */
	if (txn.req_hdrs) spool_free_hdrcache(txn.req_hdrs);
	if (txn.flags & HTTP_CLOSE) {
	    buf_free(&txn.req_body);
	    return;
	}

	continue;
    }
}

/****************************  Parsing Routines  ******************************/


/* Parse request-target into dissected path */
static int parse_target(const char *uri, struct request_target_t *tgt,
			const char **errstr)
{
    xmlURIPtr p_uri;  /* parsed URI */
    char *p;
    size_t len;

    memset(tgt, 0, sizeof(struct request_target_t));
    tgt->namespace = URL_NS_DEFAULT;

    /* Parse entire URI */
    if ((p_uri = xmlParseURI(uri)) == NULL) {
	*errstr = "Illegal request target URI";
	return HTTP_BAD_REQUEST;
    }

    if (p_uri->scheme) {
	/* Check sanity of scheme */
	size_t slen = strlen(p_uri->scheme);

	if ((slen > 5) ||  /* too long */
	    strncasecmp(p_uri->scheme, "http", 4) ||
	    ((slen > 4) && strcasecmp(p_uri->scheme+4, "s"))) {
	    xmlFreeURI(p_uri);
	    *errstr = "Unsupported URI scheme";
	    return HTTP_BAD_REQUEST;
	}
    }

    if (!p_uri->path || strlen(p_uri->path) > MAX_MAILBOX_PATH) {
	xmlFreeURI(p_uri);
	*errstr = "Illegal request target URI";
	return HTTP_BAD_REQUEST;
    }

    /* Make a working copy of the path and free the parsed struct */
    p = strcpy(tgt->path, p_uri->path);
    xmlFreeURI(p_uri);

    if (!strcmp(p, "*")) {
	tgt->allow = ALLOW_ALL;
	return 0;
    }

    if (*p != '/') {
	*errstr = "Illegal request target URI";
	return HTTP_BAD_REQUEST;
    }

    /* Break down path into interesting segments */
    if (!*++p) return 0;

    /* Check namespace */
    len = strcspn(p, "/");
    if (!strncmp(p, "principals", len)) {
	tgt->namespace = URL_NS_PRINCIPAL;
	tgt->allow |= (ALLOW_DAV | ALLOW_CAL | ALLOW_CARD);
    }
    else if (!strncmp(p, "calendars", len)) {
	tgt->namespace = URL_NS_CALENDAR;
	tgt->allow |= (ALLOW_DAV | ALLOW_CAL | ALLOW_WRITE);
    }
    else if (!strncmp(p, "addressbooks", len)) {
	tgt->namespace = URL_NS_ADDRESSBOOK;
	tgt->allow |= (ALLOW_DAV | ALLOW_CARD | ALLOW_WRITE);
    }
    else return 0;

    p += len;
    if (!*p || !*++p) return 0;

    /* Check if we're in user space */
    len = strcspn(p, "/");
    if (!strncmp(p, "user", len)) {
	p += len;
	if (!*p || !*++p) return HTTP_FORBIDDEN;  /* need to specify a userid */

	/* Get user id */
	len = strcspn(p, "/");
	tgt->user = p;
	tgt->userlen = len;

	p += len;
	if (!*p || !*++p) return 0;

	if (tgt->namespace == URL_NS_PRINCIPAL) goto done;

	len = strcspn(p, "/");
    }
    else if (tgt->namespace == URL_NS_PRINCIPAL) {
	return HTTP_FORBIDDEN;  /* need to specify a userid */
    }

    /* Get collection */
    tgt->collection = p;
    tgt->collen = len;

    p += len;
    if (!*p || !*++p) return 0;

    /* Get resource */
    len = strcspn(p, "/");
    tgt->resource = p;
    tgt->reslen = len;

    p += len;

  done:
    if (*p) {
	*errstr = "Too many segments in request target path";
	return HTTP_FORBIDDEN;
    }

    return 0;
}


/* Read the body of a request.  Handles identity and chunked encoding */
static int read_body(struct transaction_t *txn, int dump, const char **errstr)
{
    hdrcache_t req_hdrs = txn->req_hdrs;
    struct buf *req_body = &txn->req_body;
    const char **hdr;
    unsigned long len = 0, chunk;
    unsigned is_chunked;

    buf_reset(req_body);

    /* If we don't care about the body, and client hasn't sent it, we're done */
    if (dump && (txn->flags & HTTP_100CONTINUE)) return 0;

    /* Check for Transfer-Encoding */
    if ((hdr = spool_getheader(req_hdrs, "Transfer-Encoding"))) {
	if (!strcasecmp(hdr[0], "chunked")) {
	    /* "chunked" encoding */
	    is_chunked = 1;
	}
	/* XXX  Should we handle compress/deflate/gzip? */
	else {
	    txn->flags |= HTTP_CLOSE;
	    *errstr = "Specified Transfer-Encoding not implemented";
	    return HTTP_NOT_IMPLEMENTED;
	}
    }
    else {
	/* "identity" encoding - treat it as a single chunk of size "len" */
	is_chunked = 0;
	len = chunk = 0;

	/* Check for Content-Length */
	if ((hdr = spool_getheader(req_hdrs, "Content-Length"))) {
	    len = strtoul(hdr[0], NULL, 10);
	    /* XXX  Should we sanity check and/or limit the body len? */
	}
    }

    /* Tell client to send the body, if necessary */
    if (txn->flags & HTTP_100CONTINUE) response_header(HTTP_CONTINUE, txn);

    /* Read and buffer the body */
    do {
	char buf[PROT_BUFSIZE];
	unsigned long n;

	if (is_chunked) {
	    /* Read chunk-size and any chunk-ext */
	    prot_fgets(buf, PROT_BUFSIZE-2, httpd_in);
	    if (sscanf(buf, "%lx", &chunk) != 1) {
		*errstr = "Unable to read chunk size";
		return HTTP_BAD_REQUEST;
	    }
	    len = chunk;
	}

	/* Read chunk-data */ 
	while (len) {
	    if (!(n = prot_read(httpd_in, buf,
				len > PROT_BUFSIZE ? PROT_BUFSIZE : len))) {
		syslog(LOG_ERR, "prot_read() error");
		*errstr = "Unable to read body data";
		return HTTP_BAD_REQUEST;
	    }

	    if (!dump) buf_appendmap(req_body, buf, n);
	    len -= n;
	}

	if (is_chunked) {
	    if (!chunk) {
		/* last-chunk: Read/parse any trailing headers */
		spool_fill_hdrcache(httpd_in, NULL, req_hdrs, NULL);
	    }

	    /* Read CRLF terminating the chunk */
	    *buf = prot_getc(httpd_in);
	    if (*buf == '\r') *buf = prot_getc(httpd_in);
	    if (*buf != '\n') {
		*errstr = "Missing CRLF in body";
		return HTTP_BAD_REQUEST;
	    }
	}

    } while (chunk);  /* Continue until we get last-chunk */

    return 0;
}


/* Parse an XML body into a tree */
static int parse_xml_body(struct transaction_t *txn,
			  xmlDocPtr *doc, xmlNodePtr *root, const char **errstr)
{
    const char **hdr;

    *doc = NULL;
    *root = NULL;

    if (!buf_len(&txn->req_body)) return 0;

    /* Check Content-Type */
    if ((hdr = spool_getheader(txn->req_hdrs, "Content-Type")) &&
	strncmp(hdr[0], "text/xml", 8) &&
	strncmp(hdr[0], "application/xml", 15)) {
	*errstr = "This method requires an XML body";
	return HTTP_BAD_MEDIATYPE;
    }

    /* Parse the XML request */
    *doc = xmlParseMemory(buf_cstring(&txn->req_body), buf_len(&txn->req_body));
    xmlCleanupParser();
    if (!*doc) {
	*errstr = "Unable to parse XML body";
	return HTTP_BAD_REQUEST;
    }

    /* Get the root element of the XML request */
    if (!(*root = xmlDocGetRootElement(*doc))) {
	*errstr = "Missing root element in request";
	return HTTP_BAD_REQUEST;
    }

    return 0;
}


/****************************  Response Routines  *****************************/


/* Create an HTTP Status-Line given response code */
char *http_statusline(long code)
{
    static char statline[100] = HTTP_VERSION " ";
    static char *tail = NULL;

    if (!tail) tail = statline + strlen(statline);

    strcpy(tail, error_message(code));
    return statline;
}


/* Output an HTTP response header.
 * 'code' specifies the HTTP Status-Code and Reason-Phrase.
 * 'txn' contains the transaction context
 */

#define WWW_Authenticate(name, param)				\
    prot_printf(httpd_out, "WWW-Authenticate: %s", name);	\
    if (param) prot_printf(httpd_out, " %s", param);		\
    prot_printf(httpd_out, "\r\n")


static void response_header(long code, struct transaction_t *txn)
{
    char datestr[RFC822_DATETIME_MAX+1];
    struct auth_challenge_t *auth_chal = &txn->auth_chal;
    struct resp_body_t *resp_body = &txn->resp_body;

    /* Status-Line */
    prot_printf(httpd_out, "%s\r\n", http_statusline(code));


    /* General Header Fields */
    time_to_rfc822(time(0), datestr, sizeof(datestr));
    prot_printf(httpd_out, "Date: %s\r\n", datestr);

    if (txn->flags & HTTP_CLOSE) {
	prot_printf(httpd_out, "Connection: close\r\n");
    }


    /* Response Header Fields */
    if (config_serverinfo == IMAP_ENUM_SERVERINFO_ON) {
	prot_printf(httpd_out, "Server: %s\r\n", serverinfo);
    }

    prot_printf(httpd_out, "Accept-Ranges: none\r\n");

    if ((code == HTTP_NOT_ALLOWED) ||
	((code == HTTP_OK) && (txn->meth[0] == 'O'))) {
	if (txn->req_tgt.allow & ALLOW_DAV) {
	    prot_printf(httpd_out, "DAV: 1, 3");  /* 2, access-control */
	    if (txn->req_tgt.allow & ALLOW_WRITE) {
		prot_printf(httpd_out, ", extended-mkcol");
	    }
	    if (txn->req_tgt.allow & ALLOW_CAL) {
		prot_printf(httpd_out, ", calendar-access"); /* cal-schedule  */
	    }
#if 0
	    if (txn->req_tgt.allow & ALLOW_CARD) {
		prot_printf(httpd_out, ", addressbook");
	    }
#endif
	    prot_printf(httpd_out, "\r\n");
	}

	prot_printf(httpd_out, "Allow: OPTIONS, GET, HEAD");
	if (txn->req_tgt.allow & ALLOW_WRITE) {
	    prot_printf(httpd_out, ", POST, PUT, DELETE");
	}
	if (txn->req_tgt.allow & ALLOW_DAV) {
	    prot_printf(httpd_out, ", REPORT, PROPFIND");
	    if (txn->req_tgt.allow & ALLOW_WRITE) {  /* LOCK, UNLOCK, ACL */
		prot_printf(httpd_out, ", PROPPATCH, MOVE, MKCOL");  /* COPY */
		if (txn->req_tgt.allow & ALLOW_CAL) {
		    prot_printf(httpd_out, ", MKCALENDAR");
		}
	    }
	}
	prot_printf(httpd_out, "\r\n");
    }

    if (code == HTTP_UNAUTHORIZED) {
	if (!auth_chal->scheme) {
	    /* Require authentication by advertising all possible schemes */
	    struct auth_scheme_t *scheme;

	    for (scheme = auth_schemes; scheme->name; scheme++) {
		/* Only advertise what is available and
		   can work with the type of connection */
		if (scheme->is_avail &&
		    ((!txn->flags & HTTP_CLOSE) || !scheme->need_persist)) {
		    auth_chal->param = NULL;

		    if (scheme->is_server_first) {
			/* Generate the initial challenge */
			http_auth(scheme->name, auth_chal);

			if (!auth_chal->param) continue;  /* If fail, skip it */
		    }
		    WWW_Authenticate(scheme->name, auth_chal->param);
		}
	    }
	}
	else {
	    /* Continue with current authentication exchange */ 
	    WWW_Authenticate(auth_chal->scheme->name, auth_chal->param);
	}
    }
    else if (auth_chal->param) {
	/* Authentication completed with success data */
	if (auth_chal->scheme->success) {
	    /* Special handling of success data for this scheme */
	    auth_chal->scheme->success(auth_chal->scheme->name, auth_chal->param);
	}
	else {
	    /* Default handling of success data */
	    WWW_Authenticate(auth_chal->scheme->name, auth_chal->param);
	}
    }

    if (txn->flags & HTTP_NOCACHE) {
	prot_printf(httpd_out, "Cache-Control: no-cache\r\n");
    }

    if (txn->etag) prot_printf(httpd_out, "ETag: \"%s\"\r\n", txn->etag);

    if (txn->loc) prot_printf(httpd_out, "Location: %s\r\n", txn->loc);


    /* Payload Header Fields */
    if (txn->flags & HTTP_CHUNKED) {
	prot_printf(httpd_out, "Transfer-Encoding: chunked\r\n");
    }
    else {
	prot_printf(httpd_out, "Content-Length: %lu\r\n", resp_body->len);
    }


    /* Representation Header Fields */
    if (resp_body->enc) {
	prot_printf(httpd_out, "Content-Encoding: %s\r\n", resp_body->enc);
    }
    if (resp_body->lang) {
	prot_printf(httpd_out, "Content-Language: %s\r\n", resp_body->lang);
    }
    if (resp_body->loc && resp_body->len) {
	prot_printf(httpd_out, "Content-Location: %s\r\n", resp_body->loc);
    }
    if (resp_body->type) {
	prot_printf(httpd_out, "Content-Type: %s\r\n", resp_body->type);
    }
    if (resp_body->lastmod) {
	time_to_rfc822(resp_body->lastmod, datestr, sizeof(datestr));
	prot_printf(httpd_out, "Last-Modified: %s\r\n", datestr);
    }


    /* Blank line terminating the header */
    prot_printf(httpd_out, "\r\n");
}


/* Output an HTTP error response with optional text/plain body */
static void error_response(long code, struct transaction_t *txn,
			   const char *errstr)
{
    struct resp_body_t *resp_body = &txn->resp_body;

    if (errstr) {
	resp_body->len = strlen(errstr)+2;
	resp_body->type = "text/plain";
    }
    response_header(code, txn);
    if (errstr) prot_printf(httpd_out, "%s\r\n", errstr);
}


/* Output an HTTP response with application/xml body */
static void xml_response(long code, struct transaction_t *txn, xmlDocPtr xml)
{
    xmlChar *buf;
    int bufsiz;

    /* Dump XML response tree into a text buffer */
    xmlDocDumpFormatMemoryEnc(xml, &buf, &bufsiz, "utf-8", 1);

    /* Output the XML response */
    txn->resp_body.len = bufsiz;
    txn->resp_body.type = "application/xml; charset=utf-8";

    response_header(code, txn);
    prot_write(httpd_out, (char *) buf, bufsiz);

    /* Cleanup */
    xmlFree(buf);
}


/* Perform HTTP Authentication based on the given credentials ('creds').
 * Returns the selected auth scheme and any server challenge in 'chal'.
 * May be called multiple times if auth scheme requires multiple steps.
 * SASL status between steps is maintained in 'status'.
 */
#define BASE64_BUF_SIZE 21848	/* per RFC 4422: ((16K / 3) + 1) * 4  */

static int http_auth(const char *creds, struct auth_challenge_t *chal)
{
    static int status = SASL_OK;
    size_t slen;
    const char *clientin = NULL;
    unsigned int clientinlen = 0;
    struct auth_scheme_t *scheme;
    static char base64[BASE64_BUF_SIZE+1];
    const void *canon_user;

    syslog(LOG_DEBUG, "http_auth: status=%d   creds='%s'   scheme='%s'",
	   status, creds, chal->scheme ? chal->scheme->name : "");

    chal->param = NULL;

    /* Split credentials into auth scheme and response */
    slen = strcspn(creds, " \0");
    if ((clientin = strchr(creds, ' '))) clientinlen = strlen(++clientin);

    if (chal->scheme) {
	/* Use current scheme, if possible */
	scheme = chal->scheme;

	if (strncasecmp(scheme->name, creds, slen)) {
	    /* Changing auth scheme -> reset state */
	    syslog(LOG_DEBUG, "http_auth: changing scheme");
	    reset_saslconn(&httpd_saslconn);
	    chal->scheme = NULL;
	    status = SASL_OK;
	}
    }

    if (!chal->scheme) {
	/* Find the client-specified auth scheme */
	syslog(LOG_DEBUG, "http_auth: find client scheme");
	for (scheme = auth_schemes; scheme->name; scheme++) {
	    if (!strncasecmp(scheme->name, creds, slen)) {
		/* Found a supported scheme, see if its available */
		if (!scheme->is_avail) scheme = NULL;
		break;
	    }
	}
	if (!scheme) {
	    /* Didn't find a matching scheme that is available */
	    syslog(LOG_WARNING, "Unknown auth scheme '%.*s'", slen, creds);
	    return SASL_NOMECH;
	}

	/* We found it! */
	syslog(LOG_DEBUG, "http_auth: found matching scheme");
	chal->scheme = scheme;
	status = SASL_OK;
    }

    /* Base64 decode any client response, if necesary */
    if (clientin && scheme->do_base64) {
	int r = sasl_decode64(clientin, clientinlen,
			      base64, BASE64_BUF_SIZE, &clientinlen);
	if (r != SASL_OK) {
	    syslog(LOG_ERR, "Base64 decode failed: %s",
		   sasl_errstring(r, NULL, NULL));
	    return r;
	}
	clientin = base64;
    }

    if (scheme->name[0] == 'B') {
	/* Basic (plaintext) authentication */
	const char *user;
	char *pass, userbuf[MAX_MAILBOX_BUFFER];
	unsigned userlen;

	if (!clientin) {
	    /* Create initial challenge (base64 buffer is static) */
	    snprintf(base64, BASE64_BUF_SIZE,
		     "realm=\"%s\"", config_servername);
	    chal->param = base64;
	    return status;
	}

	/* Split credentials into <user> ':' <pass>.
	 * We are working with base64 buffer, so we can modify it.
	 */
	user = (char *) clientin;
	pass = strchr(user, ':');
	if (!pass) {
	    syslog(LOG_ERR, "Basic auth: Missing password");
	    return SASL_BADPARAM;
	}
	*pass++ = '\0';

	/* Canonify the userid */
	status = mysasl_canon_user(httpd_saslconn, NULL, user, strlen(user),
				   SASL_CU_AUTHID | SASL_CU_AUTHZID, NULL,
				   userbuf, sizeof(userbuf), &userlen);
	if (status) {
	    syslog(LOG_NOTICE, "badlogin: %s Basic %s invalid user",
		   httpd_clienthost, beautify_string(user));
	    memset(pass, 0, strlen(pass));	/* erase plaintext password */
	    return status;
	}
	user = userbuf;

	/* Verify the password */
	status = sasl_checkpass(httpd_saslconn, user, userlen,
				pass, strlen(pass));
	memset(pass, 0, strlen(pass));		/* erase plaintext password */

	if (status) {
	    syslog(LOG_NOTICE, "badlogin: %s Basic %s %s",
		   httpd_clienthost, user, sasl_errdetail(httpd_saslconn));

	    /* Don't allow user probing */
	    if (status == SASL_NOUSER) status = SASL_BADAUTH;
	    return status;
	}

	/* Successful authentication - fall through */
    }
    else {
	/* SASL-based authentication (Digest, Negotiate, NTLM) */
	const char *serverout = NULL;
	unsigned int serveroutlen = 0;

	if (status == SASL_CONTINUE) {
	    /* Continue current authentication exchange */
	    syslog(LOG_DEBUG, "http_auth: continue %s", scheme->saslmech);
	    status = sasl_server_step(httpd_saslconn, clientin, clientinlen,
				      &serverout, &serveroutlen);
	}
	else {
	    /* Start new authentication exchange */
	    syslog(LOG_DEBUG, "http_auth: start %s", scheme->saslmech);
	    status = sasl_server_start(httpd_saslconn, scheme->saslmech,
				       clientin, clientinlen,
				       &serverout, &serveroutlen);
	}

	/* Failure - probably bad client response */
	if ((status != SASL_OK) && (status != SASL_CONTINUE)) {
	    syslog(LOG_ERR, "SASL failed: %s",
		   sasl_errstring(status, NULL, NULL));
	    return status;
	}

	/* Base64 encode any server challenge, if necesary */
	if (serverout && scheme->do_base64) {
	    int r = sasl_encode64(serverout, serveroutlen,
				   base64, BASE64_BUF_SIZE, NULL);
	    if (r != SASL_OK) {
		syslog(LOG_ERR, "Base64 encode failed: %s",
		       sasl_errstring(r, NULL, NULL));
		return r;
	    }
	    serverout = base64;
	}

	chal->param = serverout;

	if (status == SASL_CONTINUE) {
	    /* Need another step to complete authentication */
	    return status;
	}

	/* Successful authentication
	 *
	 * HTTP doesn't support security layers,
	 * so don't attach SASL context to prot layer.
	 */
    }

    /* Get the userid from SASL - already canonicalized */
    status = sasl_getprop(httpd_saslconn, SASL_USERNAME, &canon_user);
    if (status != SASL_OK) {
	syslog(LOG_ERR, "weird SASL error %d getting SASL_USERNAME", status);
	return status;
    }

    httpd_userid = xstrdup((const char *) canon_user);

    proc_register("httpd", httpd_clienthost, httpd_userid, (char *)0);

    syslog(LOG_NOTICE, "login: %s %s %s%s %s",
	   httpd_clienthost, httpd_userid, scheme->name,
	   httpd_tls_done ? "+TLS" : "", "User logged in");

    /* Close IP-based telemetry log and create new log based on userid */
    if (httpd_logfd != -1) close(httpd_logfd);
    httpd_logfd = telemetry_log(httpd_userid, httpd_in, httpd_out, 0);

    return status;
}


/*************************  Method Execution Routines  ************************/


/* Create a mailbox name from the request URL */ 
int target_to_mboxname(struct request_target_t *req_tgt, char *mboxname)
{
    char *p;
    size_t siz, len;

    p = mboxname;
    siz = MAX_MAILBOX_BUFFER - 1;
    if (req_tgt->user) {
	len = snprintf(p, siz, "user");
	p += len;
	siz -= len;

	if (req_tgt->userlen) {
	    len = snprintf(p, siz, ".%.*s",
			   req_tgt->userlen, req_tgt->user);
	    p += len;
	    siz -= len;
	}
    }
    len = snprintf(p, siz, "%s%s", p != mboxname ? "." : "",
		   req_tgt->namespace == URL_NS_CALENDAR ? "#calendars" :
		   "#addressbooks");
    p += len;
    siz -= len;
    if (req_tgt->collection) {
	snprintf(p, siz, ".%.*s",
		 req_tgt->collen, req_tgt->collection);
    }

    return 0;
}


/* Compare an etag in a header to a resource etag.
 * Returns 0 if a match, non-zero otherwise.
 */
static int etagcmp(const char *hdr, const char *etag) {
    size_t len;

    if (!etag) return -1;		/* no representation	   */
    if (!strcmp(hdr, "*")) return 0;	/* any representation	   */

    len = strlen(etag);
    if (strlen(hdr) != len+2) return 1;	/* make sure lengths match */
    if (hdr[0] != '\"') return 1;    	/* match open DQUOTE 	   */
    return strncmp(hdr+1, etag, len);   /* skip DQUOTE		   */
}


/* Check headers for any preconditions.
 *
 * Interaction (if any) is complex and is documented in I-D HTTPbis:
 *
 * The If-Match and If-Unmodified-Since headers can be used together, and
 * the If-None-Match and If-Modified-Since headers can be used together, but
 * any other interaction is undefined.
 */
static int check_precond(const char *meth, const char *etag, time_t lastmod,
			 hdrcache_t hdrcache)
{
    unsigned ret = HTTP_OK;
    const char **hdr;
    time_t since;

    if ((hdr = spool_getheader(hdrcache, "If-Match"))) {
	if (!etagcmp(hdr[0], etag)) {
	    /* Precond success - fall through and check If-Unmodified-Since */
	}
	else return HTTP_PRECOND_FAILED;
    }

    if ((hdr = spool_getheader(hdrcache, "If-Unmodified-Since"))) {
	if (time_from_rfc822((char *) hdr[0], &since) < 0) since = lastmod;

	if (lastmod <= since) {
	    /* Precond success - ignore remaining conditional headers */
	    return HTTP_OK;
	}
	else return HTTP_PRECOND_FAILED;
    }

    if ((hdr = spool_getheader(hdrcache, "If-None-Match"))) {
	if (etagcmp(hdr[0], etag)) {
	    /* Precond success - ignore If-Modified-Since */
	    return HTTP_OK;
	}
	else if (!strchr("GH", meth[0])) return HTTP_PRECOND_FAILED;
	else {
	    ret = HTTP_NOT_MODIFIED;
	    /* Fall through and check If-Modified-Since */
	}
    }

    if ((hdr = spool_getheader(hdrcache, "If-Modified-Since"))) {
	if (time_from_rfc822((char *) hdr[0], &since) < 0) since = 0;

	if (lastmod > since) {
	    /* Precond success - this trumps an If-None-Match 304 response */
	    return HTTP_OK;
	}
	else return HTTP_NOT_MODIFIED;
    }

    return ret;
}


/* Perform a COPY/MOVE request
 *
 * preconditions:
 *   CALDAV:supported-calendar-data
 *   CALDAV:valid-calendar-data
 *   CALDAV:valid-calendar-object-resource
 *   CALDAV:supported-calendar-component
 *   CALDAV:no-uid-conflict (DAV:href)
 *   CALDAV:calendar-collection-location-ok
 *   CALDAV:max-resource-size
 *   CALDAV:min-date-time
 *   CALDAV:max-date-time
 *   CALDAV:max-instances
 *   CALDAV:max-attendees-per-instance
 */
static int meth_copy(struct transaction_t *txn)
{
    int ret = 0;

    return ret;
}


/* Perform a DELETE request */
static int meth_delete(struct transaction_t *txn)
{
    int ret = HTTP_NO_CONTENT, r, precond;
    char mailboxname[MAX_MAILBOX_BUFFER];
    struct mailbox *mailbox = NULL;
    struct caldav_db *caldavdb = NULL;
    uint32_t uid = 0;
    struct index_record record;
    const char *etag = NULL;
    time_t lastmod = 0;

    /* Construct mailbox name corresponding to request target URI */
    (void) target_to_mboxname(&txn->req_tgt, mailboxname);

    if (!txn->req_tgt.resource) {
	/* DELETE collection */
	r = mboxlist_deletemailbox(mailboxname,
				   httpd_userisadmin || httpd_userisproxyadmin,
				   httpd_userid, httpd_authstate,
				   1, 0, 0);

	if (r == IMAP_PERMISSION_DENIED) ret = HTTP_FORBIDDEN;
	else if (r == IMAP_MAILBOX_NONEXISTENT) ret = HTTP_NOT_FOUND;
	else if (r) ret = HTTP_SERVER_ERROR;

	return ret;
    }


    /* DELETE resource */

    /* Open mailbox for writing */
    if ((r = mailbox_open_iwl(mailboxname, &mailbox))) {
	syslog(LOG_INFO, "mailbox_open_iwl() failed: %s", error_message(r));
	*txn->errstr = error_message(r);
	ret = HTTP_SERVER_ERROR;
	goto done;
    }

    /* Open the associated CalDAV database */
    if ((r = caldav_open(mailbox, CALDAV_CREATE, &caldavdb))) {
	*txn->errstr = error_message(r);
	ret = HTTP_SERVER_ERROR;
	goto done;
    }

    /* Find message UID for the resource */
    caldav_lockread(caldavdb, txn->req_tgt.resource, &uid);
    /* XXX  Check errors */

    /* Fetch index record for the resource */
    if (!uid || mailbox_find_index_record(mailbox, uid, &record)) {
	ret = HTTP_NOT_FOUND;
	goto done;
    }

    etag = message_guid_encode(&record.guid);
    lastmod = record.internaldate;

    /* Check any preconditions */
    precond = check_precond(txn->meth, etag, lastmod, txn->req_hdrs);

    /* We failed a precondition - don't perform the request */
    if (precond != HTTP_OK) {
	ret = precond;
	goto done;
    }

    /* Expunge the resource */
    record.system_flags |= FLAG_EXPUNGED;

    if ((r = mailbox_rewrite_index_record(mailbox, &record))) {
	syslog(LOG_INFO, "rewrite_index_rec() failed: %s", error_message(r));
	*txn->errstr = error_message(r);
	ret = HTTP_SERVER_ERROR;
	goto done;
    }

    /* Delete mapping entry for resource name */
    caldav_delete(caldavdb, txn->req_tgt.resource);

  done:
    if (caldavdb) caldav_close(caldavdb);
    if (mailbox) mailbox_close(&mailbox);

    return ret;
}


/* Perform a GET/HEAD request */
static int meth_get(struct transaction_t *txn)
{
    int ret = 0, r, precond;
    const char *msg_base = NULL;
    unsigned long msg_size = 0;
    struct resp_body_t *resp_body = &txn->resp_body;

    /* We don't accept a body for this method */
    if (buf_len(&txn->req_body)) return HTTP_BAD_MEDIATYPE;

    if (txn->req_tgt.namespace == URL_NS_CALENDAR) {
	/* In calendar namespace */ 
	char mailboxname[MAX_MAILBOX_BUFFER];
	struct mailbox *mailbox = NULL;
	struct caldav_db *caldavdb = NULL;
	uint32_t uid = 0;
	struct index_record record;
	const char *etag = NULL;
	time_t lastmod = 0;

	/* We don't handle GET on a calendar collection */
	if (!txn->req_tgt.resource) return HTTP_NO_CONTENT;

	/* Construct mailbox name corresponding to request target URI */
	(void) target_to_mboxname(&txn->req_tgt, mailboxname);

	/* Open mailbox for reading */
	if ((r = mailbox_open_irl(mailboxname, &mailbox))) {
	    syslog(LOG_INFO, "mailbox_open_irl() failed: %s",
		   error_message(r));
	    *txn->errstr = error_message(r);
	    ret = HTTP_SERVER_ERROR;
	    goto done;
	}

	/* Open the associated CalDAV database */
	if ((r = caldav_open(mailbox, CALDAV_CREATE, &caldavdb))) {
	    *txn->errstr = error_message(r);
	    ret = HTTP_SERVER_ERROR;
	    goto done;
	}

	/* Find message UID for the resource */
	caldav_read(caldavdb, txn->req_tgt.resource, &uid);
	/* XXX  Check errors */

	/* Fetch index record for the resource */
	r = mailbox_find_index_record(mailbox, uid, &record);
	/* XXX  check for errors */

	etag = message_guid_encode(&record.guid);
	lastmod = record.internaldate;

	/* Check any preconditions */
	precond = check_precond(txn->meth, etag, lastmod, txn->req_hdrs);

	if (precond != HTTP_OK) {
	    /* We failed a precondition - don't perform the request */
	    ret = precond;
	    goto done;
	}

	/* Fill in Etag, Last-Modified, and Content-Length */
	txn->etag = etag;
	resp_body->lastmod = lastmod;
	resp_body->len = record.size - record.header_size;  /* skip msg hdr */
	resp_body->type = "text/calendar; charset=utf-8";

	response_header(HTTP_OK, txn);

	if (txn->meth[0] == 'G') {
	    /* Load message containing the resource */
	    mailbox_map_message(mailbox, record.uid, &msg_base, &msg_size);

	    prot_write(httpd_out,
		       msg_base + record.header_size,  /* skip message header */
		       resp_body->len);

	    mailbox_unmap_message(mailbox, record.uid, &msg_base, &msg_size);
	}

      done:
	if (caldavdb) caldav_close(caldavdb);
	if (mailbox) mailbox_close(&mailbox);
    }

    else if (txn->req_tgt.namespace == URL_NS_DEFAULT) {
	/* Serve up static pages */
	char path[1024] = "/tmp/doc";   /* XXX  make this configurable */
	struct stat sbuf;
	int fd;
	struct message_guid guid;
	char *outbuf;

	if (!txn->req_tgt.path || !*txn->req_tgt.path ||
	    (txn->req_tgt.path[0] == '/' && txn->req_tgt.path[1] == '\0'))
	    strcat(path, "/index.html");
	else
	    strcat(path, txn->req_tgt.path);

	/* See if file exists and get Content-Length & Last-Modified time */
	if (stat(path, &sbuf)) return HTTP_NOT_FOUND;

	/* Open the file */
	fd = open(path, O_RDONLY);
	if (fd == -1) return HTTP_NOT_FOUND;

	map_refresh(fd, 1, &msg_base, &msg_size, sbuf.st_size, path, NULL);

	/* Fill in Etag, Last-Modified, and Content-Length */
	message_guid_generate(&guid, msg_base, msg_size);
	txn->etag = message_guid_encode(&guid);
	resp_body->lastmod = sbuf.st_mtime;
	resp_body->len = msg_size;

	/* Check any preconditions */
	precond = check_precond(txn->meth, txn->etag,
				resp_body->lastmod, txn->req_hdrs);

	/* We failed a precondition - don't perform the request */
	if (precond != HTTP_OK) {
	    map_free(&msg_base, &msg_size);
	    close(fd);

	    return precond;
	}

	/* Assume identity encoding */
	outbuf = (char *) msg_base;

	/* Look for filetype signatures in the data */
	if (msg_size >= 8 &&
	    !memcmp(msg_base, "\x89\x50\x4E\x47\x0D\x0A\x1A\x0A", 8)) {
	    resp_body->type = "image/png";
	} else if (msg_size >= 4 &&
		   !memcmp(msg_base, "\xFF\xD8\xFF\xE0", 4)) {
	    resp_body->type = "image/jpeg";
	} else {
	    resp_body->type = "text/html";
	}
#if 0
	/* deflate encoding */
	resp_body->enc = "deflate";
	resp_body->len = compressBound(msg_size);
	outbuf = xmalloc(resp_body->len);
	compress(outbuf, &resp_body->len, msg_base, msg_len);
#endif
	response_header(HTTP_OK, txn);

	if (txn->meth[0] == 'G') prot_write(httpd_out, outbuf, resp_body->len);

	if (outbuf != msg_base) free(outbuf);
	map_free(&msg_base, &msg_size);
	close(fd);
    }

    return ret;
}


/* Perform a MKCOL/MKCALENDAR request */
/*
 * preconditions:
 *   DAV:resource-must-be-null
 *   DAV:need-privilege
 *   DAV:valid-resourcetype
 *   CALDAV:calendar-collection-location-ok
 *   CALDAV:valid-calendar-data (CALDAV:calendar-timezone)
 */
static int meth_mkcol(struct transaction_t *txn)
{
    int ret = 0, r = 0;
    xmlDocPtr indoc = NULL, outdoc = NULL;
    xmlNodePtr root = NULL, instr = NULL;
    xmlNodePtr propstat[NUM_PROPSTAT] = { NULL, NULL, NULL };
    xmlNsPtr ns[NUM_NAMESPACE];
    char mailboxname[MAX_MAILBOX_BUFFER], *partition = NULL;
    struct proppatch_ctx pctx;

    /* Response should not be cached */
    txn->flags |= HTTP_NOCACHE;

    if (!txn->req_tgt.collection || txn->req_tgt.resource) {
	*txn->errstr = "Calendars can only be created under a home-set collection";
	return HTTP_FORBIDDEN;
    }

    /* Parse the MKCOL/MKCALENDAR body, if exists */
    ret = parse_xml_body(txn, &indoc, &root, txn->errstr);
    if (ret) goto done;

    if (root) {
	if ((txn->meth[3] == 'O') &&
	    /* Make sure its a mkcol element */
	    xmlStrcmp(root->name, BAD_CAST "mkcol")) {
	    *txn->errstr = "Missing mkcol element in MKCOL request";
	    return HTTP_BAD_MEDIATYPE;
	}
	else if ((txn->meth[3] == 'A') &&
		 /* Make sure its a mkcalendar element */
		 xmlStrcmp(root->name, BAD_CAST "mkcalendar")) {
	    *txn->errstr = "Missing mkcalendar element in MKCALENDAR request";
	    return HTTP_BAD_MEDIATYPE;
	}

	instr = root->children;
    }

    /* Construct mailbox name corresponding to request target URI */
    (void) target_to_mboxname(&txn->req_tgt, mailboxname);

    /* Check if we are allowed to create the mailbox */
    r = mboxlist_createmailboxcheck(mailboxname, 0, NULL,
				    httpd_userisadmin || httpd_userisproxyadmin,
				    httpd_userid, httpd_authstate,
				    NULL, &partition, 0);

    if (r == IMAP_PERMISSION_DENIED) ret = HTTP_FORBIDDEN;
    else if (r == IMAP_MAILBOX_EXISTS) ret = HTTP_FORBIDDEN;
    else if (r) ret = HTTP_SERVER_ERROR;

    if (ret) goto done;

    if (instr) {
	/* Start construction of our mkcol/mkcalendar response */
	outdoc = init_prop_response(txn->meth[3] == 'A' ?
				    "mkcalendar-response" : "mkcol-response",
				    &root, ns);

	/* Populate our proppatch context */
	pctx.req_tgt = &txn->req_tgt;
	pctx.meth = txn->meth;
	pctx.mailboxname = mailboxname;
	pctx.root = root;
	pctx.ns = ns;
	pctx.tid = NULL;
	pctx.errstr = txn->errstr;
	pctx.ret = &r;

	/* Execute the property patch instructions */
	ret = do_proppatch(&pctx, instr, propstat, txn->errstr);

	if (ret || r) {
	    /* Something failed.  Abort the txn and change the OK status */
	    annotatemore_abort(pctx.tid);

	    if (!ret) {
		if (propstat[PROPSTAT_OK]) {
		    xmlNodeSetContent(propstat[PROPSTAT_OK]->parent->children,
				      BAD_CAST http_statusline(HTTP_FAILED_DEP));
		}

		/* Output the XML response */
		xml_response(HTTP_MULTI_STATUS, txn, outdoc);
		ret = 0;
	    }

	    goto done;
	}
    }

    /* Create the mailbox */
    r = mboxlist_createmailbox(mailboxname, 0, partition, 
			       httpd_userisadmin || httpd_userisproxyadmin,
			       httpd_userid, httpd_authstate,
			       0, 0, 0, NULL);

    if (!r) ret = HTTP_CREATED;
    else if (r == IMAP_PERMISSION_DENIED) ret = HTTP_FORBIDDEN;
    else if (r == IMAP_MAILBOX_EXISTS) ret = HTTP_FORBIDDEN;
    else if (r) ret = HTTP_SERVER_ERROR;

    if (instr) {
	if (r) {
	    /* Failure.  Abort the txn */
	    annotatemore_abort(pctx.tid);
	}
	else {
	    /* Success.  Commit the txn */
	    annotatemore_commit(pctx.tid);
	}
    }

  done:
    if (outdoc) xmlFreeDoc(outdoc);
    if (indoc) xmlFreeDoc(indoc);

    return ret;
}


/* Perform an OPTIONS request */
static int meth_options(struct transaction_t *txn)
{
    /* Response should not be cached */
    txn->flags |= HTTP_NOCACHE;

    if (buf_len(&txn->req_body)) {
	*txn->errstr = "A body is not expected for this method";
        return HTTP_BAD_MEDIATYPE;
    }

    response_header(HTTP_OK, txn);
    return 0;
}


/* Perform a PROPFIND request */
static int meth_propfind(struct transaction_t *txn)
{
    int ret = 0, r;
    const char **hdr;
    unsigned depth;
    xmlDocPtr indoc = NULL, outdoc = NULL;
    xmlNodePtr root, cur;
    xmlNsPtr ns[NUM_NAMESPACE];
    char mailboxname[MAX_MAILBOX_BUFFER];
    struct propfind_ctx fctx;
    struct propfind_entry_list *elist = NULL;

    /* Check Depth */
    hdr = spool_getheader(txn->req_hdrs, "Depth");
    if (!hdr || !strcmp(hdr[0], "infinity")) {
	depth = 2;
    }
    if (hdr && ((sscanf(hdr[0], "%u", &depth) != 1) || (depth > 1))) {
	*txn->errstr = "Illegal Depth value";
	return HTTP_BAD_REQUEST;
    }

    /* Normalize depth so that:
     * 0 = home-set collection, 1+ = calendar collection, 2+ = calendar resource
     */
    if (txn->req_tgt.collection) depth++;
    if (txn->req_tgt.resource) depth++;

    /* Parse the PROPFIND body, if exists */
    ret = parse_xml_body(txn, &indoc, &root, txn->errstr);
    if (ret) goto done;

    if (!root) {
	/* XXX allprop request */
    }

    /* Make sure its a propfind element */
    if (xmlStrcmp(root->name, BAD_CAST "propfind")) {
	*txn->errstr = "Missing propfind element in PROFIND request";
	return HTTP_BAD_REQUEST;
    }

    /* Find child element of propfind */
    for (cur = root->children;
	 cur && cur->type != XML_ELEMENT_NODE; cur = cur->next);

    /* Make sure its a prop element */
    /* XXX  TODO: Check for allprop and propname too */
    if (!cur || xmlStrcmp(cur->name, BAD_CAST "prop")) {
	return HTTP_BAD_REQUEST;
    }

    /* Start construction of our multistatus response */
    outdoc = init_prop_response("multistatus", &root, ns);

    /* Parse the list of properties and build a list of callbacks */
    preload_proplist(cur->children, ns, &elist);

    /* Populate our propfind context */
    fctx.req_tgt = &txn->req_tgt;
    fctx.depth = depth;
    fctx.userid = httpd_userid;
    fctx.mailbox = NULL;
    fctx.record = NULL;
    fctx.elist = elist;
    fctx.root = root;
    fctx.ns = ns;
    fctx.errstr = txn->errstr;
    fctx.ret = &ret;

    if (!txn->req_tgt.collection) {
	/* Add response for home-set collection */
	add_prop_response(&fctx);
    }

    if (depth > 0) {
	/* Calendar collection(s) */

	/* Construct mailbox name corresponding to request target URI */
	(void) target_to_mboxname(&txn->req_tgt, mailboxname);

	if (txn->req_tgt.collection) {
	    /* Add response for target calendar collection */
	    find_collection_props(mailboxname, 0, 0, &fctx);
	}
	else {
	    /* Add responses for all contained calendar collections */
	    strlcat(mailboxname, ".%", sizeof(mailboxname));
	    r = mboxlist_findall(NULL,  /* internal namespace */
				 mailboxname, 0, httpd_userid, 
				 httpd_authstate, find_collection_props, &fctx);
	}

	ret = *fctx.ret;
    }

    /* Output the XML response */
    xml_response(HTTP_MULTI_STATUS, txn, outdoc);

  done:
    /* Free the entry list */
    while (elist) {
	struct propfind_entry_list *freeme = elist;
	elist = elist->next;
	free(freeme);
    }

    if (outdoc) xmlFreeDoc(outdoc);
    if (indoc) xmlFreeDoc(indoc);

    return ret;
}


/* Perform a PROPPATCH request
 *
 * preconditions:
 *   DAV:cannot-modify-protected-property
 *   CALDAV:valid-calendar-data (CALDAV:calendar-timezone)
 */
static int meth_proppatch(struct transaction_t *txn)
{
    int ret = 0, r = 0;
    xmlDocPtr indoc = NULL, outdoc = NULL;
    xmlNodePtr root, instr;
    xmlNodePtr resp, propstat[NUM_PROPSTAT] = { NULL, NULL, NULL };
    xmlNsPtr ns[NUM_NAMESPACE];
    char mailboxname[MAX_MAILBOX_BUFFER];
    struct proppatch_ctx pctx;

    /* Response should not be cached */
    txn->flags |= HTTP_NOCACHE;

    if (!txn->req_tgt.collection || txn->req_tgt.resource) {
	*txn->errstr = "Properties can only be updated on calendar collections";
	return HTTP_FORBIDDEN;
    }

    /* Parse the PROPPATCH body */
    ret = parse_xml_body(txn, &indoc, &root, txn->errstr);
    if (!root) {
	*txn->errstr = "Missing request body";
	return HTTP_BAD_REQUEST;
    }
    if (ret) goto done;

    /* Make sure its a propertyupdate element */
    if (xmlStrcmp(root->name, BAD_CAST "propertyupdate")) {
	*txn->errstr = "Missing propertyupdate element in PROPPATCH request";
	return HTTP_BAD_REQUEST;
    }
    instr = root->children;

    /* Start construction of our multistatus response */
    outdoc = init_prop_response("multistatus", &root, ns);

    /* Add a response tree to 'root' for the specified href */
    resp = xmlNewChild(root, NULL, BAD_CAST "response", NULL);
    if (!resp) syslog(LOG_INFO, "new child response failed");
    xmlNewChild(resp, NULL, BAD_CAST "href", BAD_CAST txn->req_tgt.path);

    /* Construct mailbox name corresponding to request target URI */
    (void) target_to_mboxname(&txn->req_tgt, mailboxname);

    /* Populate our proppatch context */
    pctx.req_tgt = &txn->req_tgt;
    pctx.meth = txn->meth;
    pctx.mailboxname = mailboxname;
    pctx.root = resp;
    pctx.ns = ns;
    pctx.tid = NULL;
    pctx.errstr = txn->errstr;
    pctx.ret = &r;

    /* Execute the property patch instructions */
    ret = do_proppatch(&pctx, instr, propstat, txn->errstr);

    if (ret || r) {
	/* Something failed.  Abort the txn and change the OK status */
	annotatemore_abort(pctx.tid);

	if (ret) goto done;

	if (propstat[PROPSTAT_OK]) {
	    xmlNodeSetContent(propstat[PROPSTAT_OK]->parent->children,
			      BAD_CAST http_statusline(HTTP_FAILED_DEP));
	}
    }
    else {
	/* Success.  Commit the txn */
	annotatemore_commit(pctx.tid);
    }

    /* Output the XML response */
    xml_response(HTTP_MULTI_STATUS, txn, outdoc);

  done:
    if (outdoc) xmlFreeDoc(outdoc);
    if (indoc) xmlFreeDoc(indoc);

    return ret;
}


/* Perform a PUT/POST request
 *
 * preconditions:
 *   CALDAV:supported-calendar-data
 *   CALDAV:valid-calendar-data
 *   CALDAV:valid-calendar-object-resource
 *   CALDAV:supported-calendar-component
 *   CALDAV:no-uid-conflict (DAV:href)
 *   CALDAV:max-resource-size
 *   CALDAV:min-date-time
 *   CALDAV:max-date-time
 *   CALDAV:max-instances
 *   CALDAV:max-attendees-per-instance
 */
static int global_put_count = 0;

static int meth_put(struct transaction_t *txn)
{
    int ret = HTTP_CREATED, r, precond;
    char mailboxname[MAX_MAILBOX_BUFFER];
    struct mailbox *mailbox = NULL;
    struct caldav_db *caldavdb = NULL;
    uint32_t olduid = 0;
    struct index_record oldrecord;
    const char *etag;
    time_t lastmod;
    FILE *f = NULL;
    struct stagemsg *stage = NULL;
    const char **hdr;
    uquota_t size = 0;
    time_t now = time(NULL);
    pid_t p;
    char datestr[RFC822_DATETIME_MAX+1], msgid[8192];
    struct appendstate appendstate;
    icalcomponent *ical, *comp;

    /* We don't handle POST/PUT on non-calendar collections */
    if (!txn->req_tgt.collection) return HTTP_NOT_ALLOWED;

    /* We don't handle PUT on calendar collections */
    if (!txn->req_tgt.resource && (txn->meth[1] != 'O')) return HTTP_NOT_ALLOWED;

    /* Make sure we have a body */
    if (!buf_len(&txn->req_body)) {
	*txn->errstr = "Missing request body";
	return HTTP_BAD_REQUEST;
    }

    /* Check Content-Type */
    if ((hdr = spool_getheader(txn->req_hdrs, "Content-Type")) &&
	strncmp(hdr[0], "text/calendar", 13)) {
	*txn->errstr = "This collection only supports text/calendar data";
	return HTTP_BAD_MEDIATYPE;
    }

    /* Parse the iCal data for important properties */
    ical = icalparser_parse_string(buf_cstring(&txn->req_body));
    if (!ical) {
	*txn->errstr = "Invalid calendar data";
	return HTTP_BAD_MEDIATYPE;
    }
    comp = icalcomponent_get_first_real_component(ical);

    /* Construct mailbox name corresponding to request target URI */
    (void) target_to_mboxname(&txn->req_tgt, mailboxname);

    /* Open mailbox for reading */
    if ((r = mailbox_open_irl(mailboxname, &mailbox))) {
	syslog(LOG_INFO, "mailbox_open_irl() failed: %s", error_message(r));
	*txn->errstr = error_message(r);
	ret = HTTP_SERVER_ERROR;
	goto done;
    }

    /* Open the associated CalDAV database */
    if ((r = caldav_open(mailbox, CALDAV_CREATE, &caldavdb))) {
	*txn->errstr = error_message(r);
	ret = HTTP_SERVER_ERROR;
	goto done;
    }

    if (txn->meth[1] == 'O') {
	/* POST - Create a unique resource name and append to URL path */
	size_t len = strlen(txn->req_tgt.path);
	char *p = txn->req_tgt.path + len;

	if (p[-1] != '/') {
	    *p++ = '/';
	    len++;
	}
	snprintf(p, MAX_MAILBOX_PATH - len, "%08X-%s.ics",
		 strhash(mailboxname), icalcomponent_get_uid(comp));
	txn->req_tgt.resource = p;
	txn->req_tgt.reslen = strlen(p);
    }

    /* Find message UID for the resource */
    caldav_read(caldavdb, txn->req_tgt.resource, &olduid);
    /* XXX  Check errors */

    if (olduid) {
	/* Overwriting existing resource */

	/* Fetch index record for the resource */
	r = mailbox_find_index_record(mailbox, olduid, &oldrecord);
	/* XXX  check for errors */

	etag = message_guid_encode(&oldrecord.guid);
	lastmod = oldrecord.internaldate;
    }
    else {
	/* New resource */
	etag = NULL;
	lastmod = 0;
    }

    /* Check any preconditions */
    precond = check_precond(txn->meth, etag, lastmod, txn->req_hdrs);

    if (precond != HTTP_OK) {
	/* We failed a precondition - don't perform the request */
	ret = precond;
	goto done;
    }

    /* Finished our initial read */
    mailbox_unlock_index(mailbox, NULL);

    /* Check if we can append a new iMIP message to calendar mailbox */
    if ((r = append_check(mailboxname, httpd_authstate, ACL_INSERT, size))) {
	*txn->errstr = error_message(r);
	ret = HTTP_SERVER_ERROR;
	goto done;
    }

    /* Prepare to stage the message */
    if (!(f = append_newstage(mailboxname, now, 0, &stage))) {
	*txn->errstr = error_message(r);
	ret = HTTP_SERVER_ERROR;
	goto done;
    }


    /* Create iMIP header for resource */
    fprintf(f, "From: <%s>\r\n", httpd_userid ? httpd_userid : "");

    fprintf(f, "Subject: %s\r\n", icalcomponent_get_summary(comp));

    time_to_rfc822(now, datestr, sizeof(datestr));
    fprintf(f, "Date: %s\r\n", datestr);

    p = getpid();
    snprintf(msgid, sizeof(msgid), "<cmu-http-%d-%d-%d@%s>", 
	     (int) p, (int) now, global_put_count++, config_servername);
    fprintf(f, "Message-ID: %s\r\n", msgid);

    hdr = spool_getheader(txn->req_hdrs, "Content-Type");
    fprintf(f, "Content-Type: %s\r\n", hdr[0]);

    fprintf(f, "Content-Length: %u\r\n", buf_len(&txn->req_body));
    fprintf(f, "Content-Disposition: inline; filename=%s\r\n",
	    txn->req_tgt.resource);

    /* XXX  Check domain of data and use appropriate CTE */

    fprintf(f, "MIME-Version: 1.0\r\n");
    fprintf(f, "\r\n");
    size += ftell(f);

    /* Write the iCal data to the file */
    fprintf(f, "%s", buf_cstring(&txn->req_body));
    size += buf_len(&txn->req_body);

    fclose(f);


    /* Prepare to append the iMIP message to calendar mailbox */
    if ((r = append_setup(&appendstate, mailboxname, 
			  httpd_userid, httpd_authstate, ACL_INSERT, size))) {
	ret = HTTP_SERVER_ERROR;
	*txn->errstr = "append_setup() failed";
    }
    else {
	struct body *body = NULL;

	/* Append the iMIP file to the calendar mailbox */
	if ((r = append_fromstage(&appendstate, &body, stage, now, NULL, 0))) {
	    ret = HTTP_SERVER_ERROR;
	    *txn->errstr = "append_fromstage() failed";
	}
	if (body) message_free_body(body);

	if (!r) {
	    /* Commit the append to the calendar mailbox */
	    if ((r = append_commit(&appendstate, size,
				   NULL, NULL, NULL, &mailbox))) {
		ret = HTTP_SERVER_ERROR;
		*txn->errstr = "append_commit() failed";
	    }
	    else {
		struct index_record newrecord, *expunge;

		/* Read index record for new message (always the last one) */
		mailbox_read_index_record(mailbox, mailbox->i.num_records,
					  &newrecord);

		/* Find message UID for the resource */
		caldav_lockread(caldavdb, txn->req_tgt.resource, &olduid);
		/* XXX  check for errors */

		if (olduid) {
		    /* Now that we have the replacement message in place
		       and the mailbox locked, re-read the old record
		       and re-test any preconditions. Either way,
		       one of our records will have to be expunged.
		    */
		    ret = HTTP_NO_CONTENT;

		    /* Fetch index record for the resource */
		    r = mailbox_find_index_record(mailbox, olduid, &oldrecord);

		    etag = message_guid_encode(&oldrecord.guid);
		    lastmod = oldrecord.internaldate;

		    /* Check any preconditions */
		    precond = check_precond(txn->meth, etag, lastmod, txn->req_hdrs);

		    if (precond != HTTP_OK) {
			/* We failed a precondition */
			ret = precond;

			/* Keep old resource - expunge the new one */
			expunge = &newrecord;
		    }
		    else {
			/* Keep new resource - expunge the old one */
			expunge = &oldrecord;
		    }

		    /* Perform the actual expunge */
		    expunge->system_flags |= FLAG_EXPUNGED;
		    if ((r = mailbox_rewrite_index_record(mailbox, expunge))) {
			syslog(LOG_INFO, "rewrite_index_rec() failed: %s",
			       error_message(r));
			*txn->errstr = error_message(r);
			ret = HTTP_SERVER_ERROR;
			goto done;
		    }
		}

		/* Create mapping entry from resource name and UID */
		caldav_write(caldavdb, txn->req_tgt.resource, newrecord.uid);
		/* XXX  check for errors, if this fails, backout changes */

		/* Tell client about the new resource */
		txn->etag = message_guid_encode(&newrecord.guid);
		txn->loc = txn->req_tgt.path;
	    }
	}
	else {
	    append_abort(&appendstate);
	}
    }

  done:
    if (stage) append_removestage(stage);
    if (caldavdb) caldav_close(caldavdb);
    if (mailbox) mailbox_close(&mailbox);

    return ret;
}


/* Report types */
enum {
    REPORT_CAL_QUERY = 0,
    REPORT_CAL_MULTIGET,
    REPORT_FB_QUERY,
    REPORT_EXPAND_PROP,
    REPORT_PRIN_PROP_SET,
    REPORT_PRIN_MATCH,
    REPORT_PRIN_PROP_SRCH
};

/* Perform a REPORT request */
static int meth_report(struct transaction_t *txn)
{
    int ret = 0, r;
    const char **hdr;
    unsigned depth = 0, type;
    xmlDocPtr indoc = NULL, outdoc = NULL;
    xmlNodePtr root, cur;
    xmlNsPtr ns[NUM_NAMESPACE];
    char mailboxname[MAX_MAILBOX_BUFFER];
    struct mailbox *mailbox = NULL;
    struct caldav_db *caldavdb = NULL;
    struct propfind_ctx fctx;
    struct propfind_entry_list *elist = NULL;

    /* Check Depth */
    if ((hdr = spool_getheader(txn->req_hdrs, "Depth"))) {
	if (!strcmp(hdr[0], "infinity")) {
	    *txn->errstr = "This server DOES NOT support infinite depth requests";
	    return HTTP_SERVER_ERROR;
	}
	else if ((sscanf(hdr[0], "%u", &depth) != 1) || (depth > 1)) {
	    *txn->errstr = "Illegal Depth value";
	    return HTTP_BAD_REQUEST;
	}
    }

    /* Parse the REPORT body */
    ret = parse_xml_body(txn, &indoc, &root, txn->errstr);
    if (!root) {
	*txn->errstr = "Missing request body";
	return HTTP_BAD_REQUEST;
    }
    if (ret) goto done;

    /* Make sure its a calendar element */
    if (!xmlStrcmp(root->name, BAD_CAST "calendar-query")) {
	type = REPORT_CAL_QUERY;
    }
    else if (!xmlStrcmp(root->name, BAD_CAST "calendar-multiget")) {
	type = REPORT_CAL_MULTIGET;
    }
    else {
	*txn->errstr = "Unsupported REPORT type";
	return HTTP_NOT_IMPLEMENTED;
    }

    /* Find child element of report */
    for (cur = root->children;
	 cur && cur->type != XML_ELEMENT_NODE; cur = cur->next);

    /* Make sure its a prop element */
    if (!cur || xmlStrcmp(cur->name, BAD_CAST "prop")) {
	*txn->errstr = "MIssing prop element";
	return HTTP_BAD_REQUEST;
    }

    /* Start construction of our multistatus response */
    outdoc = init_prop_response("multistatus", &root, ns);

    /* Parse the list of properties and build a list of callbacks */
    preload_proplist(cur->children, ns, &elist);

    /* Construct mailbox name corresponding to request target URI */
    (void) target_to_mboxname(&txn->req_tgt, mailboxname);

    /* Open mailbox for reading */
    if ((r = mailbox_open_irl(mailboxname, &mailbox))) {
	syslog(LOG_INFO, "mailbox_open_irl() failed: %s", error_message(r));
	*txn->errstr = error_message(r);
	ret = HTTP_SERVER_ERROR;
	goto done;
    }

    /* Open the associated CalDAV database */
    if ((r = caldav_open(mailbox, CALDAV_CREATE, &caldavdb))) {
	*txn->errstr = error_message(r);
	ret = HTTP_SERVER_ERROR;
	goto done;
    }

    /* Populate our propfind context */
    fctx.req_tgt = &txn->req_tgt;
    fctx.depth = depth;
    fctx.userid = httpd_userid;
    fctx.mailbox = mailbox;
    fctx.record = NULL;
    fctx.elist = elist;
    fctx.root = root;
    fctx.ns = ns;
    fctx.errstr = txn->errstr;
    fctx.ret = &ret;

    switch (type) {
    case REPORT_CAL_QUERY: {

	/* XXX  TODO: Need to handle the filter */
	caldav_foreach(caldavdb, find_resource_props, &fctx);
    }
	break;
    case REPORT_CAL_MULTIGET:
	/* Get props for each href */
	for (; cur; cur = cur->next) {
	    if ((cur->type == XML_ELEMENT_NODE) &&
		!xmlStrcmp(cur->name, BAD_CAST "href")) {
		xmlChar *href = xmlNodeListGetString(indoc, cur->children, 1);
		const char *resource = strrchr((char *) href, '/') + 1;
		uint32_t uid = 0;

		/* Find message UID for the resource */
		caldav_read(caldavdb, resource, &uid);
		/* XXX  Check errors */

		find_resource_props(&fctx, resource, uid);
	    }
	}
	break;
    }

    /* Output the XML response */
    xml_response(HTTP_MULTI_STATUS, txn, outdoc);

  done:
    if (caldavdb) caldav_close(caldavdb);
    if (mailbox) mailbox_close(&mailbox);

    /* Free the entry list */
    while (elist) {
	struct propfind_entry_list *freeme = elist;
	elist = elist->next;
	free(freeme);
    }

    if (outdoc) xmlFreeDoc(outdoc);
    if (indoc) xmlFreeDoc(indoc);

    return ret;
}


#if 0  /* XXX  for debugging */
static void print_element_names(xmlNode * a_node)
{
    xmlNode *cur_node = NULL;

    for (cur_node = a_node; cur_node; cur_node = cur_node->next) {
        if (cur_node->type == XML_ELEMENT_NODE) {
            syslog(LOG_INFO, "node type: Element, name: %s, ns %s:%s",
		   cur_node->name, cur_node->ns->prefix, cur_node->ns->href);
        }

        print_element_names(cur_node->children);
    }
}
#endif
