/*
 * Perl interface to the Cyrus imclient routines.  This enables the
 * use of Perl to implement Cyrus utilities, in particular imtest and cyradm.
 */

#include "EXTERN.h"
#include "perl.h"
#include "XSUB.h"
#include <pwd.h>
#include "cyrus/imclient.h"
#include "cyrperl.h"

typedef struct xscyrus *IMAP_Cyrus;

MODULE = IMAP::Cyrus	PACKAGE = IMAP::Cyrus	
PROTOTYPES: ENABLE

int
CONN_NONSYNCLITERAL()
PPCODE:
	RETVAL = IMCLIENT_CONN_NONSYNCLITERAL;

int
CONN_INITIALRESPONSE()
PPCODE:
	RETVAL = IMCLIENT_CONN_INITIALRESPONSE;

int
CALLBACK_NUMBERED()
PPCODE:
	RETVAL = CALLBACK_NUMBERED;

int
CALLBACK_NOLITERAL()
PPCODE:
	RETVAL = CALLBACK_NOLITERAL;

MODULE = IMAP::Cyrus	PACKAGE = IMAP::Cyrus	PREFIX=imclient_
PROTOTYPES: ENABLE

SV *
imclient_new(class, host = "localhost", port = 0, flags = 0)
	char *class;
	char *host
	char *port
	int flags;
PREINIT:
	struct imclient *client;
	int rc;
	SV *bang;
	IMAP_Cyrus rv;
CODE:
	rc = imclient_connect(&client, host, port);
	switch (rc) {
	case -1:
	  Perl_croak("imclient_connect: unknown host \"%s\"", host);
	  break;
	case -2:
	  Perl_croak("imclient_connect: unknown service \"%s\", port");
	  break;
	case 0:
	  if (client) {
	    rv = safemalloc(sizeof *rv);
	    rv->imclient = client;
	    rv->class = safemalloc(strlen(class) + 1);
	    strcpy(rv->class, class);
	    rv->cb = 0;
	    imclient_setflags(client, flags);
	    rv->flags = flags;
	    rv->cnt = 1;
	    break;
	  }
	  /*FALLTHROUGH*/
	default:
	  bang = perl_get_sv("^E", TRUE);
	  Perl_sv_setiv(bang, rc);
	  XSRETURN_UNDEF;
	}
	ST(0) = sv_newmortal();
/*fprintf(stderr, "!NEW %p\n", rv);*/
	sv_setref_pv(ST(0), class, (void *) rv);

void
imclient_DESTROY(client)
	IMAP_Cyrus client
PREINIT:
	struct xscb *nx;
CODE:
/*fprintf(stderr, "!DESTROY %p %d\n", client, client->cnt);*/
	if (!--client->cnt) {
	  imclient_close(client->imclient);
	  while (client->cb) {
	    nx = client->cb->next;
	    if (client->cb->name) safefree(client->cb->name);
	    if (client->cb->rock->pcb) SvREFCNT_dec(client->cb->rock->pcb);
	    if (client->cb->rock->prock) SvREFCNT_dec(client->cb->rock->prock);
	    safefree(client->cb->rock);
	    client->cb = nx;
	  }
	  safefree(client->class);
	  safefree(client);
	}

void
imclient_setflags(client, flags)
	IMAP_Cyrus client
	int flags
PPCODE:
	imclient_setflags(client->imclient, flags);
	client->flags |= flags;

void
imclient_clearflags(client, flags)
	IMAP_Cyrus client
	int flags
PPCODE:
	imclient_clearflags(client->imclient, flags);
	client->flags &= ~flags;

int
imclient_flags(client)
	IMAP_Cyrus client
PPCODE:
	/* why is there no way to query this? */
	RETVAL = client->flags;

char *
imclient_servername(client)
	IMAP_Cyrus client
PREINIT:
	char *cp;
CODE:
	cp = imclient_servername(client->imclient);
	RETVAL = cp;
OUTPUT:
	RETVAL

void
imclient_processoneevent(client)
	IMAP_Cyrus client
PPCODE:
	imclient_processoneevent(client->imclient);

SV *
imclient__authenticate(client, mechlist, service, user, minssf, maxssf)
	IMAP_Cyrus client
	char* mechlist
	char* service
	char* user
	int minssf
	int maxssf
PREINIT:
	int rc;
