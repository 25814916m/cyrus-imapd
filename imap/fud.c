/* fud.c -- long-lived finger information provider
 *
 * Copyright 1998 Carnegie Mellon University
 * 
 * No warranties, either expressed or implied, are made regarding the
 * operation, use, or results of the software.
 *
 * Permission to use, copy, modify and distribute this software and its
 * documentation is hereby granted for non-commercial purposes only
 * provided that this copyright notice appears in all copies and in
 * supporting documentation.
 *
 * Permission is also granted to Internet Service Providers and others
 * entities to use the software for internal purposes.
 *
 * The distribution, modification or sale of a product which uses or is
 * based on the software, in whole or in part, for commercial purposes
 * or
 * benefits requires specific, additional permission from:
 *
 *  Office of Technology Transfer
 *  Carnegie Mellon University
 *  5000 Forbes Avenue
 *  Pittsburgh, PA  15213-3890
 *  (412) 268-4387, fax: (412) 268-7395
 *  tech-transfer@andrew.cmu.edu
 *
 */

/* $Id: fud.c,v 1.11 2000/02/10 08:00:20 leg Exp $ */
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <syslog.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <com_err.h>
#include <pwd.h>

#include "assert.h"
#include "config.h"
#include "exitcodes.h"
#include "imap_err.h"
#include "mailbox.h"
#include "xmalloc.h"
#include "acl.h"
#include "seen.h"
#include "mboxname.h"
#include "map.h"

#define REQ_OK		0
#define REQ_DENY	1
#define REQ_UNK		2

extern int errno;
extern int optind;
extern char *optarg;

/* forward decls */
int handle_request(char *who,char *name,struct sockaddr_in sfrom);

void send_reply P((struct sockaddr_in sfrom, int status,
		   char *user, char *mbox, int numrecent, time_t lastread,
		   time_t lastarrived));

int code = 0;
int soc;

char who[16];

int init_network(int port)
{
    soc = 0;	/* inetd has handed us the port as stdin */
    return(0);
}

#define MAXLOGNAME 16		/* should find out for real */

int begin_handling(void)
{
        struct sockaddr_in  sfrom;
        int sfromsiz = sizeof(sfrom);
        int r;
        char    buf[MAXLOGNAME + MAX_MAILBOX_NAME + 1];
        char    username[MAXLOGNAME];
        char    mbox[MAX_MAILBOX_NAME+1];
        char    *q;
        int     off;
        
        while(1) {
            /* For safety */
            memset(username,'\0',MAXLOGNAME);	
            memset(mbox,'\0',MAX_MAILBOX_NAME+1);
            memset(buf, '\0', MAXLOGNAME + MAX_MAILBOX_NAME + 1);

            r = recvfrom(soc, buf, 511, 0, (struct sockaddr *) &sfrom, &sfromsiz);
            if(r == -1)
                    return(errno);
            for(off = 0; buf[off] != '|' && off < MAXLOGNAME; off++);
            if(off < MAXLOGNAME) {
                    strncpy(username,buf,off);
            } else {
                    continue;
            }
            q = buf + off + 1;
            strncpy(mbox,q,(r - (off + 1)  < MAX_MAILBOX_NAME) ? r - (off + 1) : MAX_MAILBOX_NAME);

            handle_request(username,mbox,sfrom);
        }
}



int main(int argc, char **argv)
{
    int port = 0;
    int r;
   
    r = 0; /* to shut up lint/gcc */

    config_init("fud");

    if(geteuid() == 0)
        fatal("must run as the Cyrus user", EC_USAGE);
    r = init_network(port);
    signal(SIGHUP,SIG_IGN);

    if (r)
        fatal("unable to configure network port", EC_OSERR);
    
    begin_handling();

    exit(code);
}






int handle_request(char *who,char *name,struct sockaddr_in sfrom)
{
    int r;
    struct mailbox mailbox;
    struct seen *seendb;
    time_t lastread;
    time_t lastarrived;
    unsigned lastuid;
    char *seenuids;
    unsigned numrecent;
    char mboxname[MAX_MAILBOX_NAME+1];

    numrecent = 0;
    lastread = 0;
    lastarrived = 0;

    r = mboxname_tointernal(name,who,mboxname);
    if (r) return r; 

    /*
     * Open/lock header 
     */
    r = mailbox_open_header(mboxname, 0, &mailbox);
    if (r) {
        send_reply(sfrom, REQ_UNK, who, name, 0, 0, 0);
	return r; 
    }
    r = mailbox_open_index(&mailbox);

    if (r) {
	mailbox_close(&mailbox);
	return r;
    }

    if(!(strncmp(mboxname,"user.",5)) && !(mailbox.myrights & ACL_USER0)) {
        send_reply(sfrom, REQ_DENY, who, name, 0, 0, 0);
    }
   

    r = seen_open(&mailbox, who, &seendb);
    if (r) return r;
    r = seen_lockread(seendb, &lastread, &lastuid, &lastarrived, &seenuids);
    seen_close(seendb);
    if (r) return r;
    
    lastarrived = mailbox.last_appenddate;
    {
        const char *base;
        unsigned long len = 0;
        int msg;
        unsigned uid;
         
        map_refresh(mailbox.index_fd, 0, &base, &len,
                    mailbox.start_offset + mailbox.exists * 
                    mailbox.record_size,  "index",
                    mailbox.name);

        for (msg = 0; msg < mailbox.exists; msg++) {
                uid = ntohl(*((bit32 *)(base + mailbox.start_offset +
                                        msg * mailbox.record_size +
                                        OFFSET_UID)));
                if (uid > lastuid) numrecent++;
	}
        map_free(&base,&len);
        free(seenuids);
    }

    mailbox_close(&mailbox);
    
    send_reply(sfrom, REQ_OK, who, name, numrecent, lastread, lastarrived);
    
    return(0);
}

void
send_reply(sfrom, status, user, mbox, numrecent, lastread, lastarrived)
struct sockaddr_in sfrom;
int status; 
char *user; 
char *mbox; 
int numrecent; 
time_t lastread; 
time_t lastarrived;
{
    char buf[MAX_MAILBOX_PATH + 16 + 9];
    int siz;

    switch(status) {
        case REQ_DENY:
            sendto(soc,"PERMDENY",9,0,(struct sockaddr *) &sfrom, sizeof(sfrom));       
            break;
        case REQ_OK:
            siz = sprintf(buf,"%s|%s|%d|%d|%d",user,mbox,numrecent,(int) lastread,(int) lastarrived);
            sendto(soc,buf,siz,0,(struct sockaddr *) &sfrom, sizeof(sfrom));       
            break;
        case REQ_UNK:
            sendto(soc,"UNKNOWN",8,0,(struct sockaddr *) &sfrom, sizeof(sfrom));       
            break;
    } 
}

void fatal(const char* s, int code)
{
    fprintf(stderr, "fud: %s\n", s);
    exit(code);
}
