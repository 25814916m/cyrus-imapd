/* reconstruct.c -- program to reconstruct a mailbox 
 *
 * Copyright (c) 1998-2000 Carnegie Mellon University.  All rights reserved.
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

/* $Id: reconstruct.c,v 1.54 2000/07/11 17:55:01 leg Exp $ */

#include <config.h>

#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <ctype.h>
#include <syslog.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <sys/stat.h>
#include <com_err.h>
#include <stdlib.h>

#if HAVE_DIRENT_H
# include <dirent.h>
# define NAMLEN(dirent) strlen((dirent)->d_name)
#else
# define dirent direct
# define NAMLEN(dirent) (dirent)->d_namlen
# if HAVE_SYS_NDIR_H
#  include <sys/ndir.h>
# endif
# if HAVE_SYS_DIR_H
#  include <sys/dir.h>
# endif
# if HAVE_NDIR_H
#  include <ndir.h>
# endif
#endif

#include "acl.h"
#include "assert.h"
#include "bsearch.h"
#include "imparse.h"
#include "imapconf.h"
#include "exitcodes.h"
#include "imap_err.h"
#include "mailbox.h"
#include "message.h"
#include "xmalloc.h"
#include "imapconf.h"
#include "mboxname.h"
#include "mboxlist.h"
#include "seen.h"
#include "retry.h"
#include "convert_code.h"
#include "util.h"
#include "acapmbox.h"

extern int errno;
extern int optind;
extern char *optarg;

struct discovered {
    char *name;
    struct discovered *next;
};       

/* forward declarations */
void do_mboxlist(void);
int do_reconstruct(char *name, int matchlen, int maycreate, void *rock);
int reconstruct(char *name, struct discovered *l);
void usage(void);
void shut_down(int code);

extern char *mailbox_findquota(const char *name);
extern acl_canonproc_t mboxlist_ensureOwnerRights;

static acapmbox_handle_t *acaphandle = NULL;

int code = 0;

int main(int argc, char **argv)
{
    int opt, i;
    int rflag = 0;
    int mflag = 0;
    int fflag = 0;
    char buf[MAX_MAILBOX_PATH];
    struct discovered head;

    memset(&head, 0, sizeof(head));
    config_init("reconstruct");

    if (geteuid() == 0) fatal("must run as the Cyrus user", EC_USAGE);

    /* Ensure we're up-to-date on the index file format */
    assert(INDEX_HEADER_SIZE == (OFFSET_FLAGGED+4));
    assert(INDEX_RECORD_SIZE == (OFFSET_USER_FLAGS+MAX_USER_FLAGS/8));

    while ((opt = getopt(argc, argv, "rmf")) != EOF) {
	switch (opt) {
	case 'r':
	    rflag = 1;
	    break;

	case 'm':
	    mflag = 1;
	    break;

	case 'f':
	    fflag = 1;
	    break;

	default:
	    usage();
	}
    }

    if (mflag) {
	if (rflag || fflag || optind != argc) usage();
	do_mboxlist();
    }

    mboxlist_init(0);
    mboxlist_open(NULL);

    signals_set_shutdown(&shut_down);
    signals_add_handlers();

    mailbox_reconstructmode();

    if (optind == argc) {
	if (rflag) {
	    fprintf(stderr, "please specify a mailbox to recurse from\n");
	    exit(EC_USAGE);
	}
	assert(!rflag);
	strcpy(buf, "*");
	mboxlist_findall(buf, 1, 0, 0, do_reconstruct, NULL);
    }

    for (i = optind; i < argc; i++) {
	do_reconstruct(argv[i], 0, 0, fflag ? &head : NULL);
	if (rflag) {
	    strcpy(buf, argv[i]);
	    strcat(buf, ".*");
	    mboxlist_findall(buf, 1, 0, 0, do_reconstruct, 
			     fflag ? &head : NULL);
	}
    }

    /* examine our list to see if we discovered anything */
    while (head.next) {
	struct discovered *p;
	int r;

	p = head.next;
	head.next = p->next;

	/* create p and reconstruct it */
	r = mboxlist_createmailbox(p->name, 0, NULL, 1, "cyrus", NULL);
	if (!r) do_reconstruct(p->name, 0, 0, &head);
	/* may have added more things into our list */

	free(p->name);
	free(p);
    }

    mboxlist_close();
    mboxlist_done();

    exit(code);
}