CODE:
	rc = imclient_authenticate(client->imclient, mechlist, service, user,
				   minssf, maxssf);
	ST(0) = sv_newmortal();
	if (rc)
	  ST(0) = &sv_no;
	else
	  ST(0) = &sv_yes;

void
imclient_addcallback(client, ...)
	IMAP_Cyrus client
PREINIT:
	int arg;
	HV *cb;
	char *keyword;
	int klen;
	int flags;
	SV **val;
	SV *pcb;
	SV *prock;
	struct xsccb *rock;
	struct xscb *xcb;
PPCODE:
	/*
	 * $client->addcallback(\%cb[, ...]);
	 *
	 * where %cb is:
	 *
	 * -trigger => 'OK' (or 'NO', etc.)
	 * -flags => CALLBACK_NOLITERAL|CALLBACK_NUMBERED (optional)
	 * -callback => \&sub or undef (optional)
	 * -rock => SV, reference or undef (optional)
	 *
	 * this is moderately complicated because the callback is a Perl ref...
	 */
	for (arg = 1; arg < items; arg++) {
	  if (!SvROK(ST(arg)) || SvTYPE(SvRV(ST(arg))) != SVt_PVHV)
	    Perl_croak("addcallback: arg %d not a hash reference", arg);
	  cb = (HV *) SvRV(ST(arg));
	  /* pull callback crud */
	  if (((val = hv_fetch(cb, "-trigger", 8, 0)) ||
	       (val = hv_fetch(cb, "Trigger", 7, 0))) &&
	      SvTYPE(*val) == SVt_PV)
	    keyword = SvPV(*val, klen);
	  else
	    Perl_croak("addcallback: arg %d missing trigger", arg);
	  if (!(((val = hv_fetch(cb, "-flags", 6, 0)) ||
		 (val = hv_fetch(cb, "Flags", 5, 0))) &&
		SvTYPE(*val) == SVt_IV &&
		!((flags = SvIV(*val)) &
		  ~(CALLBACK_NUMBERED|CALLBACK_NOLITERAL))))
	    flags = 0;
	  if (((val = hv_fetch(cb, "-callback", 9, 0)) ||
	       (val = hv_fetch(cb, "Callback", 8, 0))) &&
	      ((SvROK(*val) && SvTYPE(SvRV(*val)) == SVt_PVCV) ||
	       SvTYPE(*val) == SVt_PV))
	    pcb = *val;
	  else
	    pcb = 0;
	  if ((val = hv_fetch(cb, "-rock", 5, 0)) ||
	      (val = hv_fetch(cb, "Rock", 4, 0)))
	    prock = *val;
	  else
	    prock = &sv_undef;
	  /*
	   * build our internal rock, which is used by our internal
	   * callback handler to invoke the Perl callback
	   */
	  if (!pcb)
	    rock = 0;
	  else {
	    rock = (struct xsccb *) safemalloc(sizeof *rock);
	    /* bump refcounts on these so they don't go away */
	    rock->pcb = SvREFCNT_inc(pcb);
	    if (!prock) prock = &sv_undef;
	    rock->prock = SvREFCNT_inc(prock);
	    rock->client = client;
	    rock->autofree = 0;
	  }
	  /* and add the resulting callback */
	  imclient_addcallback(client->imclient, keyword, flags,
			       (pcb ? imclient_xs_cb : 0), rock, 0);
	  /* update the callback list, possibly freeing old callback info */
	  for (xcb = client->cb; xcb; xcb = xcb->next) {
	    if (xcb->name && strcmp(xcb->name, keyword) == 0 &&
		xcb->flags == flags)
	      break;
	  }
	  if (xcb) {
	    if (xcb->rock->pcb) SvREFCNT_dec(xcb->rock->pcb);
	    if (xcb->rock->prock) SvREFCNT_dec(xcb->rock->prock);
	    safefree(xcb->rock);
	  }
	  else if (pcb) {
	    xcb = (struct xscb *) safemalloc(sizeof *xcb);
	    xcb->prev = 0;
	    xcb->name = safemalloc(strlen(keyword) + 1);
	    strcpy(xcb->name, keyword);
	    xcb->flags = flags;
	    xcb->next = client->cb;
	    client->cb = xcb;
	  }
	  if (pcb)
	    xcb->rock = rock;
	  else if (xcb) {
	    if (xcb->prev)
	      xcb->prev->next = xcb->next;
	    else
	      client->cb = xcb->next;
	    if (xcb->next) xcb->next->prev = xcb->prev;
	    safefree(xcb->name);
	    safefree(xcb);
	  }
	}
	
