/* $Id: acconfig.h,v 1.10 2000/10/18 20:27:55 leg Exp $ */
/* 
 * Copyright (c) 2000 Carnegie Mellon University.  All rights reserved.
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
 *    prior written permission. For permission or any other legal
 *    details, please contact  
 *      Office of Technology Transfer
 *      Carnegie Mellon University
 *      5000 Forbes Avenue
 *      Pittsburgh, PA  15213-3890
 *      (412) 268-4387, fax: (412) 268-7395
 *      tech-transfer@andrew.cmu.edu
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
 */


@TOP@

/* where are we going to be installed? */
#define CYRUS_PATH "/usr/cyrus"

/* what user are we going to run as? */
#define CYRUS_USER "cyrus"

/* do we have strerror()? */
#undef HAS_STRERROR

/* do we have kerberos? */
#undef HAVE_KRB

/* do we already have sys_errlist? */
#undef NEED_SYS_ERRLIST

/* how should we setproctitle? */
#undef SPT_TYPE

/* do we have the AFS symbol pr_End? */
#undef HAVE_PR_END

/* do we have an acceptable regex library? */
#undef ENABLE_REGEX

/* do we support XNETSCAPE */
#undef ENABLE_X_NETSCAPE_HACK

/* do we support THREAD=JWZ */
#undef ENABLE_THREAD_JWZ

/* do we support THREAD=REF */
#undef ENABLE_THREAD_REF

/* we better have berkeley db 3.x */
#undef HAVE_LIBDB

/* the AFS RX (RPC) package */
#undef HAVE_RX

/* the TCP control package */
#undef HAVE_LIBWRAP

/* do we have OpenSSL? */
#undef HAVE_SSL

/* where should we put state information? */
#undef STATEDIR

/* is Sieve enabled? */
#undef USE_SIEVE

/* _POSIX_PTHREAD_SEMANTICS needed? */
#undef _POSIX_PTHREAD_SEMANTICS

/* _REENTRANT needed? */
#undef _REENTRANT

/* _SGI_REENTRANT_FUNCTIONS needed? */
#undef _SGI_REENTRANT_FUNCTIONS

/* This seems to be required to make Solaris happy. */
#undef __EXTENSIONS__

@BOTTOM@

#ifndef __GNUC__
#define __attribute__(foo)
#endif

/* compile time options; think carefully before modifying */
enum {
    /* should a hierarchical rename stop on error? */
    RENAME_STOP_ON_ERROR = 1,

    /* should we call fsync() to maybe help with softupdates?
     * (that is, i'm not sure this solves the problem.) */
    APPEND_ULTRA_PARANOID = 1,

    /* should we log extra information at the DEBUG level for DB stuff? 
     * 0 -> nothing; 1 -> some; higher -> even more */
    CONFIG_DB_VERBOSE = 1,

    /* log timing information to LOG_DEBUG */
    CONFIG_TIMING_VERBOSE = 0
};
