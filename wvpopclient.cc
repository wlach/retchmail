/*
 * Worldvisions Weaver Software:
 *   Copyright (C) 1997-2003 Net Integration Technologies, Inc.
 *
 * Avery's insanely fast alternative to Fetchmail -- Pop3 Client
 *
 * This code is LGPL - see the file COPYING.LIB for a full copy of the
 * license.
 */

#include "wvstring.h"
#include "wvpopclient.h"
#include "wvdigest.h"
#include "wvhex.h"
#include "strutils.h"

#define MAX_PROCESSES 5
#define MAX_REQUESTS 10


WvPopClient::WvPopClient(WvStream *conn, WvIStreamList &_l,
			 WvStringParm acct, WvStringParm _password,
			 WvStringParm _deliverto, WvStringParm _mda, 
                         bool _flushing, bool _apop_enable,
                         bool _apop_enable_fallback, bool _explode)
  : WvStreamClone(conn), l(_l), sendprocs(10),
    username(acctparse(acct)), // I hate constructors!
    password(_password), deliverto(_deliverto), mda(_mda),
    log(WvString("PopRetriever %s", acct), WvLog::Debug3)
{
    uses_continue_select = true;
    personal_stack_size = 65536;
    mess = NULL;
    never_select = false;
    flushing = _flushing;
    apop_enable = _apop_enable;
    apop_enable_fallback = _apop_enable_fallback;
    next_req = next_ack = sendmails = 0;
    explode = _explode;
    
    log(WvLog::Info, "Retrieve mail from %s into %s.\n", acct, deliverto);
}


WvPopClient::~WvPopClient()
{
    if (isok()) // someone is killing us before our time
	seterr("Closed connection at user request.");
    
    terminate_continue_select();

    // disable callbacks for all remaining sendmail procs
    WvSendmailProcDict::Iter i(sendprocs);
    for (i.rewind(); i.next(); )
	i->cb = WvSendmailCallback();
    
    if (geterr())
	log(WvLog::Error, "Aborted.  Error was: %s\n", errstr());
    else
	log(WvLog::Info, "Done.\n");
    
    if (mess)
	deletev mess;
}


WvString WvPopClient::acctparse(WvStringParm acct)
{
    WvString u(acct);
    char *cptr = strrchr(u.edit(), '@');
    
    if (cptr)
	*cptr = 0;
    return u;
}


void WvPopClient::cmd(WvStringParm s)
{
    print("%s\r\n", s);
    
    if (!strncasecmp(s, "pass ", 5))
	trace.append(new WvString("pass"), true);
    else if (!strncasecmp(s, "apop ", 5))
        trace.append(new WvString("apop"), true);
    else
    { 
        if (!strncasecmp(s, "apop ", 5))
            trace.append(new WvString("apop"), true);
        else
	    trace.append(new WvString(s), true);
    }
    
    if (!strncasecmp(s, "retr ", 5)) // less important
	log(WvLog::Debug3, "Send: %s\n", s);
    else if (!strncasecmp(s, "pass ", 5))    // hide passwords
	log(WvLog::Debug2, "Send: pass <NOT SHOWN>\n");
    else
	log(WvLog::Debug2, "Send: %s\n", s);
}


bool WvPopClient::response()
{
    flush(0);
    
    assert(!trace.isempty());
    WvStringList::Iter i(trace);
    i.rewind(); i.next();
    
    char *line = getline(60*1000);
    if (!line)
    {
	if (isok())
	    seterr(ETIMEDOUT);
	return false;
    }
    
    log(WvLog::Debug2, "Recv(%s): %s\n", *i, line);
    i.unlink();
    
    if (!strncmp(line, "+OK", 3))
    {
	res1 = res2 = 0; 
	sscanf(line, "+OK %ld %ld", &res1, &res2);
	return true;
    }
    
    return false;
}


static int messcompare(const void *_a, const void *_b)
{
    const WvPopClient::MsgInfo *a = (WvPopClient::MsgInfo *)_a;
    const WvPopClient::MsgInfo *b = (WvPopClient::MsgInfo *)_b;
    
    long base = 10240;
    long alen = a->len / base, blen = b->len / base;
    return (alen*base + a->num) - (blen*base + b->num);
}


bool WvPopClient::pre_select(SelectInfo &si)
{
    bool val, oldrd = si.wants.readable;

    if (never_select)
	si.wants.readable = false;
    val = WvStreamClone::pre_select(si);
    si.wants.readable = oldrd;
    return val;
}