void
imclient__send(client, finishproc, finishrock, str)
	IMAP_Cyrus client
	SV *finishproc
	SV *finishrock
	char *str
PREINIT:
	int arg;
	SV *pcb;
	SV *prock;
	struct xscb *xcb;
	struct xsccb *rock;
	char *cp, *dp, *xstr;
PPCODE:
	/*
	 * The C version does escapes.  It also does varargs, which I would
	 * much rather not have to reimplement in XS code; so that is done in
	 * Perl instead.  (The minus being that I have to track any changes
	 * to the C API; but it'll be easier in Perl than in XS.)
	 *
	 * We still have to do the callback, though.
	 *
	 * @@@ the Perl code can't do synchronous literals
	 */
	if (SvROK(finishproc) && SvTYPE(SvRV(finishproc)) == SVt_PVCV)
	  pcb = SvRV(finishproc);
	else
	  pcb = 0;
	if (!pcb)
	  prock = newRV_inc(&sv_undef);
	else if (finishrock)
	  prock = finishrock;
	else
	  prock = &sv_undef;
	/*
	 * build our internal rock, which is used by our internal
	 * callback handler to invoke the Perl callback
	 */
	rock = (struct xsccb *) safemalloc(sizeof *rock);
        /* bump refcounts on these so they don't go away */
	if (!pcb) pcb = &sv_undef;
	rock->pcb = SvREFCNT_inc(pcb);
	if (!prock) prock = &sv_undef;
	rock->prock = SvREFCNT_inc(prock);
	rock->client = client;
	rock->autofree = 1;
	/* register this callback so it can be gc'ed properly (pointless?) */
	xcb = (struct xscb *) safemalloc(sizeof *xcb);
	xcb->prev = 0;
	xcb->name = 0;
	xcb->flags = 0;
	xcb->rock = rock;
	xcb->next = client->cb;
	client->cb = xcb;
	/* protect %'s in the string, since the caller does the dirty work */
	arg = 0;
	for (cp = str; *cp; cp++)
	  if (*cp == '%') arg++;
	xstr = safemalloc(strlen(str) + arg + 1);
	dp = xstr;
	for (cp = str; *cp; cp++) {
	  *dp++ = *cp;
	  if (*cp == '%') *dp++ = *cp;
	}
	*dp = 0;
	/* and do it to it */
	imclient_send(client->imclient,
		      (pcb == &sv_undef ?
		       imclient_xs_fcmdcb :
		       imclient_xs_cb),
		      rock, xstr);
	safefree(xstr);
	/* if there was no Perl callback, spin on events until finished */
	if (pcb == &sv_undef) {
	  AV *av;
	  while (SvTYPE(SvRV(prock)) != SVt_PVAV)
	    imclient_processoneevent(client->imclient);
	  /* push the result; if scalar, stuff text in $@ */
	  av = (AV *) SvRV(prock);
	  if (GIMME_V == G_SCALAR) {
	    EXTEND(SP, 1);
	    pcb = av_shift(av);
	    if (strcmp(SvPV(pcb, arg), "OK") == 0)
	      PUSHs(&sv_yes);
	    else
	      PUSHs(&sv_no);
	    pcb = perl_get_sv("@", TRUE);
	    Perl_sv_setsv(pcb, av_shift(av));
	    if (av_len(av) != -1) {
	      pcb = perl_get_sv("^E", TRUE);
	      Perl_sv_setsv(pcb, av_shift(av));
	    }
	  } else {
	    EXTEND(SP, av_len(av) + 1);
	    PUSHs(av_shift(av));
	    PUSHs(av_shift(av));
	    if (av_len(av) != -1) PUSHs(av_shift(av));
	  }
	  /* and free it */
	  SvREFCNT_dec(prock);
	}

void
imclient_getselectinfo(client)
	IMAP_Cyrus client
PREINIT:
	int fd, writep;
PPCODE:
	imclient_getselectinfo(client->imclient, &fd, &writep);
	/*
	 * should this return a glob?  (evil, but would solve a nasty issue
	 * in &send()...)
	 *
	 * also, should this check for scalar context and complain?
	 */
	EXTEND(SP, 2);
	PUSHs(sv_2mortal(newSViv(fd)));
	if (writep)
	  PUSHs(&sv_yes);
	else
	  PUSHs(&sv_no);
