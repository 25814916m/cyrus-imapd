/*
 * ipurge
 *
 * delete mail from cyrus imap mailbox or partition
 * based on date (or size?)
 *
 * includes support for ISPN virtual host extensions
 *
 * $Id: ipurge.c,v 1.3 2000/02/17 02:48:27 leg Exp $
 *
 */

#include <config.h>

#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#include <stdlib.h>
#include <stdio.h>
#include <syslog.h>
#include <com_err.h>
#include <string.h>
#include <time.h>

/* cyrus includes */
#include "config.h"
#include "sysexits.h"
#include "imap_err.h"
#include "mailbox.h"
#include "xmalloc.h"
#include "mboxlist.h"

/* globals for getopt routines */
extern char *optarg;
extern int  optind;
extern int  opterr;
extern int  optopt;

/* globals for callback functions */
int days = -1;
int size = -1;
int exact = -1;
int pattern = -1;

/* for statistical purposes */
typedef struct mbox_stats_s {

    int total;         /* total including those deleted */
    int total_bytes;
    int deleted;       
    int deleted_bytes;

} mbox_stats_t;

int verbose = 1;

int purge_me(char *, int, int);
int purge_check(void *, char *);
int usage(char *name);
void print_stats(mbox_stats_t *stats);

int
main (int argc, char *argv[]) {
  char option;
  char buf[MAX_MAILBOX_PATH];

  if (geteuid() == 0) { /* don't run as root, changes permissions */
    usage(argv[0]);
  }

  while ((option = getopt(argc, argv, "hxd:b:k:m:")) != EOF) {
    switch (option) {
    case 'd': {
      if (optarg == 0) {
	usage(argv[0]);
      }
      days = atoi(optarg) * 86400 /* nominal # of seconds in a 'day' */;
    } break;
    case 'b': {
      if (optarg == 0) {
	usage(argv[0]);
      }
      size = atoi(optarg);
    } break;
    case 'k': {
      if (optarg == 0) {
	usage(argv[0]);
      }
      size = atoi(optarg) * 1024; /* make it bytes */
    } break;
    case 'm': {
      if (optarg == 0) {
	usage(argv[0]);
      }
      size = atoi(optarg) * 1048576; /* 1024 * 1024 */
    } break;
    case 'x' : {
      exact = 1;
    } break;
    case 'h':
    default: usage(argv[0]);
    }
  }
  if ((days == -1 ) && (size == -1)) {
    printf("One of these must be specified -d, -b -k, -m\n");
    usage(argv[0]);
  }

  config_init("ipurge");

  if (geteuid() == 0) fatal("must run as the Cyrus user", EX_USAGE);

  if (optind == argc) { /* do the whole partition */
    strcpy(buf, "*");
    mboxlist_findall(buf, 1, 0, 0, purge_me, NULL);
  } else {
    for (; optind < argc; optind++) {
      strncpy(buf, argv[optind], MAX_MAILBOX_NAME);
      mboxlist_findall(buf, 1, 0, 0, purge_me, NULL);
    }
  }
  return 0;
}

int
usage(char *name) {
  printf("usage: %s [-x] {-d days &| -b bytes|-k Kbytes|-m Mbytes}\n\t[mboxpattern1 ... [mboxpatternN]]\n", name);
  printf("\tthere are no defaults and at least one of -d, -b, -k, -m\n\tmust be specified\n");
  printf("\tif no mboxpattern is given %s works on all mailboxes\n", name);
  printf("\t -x specifies an exact match for days or size\n");
  exit(0);
}

/* we don't check what comes in on matchlen and maycreate, should we? */
int
purge_me(char *name, int matchlen, int maycreate) {
  struct mailbox the_box;
  int            error;
  mbox_stats_t   stats;

  /* DON'T purge INBOX* and user.* */
  if ((strncasecmp(name,"INBOX",5)==0) || (strncasecmp(name,"user.",5)==0))
      return 0;

  memset(&stats, '\0', sizeof(mbox_stats_t));

  if (verbose)
      printf("Working on %s...\n",name);

  error = mailbox_open_header(name, 0, &the_box);
  if (error != 0) { /* did we find it? */
    syslog(LOG_ERR, "Couldn't find %s, check spelling (user.????)", name);
    return error;
  }
  if (the_box.header_fd != -1) {
    (void) mailbox_lock_header(&the_box);
  }
  the_box.header_lock_count = 1;

  error = chdir(the_box.path);
  if (error < 0) {
    syslog(LOG_ERR, "Couldn't change directory to %s : %m", the_box.path);
    return error;
  }
  error = mailbox_open_index(&the_box);
  if (error != 0) {
    mailbox_close(&the_box);
    syslog(LOG_ERR, "Couldn't open mailbox index for %s", name);
    return error;
  }
  (void) mailbox_lock_index(&the_box);
  the_box.index_lock_count = 1;

  mailbox_expunge(&the_box, 1, purge_check, &stats);
  mailbox_close(&the_box);

  print_stats(&stats);

  return 0;
}

void deleteit(struct index_record *the_record, mbox_stats_t *stats)
{
    stats->deleted++;
    stats->deleted_bytes+=the_record->size;
}

/* thumbs up routine, checks date & size and returns yes or no for deletion */
/* 0 = no, 1 = yes */
int
purge_check(void *deciderock, char *buf) {
  struct index_record *the_record;
  unsigned long       my_time;
  mbox_stats_t *stats = (mbox_stats_t *) deciderock;

  the_record = (struct index_record *)buf;

  stats->total++;
  stats->total_bytes+=the_record->size;

  if (exact == 1) {
    if (days >= 0) {
      my_time = time(0);
      /*    printf("comparing %ld :: %ld\n", my_time, the_record->sentdate); */
      if (((my_time - the_record->sentdate)/86400) == (days/86400)) {
	  deleteit(the_record, stats);
	  return 1;
      }
    }
    if (size >= 0) {
      /* check size */
      if (the_record->size == size) {
	  deleteit(the_record, stats);
	  return 1;
      }
    }
    return 0;
  } else {
    if (days >= 0) {
      my_time = time(0);
      /*    printf("comparing %ld :: %ld\n", my_time, the_record->sentdate); */
      if ((my_time - the_record->sentdate) > days) {
	  deleteit(the_record, stats);
	  return 1;
      }
    }
    if (size >= 0) {
      /* check size */
      if (the_record->size > size) {
	  deleteit(the_record, stats);
	  return 1;
      }
    }
    return 0;
  }
}

void print_stats(mbox_stats_t *stats)
{
    printf("total messages    \t\t %d\n",stats->total);
    printf("total bytes       \t\t %d\n",stats->total_bytes);
    printf("Deleted messages  \t\t %d\n",stats->deleted);
    printf("Deleted bytes     \t\t %d\n",stats->deleted_bytes);
    printf("Remaining messages\t\t %d\n",stats->total - stats->deleted);
    printf("Remaining bytes   \t\t %d\n",stats->total_bytes - stats->deleted_bytes);
}

/* fatal needed for imap library */
void
fatal(const char *s, int code) {
  fprintf(stderr, "ipurge: %s\n", s);
  exit(code);
}