void WvPopClient::execute()
{
    const char format[] = "%20.20s> %-40.40s\n";
    char *line, *greeting, *start, *end, *cptr;
    int count, nmsgs;
    WvString from, subj;
    bool printed, in_head;
    
    WvStreamClone::execute();
    
    // read hello header
    // Adapted for APOP by markj@luminas.co.uk
    // trace.append(new WvString("HELLO"), true);
    greeting = getline(60*1000);
    if (!greeting || strncmp(greeting,"+OK", 3)) goto fail;
    // if (!response()) goto fail;
    log(WvLog::Debug1, "Recv(HELLO): %s", greeting);

    // log in
    if (apop_enable && strrchr(greeting, '>')) 
    {
        // APOP login attempt -- early code from fetchmail
        /* build MD5 digest from greeting timestamp + password */
        /* find start of timestamp */
        for (start = greeting;  *start != 0 && *start != '<';  start++)
            continue;
        if (*start == 0) goto fail;

        /* find end of timestamp */
        for (end = start;  *end != 0  && *end != '>';  end++)
            continue;
        if (*end == 0 || end == start + 1) goto fail;
        else
            *++end = '\0';
        // end of fetchmail code
	// 
        log(WvLog::Debug2, "Using APOP seed: %s\n", start);

        /* copy timestamp and password into digestion buffer */
        WvString digestsecret("%s%s",start,password);
        WvDynBuf md5buf;
        WvMD5Digest().flushstrbuf(digestsecret, md5buf, true /*finish*/);
        WvString md5hex = WvHexEncoder().strflushbuf(md5buf, true);
        
        log(WvLog::Debug2, "Using APOP response: %s\n", md5hex);
        cmd("apop %s %s", username, md5hex);
    } 

    if (!apop_enable || !strrchr(greeting, '>') 
	|| (!response() && apop_enable_fallback))
    {
        // USER/PASS login
        cmd("user %s", username);
        cmd("pass %s", password);
        if (!response()) goto fail;
        if (!response())
        {
           seterr("Server denied access.  Wrong password?");
           return;
        }
    }

    // get the number of messages
    cmd("stat");
    cmd("list"); // for later
    if (!response()) goto fail;
    log(WvLog::Info, "%s %s in %s bytes.\n", res1,
	res1==1 ? "message" : "messages", res2);
    nmsgs = res1;
    mess = new MsgInfo[nmsgs];
    
    // get the list of messages and their sizes
    if (!response()) goto fail;
    for (count = 0; count < nmsgs; count++)
    {
	line = getline(60*1000);
	if (!isok() || !line || 
	    sscanf(line, "%d %ld", &mess[count].num, &mess[count].len) != 2)
	{
	    log(WvLog::Error, "Invalid LIST response: '%s'\n", line);
	    goto fail;
	}
    }
    line = getline(60*1000);
    if (!isok() || !line || strcmp(line, ".") && strcmp(line, ".\r"))
    {
	log(WvLog::Error, "Invalid LIST terminator: '%s'\n", line);
	goto fail;
    }
    
    // sort the list in order of size
    qsort(mess, nmsgs, sizeof(mess[0]), messcompare);
    
    while (next_ack < nmsgs 
	   || (next_ack && mess[next_ack-1].deletes_after_this) || sendmails)
    {
	if (!isok()) break;
	log(WvLog::Debug4, "next_ack=%s/%s dels=%s sendmails=%s/%s\n",
	    next_ack, nmsgs,
	    mess[0].deletes_after_this, sendmails,
	    WvSendmailProc::num_sendmails);
	
	while (next_req < nmsgs && next_req-next_ack < MAX_REQUESTS)
	{
	    cmd("retr %s", mess[next_req].num);
	    next_req++;
	}
    
	MsgInfo &m = mess[next_ack];
	
	while (next_ack > 0 && mess[next_ack-1].deletes_after_this)
	{
	    if (!response())
	    {
		log(WvLog::Warning, "Failed deleting a message!?\n");
		if (flushing)
		    log(WvLog::Warning, "Canceling future deletions "
			"to protect the innocent.\n");
		flushing = false;
	    }
	    
	    mess[next_ack-1].deletes_after_this--;
	    
	    if (!isok())
		goto fail;
	}
	
	if (next_ack >= nmsgs)
	{
	    never_select = true;
	    continue_select(20);
	    never_select = false;
	    continue; // only deletions and/or sendmails remaining
	}
	
	if (!response() && isok())
	{
	    next_ack++;
	    log(WvLog::Warning, "Hmm... missing message #%s.\n", m.num);
	    continue;
	}
	
	if (!isok())
	{
	    if (!geterr()) // Keep the older, better?, error message
	        seterr("Aborted.\n");
	    return;
	}
	
	next_ack++;
	
	WvString size;
	if (m.len > 10*1024*1024)
	    size = WvString("%sM", m.len/1024/1024);
	else if (m.len >= 10*1024)
	    size = WvString("%sk", m.len/1024);
	else if (m.len > 1024)
	    size = WvString("%s.%sk", m.len/1024, (m.len*10/1024) % 10);
	else
	    size = WvString("%sb", m.len);
	
	log(WvLog::Debug1, "%-6s %6s ", 
	    WvString("[%s]", mess[next_ack-1].num), size);
	
	from = "";
	subj = "";
	printed = false;
	in_head = true;
	
	while (WvSendmailProc::num_sendmails >= MAX_PROCESSES && isok())
	{
	    never_select = true;
	    continue_select(1000);
	    never_select = false;
	}
	
	const char *argv[] = {mda, deliverto, NULL};
	//const char *argv[] = {"dd", "of=/dev/null", NULL};
	WvSendmailProc *p = NULL; 
	if (!explode)
	  {
	    p = new WvSendmailProc(argv, next_ack-1,
				   WvSendmailCallback(this, &WvPopClient::send_done));
	    sendmails++;
	  }
	
	while ((line = getline(60*1000)) != NULL)
	{
	    if (!isok())
	    {
		if (p)
		  p->kill(SIGTERM);
		seterr("Connection dropped while reading message contents!");
		return;
	    }
	    if (!line)
	    {
	      if (p)
		p->kill(SIGTERM);
		seterr(ETIMEDOUT);
		return;
	    }
	    
	    // remove \r character, if the server gave one
	    cptr = strchr(line, 0) - 1;
	    if (*cptr == '\r')
		*cptr = 0;
	    
	    if (line[0]==0 || line[0]=='\r')
		in_head = false;
	    if (line[0]=='.')
	    {
		if (!strcmp(line, ".") || !strcmp(line, ".\r"))
		    break; // dot on a line by itself: done this message.
		else
		{
		    // POP servers turn any line *starting with* a dot into
		    // a line starting with dot-dot; so remove the extra
		    // dot.
		    line++;
		}
	    }
	    
	    if (in_head && !strncasecmp(line, "From: ", 6))
	    {
		cptr = strchr(line+6, '<');
		if (cptr)
		{
		    WvString tmp(cptr + 1);
		    cptr = strchr(tmp.edit(), '>');
		    if (cptr)
			*cptr = 0;
		    from = WvString("<%s", tmp);
		}
		else
		    from = line+6;
		trim_string(from.edit());
	    }
	    else if (in_head && explode && !p
		     && !strncasecmp(line, "X-Envelope-To: ", 14))
	    {
	      WvString sendto = line+strlen("X-Envelope-To:");
	      sendto.edit();
		cptr = strchr(sendto, '@');
		if (cptr)
		    *cptr = 0;
		trim_string(sendto.edit());
		const char *argvE[] = {mda, sendto, NULL};
		p = new WvSendmailProc(argvE, next_ack-1,
				       WvSendmailCallback(this, &WvPopClient::send_done));
		sendmails++;
	    }
	    else if (in_head && !strncasecmp(line, "Subject: ", 9))
	    {
		subj = line+9;
		trim_string(subj.edit());
	    }
	    
	    if (!!from && !!subj && !printed)
	    {
		log(WvLog::Debug1, format, from, subj);
		printed = true;
	    }
	    
	    if (p)
	      p->print("%s\n", line);
	}
	
	if (isok() && !printed)
	{
	    if (!subj) subj = "<NO SUBJECT>";
	    if (!from) from = "<NO SENDER";
	    log(WvLog::Debug1, format, from, subj);
	}
	
	if (p)
	  {
	    p->done();
	    l.append(p, true, "sendmail");
	    sendprocs.add(p, false);
	  }
    }
    
    cmd("quit");
    if (!response()) goto fail;
    
    close();
    return;
    
fail:
    if (cloned && cloned->geterr())
	seterr("Server connection error: %s", cloned->errstr());
    else if (!cloned || !cloned->isok())
	seterr("Server connection closed unexpectedly (%s bytes left)!",
	       cloned ? inbuf.used() : 0);
    else
	seterr("Server said something unexpected!");
    return;
}


void WvPopClient::send_done(int count, bool success)
{
    assert(mess);
    log(WvLog::Debug4, "send_done %s (success=%s)\n", count+1, success);
    
    WvSendmailProc *proc = sendprocs[count];
    if (proc)
	sendprocs.remove(proc);
    
    sendmails--;

    if (!success)
    {
	log(WvLog::Warning, "Error delivering message %s to sendmail.\n",
	    mess[count].num);
    }
    else
    {
	mess[count].sent = true;
	
	// remember that we had one more delete message after the most recent
	// request (which might have no relation to the message we're deleting,
	// but we have to count responses carefully!)
	if (flushing)
	{
	    cmd("dele %s", mess[count].num);
	    mess[next_req-1].deletes_after_this++;
	}
    }
    
    // wake up the WvPopClient, which may have been waiting for a sendmail
    // to finish.
    never_select = false;
}