void usage(void)
{
    fprintf(stderr, "usage: reconstruct [-r] [-f] mailbox...\n");
    fprintf(stderr, "       reconstruct -m\n");
    exit(EC_USAGE);
}    

int compare_uid(a, b)
char *a, *b;
{
    return *(unsigned long *)a - *(unsigned long *)b;
}

#define UIDGROW 300


/*
 * mboxlist_findall() callback function to reconstruct a mailbox
 */
int
do_reconstruct(char *name, int matchlen, int maycreate, void *rock)
{
    int r;

    signals_poll();
    r = reconstruct(name, rock);
    if (r) {
	com_err(name, r, (r == IMAP_IOERROR) ? error_message(errno) : NULL);
	code = convert_code(r);
    }
    else {
	printf("%s\n", name);
    }

    return 0;
}

/*
 * Reconstruct the single mailbox named 'name'
 */
int reconstruct(char *name, struct discovered *found)
{
    int r;
    struct mailbox mailbox;
    char *quota_root;
    int i, flag;
    char *p;
    int format = MAILBOX_FORMAT_NORMAL;
    bit32 valid_user_flags[MAX_USER_FLAGS/32];
    char fnamebuf[MAX_MAILBOX_PATH];
    FILE *newindex;
    int newcache_fd;
    char buf[INDEX_HEADER_SIZE > INDEX_RECORD_SIZE ?
	     INDEX_HEADER_SIZE : INDEX_RECORD_SIZE];
    unsigned long *uid;
    int uid_num, uid_alloc;
    DIR *dirp;
    struct dirent *dirent;
    int msg, old_msg = 0;
    int new_exists = 0, 
	new_answered = 0,
	new_flagged = 0,
	new_deleted = 0;
    unsigned long new_quota = 0;
    struct index_record message_index, old_index;
    static struct index_record zero_index;
    char newspath[4096], *end_newspath;
    FILE *msgfile;
    struct stat sbuf;
    int n;

    /* Open/lock header */
    r = mailbox_open_header(name, 0, &mailbox);
    if (r) {
	return r;
    }
    if (mailbox.header_fd != -1) {
	(void) mailbox_lock_header(&mailbox);
    }
    mailbox.header_lock_count = 1;

    if (chdir(mailbox.path) == -1) {
	return IMAP_IOERROR;
    }

    /* Fix quota root */
    quota_root = mailbox_findquota(mailbox.name);
    if (mailbox.quota.root) free(mailbox.quota.root);
    if (quota_root) {
	mailbox.quota.root = xstrdup(quota_root);
    }
    else {
	mailbox.quota.root = 0;
    }

    /* Validate user flags */
    for (i = 0; i < MAX_USER_FLAGS/32; i++) {
	valid_user_flags[i] = 0;
    }
    for (flag = 0; flag < MAX_USER_FLAGS; flag++) {
	if (!mailbox.flagname[flag]) continue;
	if ((flag && !mailbox.flagname[flag-1]) ||
	    !imparse_isatom(mailbox.flagname[flag])) {
	    free(mailbox.flagname[flag]);
	    mailbox.flagname[flag] = 0;
	}
	valid_user_flags[flag/32] |= 1<<(flag&31);
    }
    
    /* Write header */
    r = mailbox_write_header(&mailbox);
    if (r) {
	mailbox_close(&mailbox);
	return r;
    }

    /* Attempt to open/lock index */
    r = mailbox_open_index(&mailbox);
    if (r) {
	mailbox.exists = 0;
	mailbox.last_uid = 0;
	mailbox.last_appenddate = 0;
	mailbox.uidvalidity = time(0);
    }
    else {
	(void) mailbox_lock_index(&mailbox);
    }
    mailbox.index_lock_count = 1;
    mailbox.pop3_last_login = 0;

    /* Create new index/cache files */
    strcpy(fnamebuf, FNAME_INDEX+1);
    strcat(fnamebuf, ".NEW");
    newindex = fopen(fnamebuf, "w+");
    if (!newindex) {
	mailbox_close(&mailbox);
	return IMAP_IOERROR;
    }

    strcpy(fnamebuf, FNAME_CACHE+1);
    strcat(fnamebuf, ".NEW");
    newcache_fd = open(fnamebuf, O_RDWR|O_TRUNC|O_CREAT, 0666);
    if (newcache_fd == -1) {
	fclose(newindex);
	mailbox_close(&mailbox);
	return IMAP_IOERROR;
    }
    
    memset(buf, 0, sizeof(buf));
    (*(bit32 *)buf) = mailbox.generation_no + 1;
    fwrite(buf, 1, INDEX_HEADER_SIZE, newindex);
    retry_write(newcache_fd, buf, sizeof(bit32));

    /* Find all message files in directory */
    uid = (unsigned long *) xmalloc(UIDGROW * sizeof(unsigned long));
    uid_num = 0;
    uid_alloc = UIDGROW;
    dirp = opendir(".");
    end_newspath = newspath;
    if (!dirp) {
	fclose(newindex);
	close(newcache_fd);
	mailbox_close(&mailbox);
	free(uid);
	return IMAP_IOERROR;
    } else {
	while ((dirent = readdir(dirp))!=NULL) {
	    if (!isdigit((int) (dirent->d_name[0])) || dirent->d_name[0] ==
		'0')
		continue;
	    if (uid_num == uid_alloc) {
		uid_alloc += UIDGROW;
		uid = (unsigned long *)
		    xrealloc((char *)uid, uid_alloc * sizeof(unsigned long));
	    }
	    uid[uid_num] = 0;
	    p = dirent->d_name;
	    while (isdigit((int) *p)) {
		uid[uid_num] = uid[uid_num] * 10 + *p++ - '0';
	    }
	    if (*p++ != '.') continue;
	    if (*p) continue;
	    
	    uid_num++;
	}
	closedir(dirp);
	qsort((char *)uid, uid_num, sizeof(*uid), compare_uid);
    }

    /* Put each message file in the new index/cache */
    old_msg = 0;
    old_index.uid = 0;
    mailbox.format = format;
    if (mailbox.cache_fd) close(mailbox.cache_fd);
    mailbox.cache_fd = newcache_fd;

    for (msg = 0; msg < uid_num; msg++) {
	message_index = zero_index;
	message_index.uid = uid[msg];
	
	msgfile = fopen(mailbox_message_fname(&mailbox, uid[msg]), "r");
	if (!msgfile) continue;
	if (fstat(fileno(msgfile), &sbuf)) {
	    fclose(msgfile);
	    continue;
	}
	if (sbuf.st_size == 0) {
	    /* Zero-length message file--blow it away */
	    fclose(msgfile);
	    unlink(mailbox_message_fname(&mailbox, uid[msg]));
	    continue;
	}

	/* Find old index record, if it exists */
	while (old_msg < mailbox.exists && old_index.uid < uid[msg]) {
	    if (mailbox_read_index_record(&mailbox, ++old_msg, &old_index)) {
		old_index.uid = 0;
	    }
	}

	if (old_index.uid == uid[msg]) {
	    /* Use data in old index file, subject to validity checks */
	    message_index.internaldate = old_index.internaldate;
	    message_index.system_flags = old_index.system_flags &
	      (FLAG_ANSWERED|FLAG_FLAGGED|FLAG_DELETED|FLAG_DRAFT);
	    for (i = 0; i < MAX_USER_FLAGS/32; i++) {
		message_index.user_flags[i] =
		  old_index.user_flags[i] & valid_user_flags[i];
	    }
	}
	else {
	    /* Message file write time is good estimate of internaldate */
	    message_index.internaldate = sbuf.st_mtime;
	}
	message_index.last_updated = time(0);
	
	if ((r = message_parse_file(msgfile, &mailbox, &message_index))!=0) {
	    fclose(msgfile);
	    fclose(newindex);
	    mailbox_close(&mailbox);
	    free(uid);
	    return r;
	}
	fclose(msgfile);
	
	/* Write out new entry in index file */
	*((bit32 *)(buf+OFFSET_UID)) = htonl(message_index.uid);
	*((bit32 *)(buf+OFFSET_INTERNALDATE)) = htonl(message_index.internaldate);
	*((bit32 *)(buf+OFFSET_SENTDATE)) = htonl(message_index.sentdate);
	*((bit32 *)(buf+OFFSET_SIZE)) = htonl(message_index.size);
	*((bit32 *)(buf+OFFSET_HEADER_SIZE)) = htonl(message_index.header_size);
	*((bit32 *)(buf+OFFSET_CONTENT_OFFSET)) = htonl(message_index.content_offset);
	*((bit32 *)(buf+OFFSET_CACHE_OFFSET)) = htonl(message_index.cache_offset);
	*((bit32 *)(buf+OFFSET_LAST_UPDATED)) = htonl(message_index.last_updated);
	*((bit32 *)(buf+OFFSET_SYSTEM_FLAGS)) = htonl(message_index.system_flags);
	for (i = 0; i < MAX_USER_FLAGS/32; i++) {
	    *((bit32 *)(buf+OFFSET_USER_FLAGS+4*i)) = htonl(message_index.user_flags[i]);
	}
	n = fwrite(buf, 1, INDEX_RECORD_SIZE, newindex);
	if (n != INDEX_RECORD_SIZE) {
	    fclose(newindex);
	    mailbox_close(&mailbox);
	    free(uid);
	    return IMAP_IOERROR;
	}

	new_exists++;
	if (message_index.system_flags & FLAG_ANSWERED) new_answered++;
	if (message_index.system_flags & FLAG_FLAGGED) new_flagged++;
	if (message_index.system_flags & FLAG_DELETED) new_deleted++;
	new_quota += message_index.size;
    }
    
    /* Write out new index file header */
    rewind(newindex);
    if (uid_num && mailbox.last_uid < uid[uid_num-1]) {
	mailbox.last_uid = uid[uid_num-1] + 100;
    }
    if (mailbox.last_appenddate == 0 || mailbox.last_appenddate > time(0)) {
	mailbox.last_appenddate = time(0);
    }
    if (mailbox.uidvalidity == 0 || mailbox.uidvalidity > time(0)) {
	mailbox.uidvalidity = time(0);
    }
    free(uid);
    *((bit32 *)(buf+OFFSET_GENERATION_NO)) = mailbox.generation_no + 1;
    *((bit32 *)(buf+OFFSET_FORMAT)) = htonl(mailbox.format);
    *((bit32 *)(buf+OFFSET_MINOR_VERSION)) = htonl(MAILBOX_MINOR_VERSION);
    *((bit32 *)(buf+OFFSET_START_OFFSET)) = htonl(INDEX_HEADER_SIZE);
    *((bit32 *)(buf+OFFSET_RECORD_SIZE)) = htonl(INDEX_RECORD_SIZE);
    *((bit32 *)(buf+OFFSET_EXISTS)) = htonl(new_exists);
    *((bit32 *)(buf+OFFSET_LAST_APPENDDATE)) = htonl(mailbox.last_appenddate);
    *((bit32 *)(buf+OFFSET_LAST_UID)) = htonl(mailbox.last_uid);
    *((bit32 *)(buf+OFFSET_QUOTA_MAILBOX_USED)) = htonl(new_quota);
    *((bit32 *)(buf+OFFSET_POP3_LAST_LOGIN)) = htonl(mailbox.pop3_last_login);
    *((bit32 *)(buf+OFFSET_UIDVALIDITY)) = htonl(mailbox.uidvalidity);
    *((bit32 *)(buf+OFFSET_DELETED)) = htonl(new_deleted);
    *((bit32 *)(buf+OFFSET_ANSWERED)) = htonl(new_answered);
    *((bit32 *)(buf+OFFSET_FLAGGED)) = htonl(new_flagged);

    n = fwrite(buf, 1, INDEX_HEADER_SIZE, newindex);
    fflush(newindex);
    if (n != INDEX_HEADER_SIZE || ferror(newindex) 
	|| fsync(fileno(newindex)) || fsync(newcache_fd)) {
	fclose(newindex);
	mailbox_close(&mailbox);
	return IMAP_IOERROR;
    }

    /* Rename new index/cache file in place */
    strcpy(fnamebuf, FNAME_INDEX+1);
    strcat(fnamebuf, ".NEW");
    if (rename(fnamebuf, FNAME_INDEX+1)) {
	fclose(newindex);
	mailbox_close(&mailbox);
	return IMAP_IOERROR;
    }
    strcpy(fnamebuf, FNAME_CACHE+1);
    strcat(fnamebuf, ".NEW");
    if (rename(fnamebuf, FNAME_CACHE+1)) {
	fclose(newindex);
	mailbox_close(&mailbox);
	return IMAP_IOERROR;
    }
    
    acapmbox_setproperty(acaphandle, mailbox.name, ACAPMBOX_UIDVALIDITY,
			 mailbox.uidvalidity);

    fclose(newindex);
    r = seen_reconstruct(&mailbox, (time_t)0, (time_t)0, (int (*)())0, (void *)0);
    mailbox_close(&mailbox);

    if (found) {
	/* we recurse down this directory to see if there's any mailboxes
	   under this not in the mailboxes database */
	dirp = opendir(".");

	while ((dirent = readdir(dirp)) != NULL) {
	    struct discovered *new;

	    /* mailbox directories never have a dot in them */
	    if (strchr(dirent->d_name, '.')) continue;
	    if (stat(dirent->d_name, &sbuf) < 0) continue;
	    if (!S_ISDIR(sbuf.st_mode)) continue;

	    /* ok, we found a directory that doesn't have a dot in it */
	    strcpy(fnamebuf, name);
	    strcat(fnamebuf, ".");
	    strcat(fnamebuf, dirent->d_name);

	    /* does fnamebuf exist as a mailbox? */
	    do {
		r = mboxlist_lookup(fnamebuf, NULL, NULL, NULL);
	    } while (r == IMAP_AGAIN);
	    if (!r) continue; /* mailbox exists; it'll be reconstructed
			         with a -r */
	    if (r != IMAP_MAILBOX_NONEXISTENT) break; /* erg? */
	    printf("discovered %s\n", fnamebuf);
	    new = (struct discovered *) xmalloc(sizeof(struct discovered));
	    new->name = strdup(fnamebuf);
	    new->next = found->next;
	    found->next = new;
	}
	closedir(dirp);
    }

    return r;
}

void do_mboxlist(void)
{
    fprintf(stderr, "reconstructing mailboxes.db currently not supported\n");
    exit(EC_USAGE);
}

/*
 * Cleanly shut down and exit
 */
void shut_down(int code) __attribute__((noreturn));
void shut_down(int code)
{
    mboxlist_close();
    mboxlist_done();
    exit(code);
}

void fatal(const char* s, int code)
{
    static int recurse_code = 0;
    
    if (recurse_code) {
	/* We were called recursively. Just give up */
	exit(recurse_code);
    }
    
    recurse_code = code;
    fprintf(stderr, "reconstruct: %s\n", s);
    shut_down(code);
}
