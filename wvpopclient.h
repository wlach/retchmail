/*
 * Worldvisions Weaver Software:
 *   Copyright (C) 1997-2003 Net Integration Technologies, Inc.
 *
 * Avery's insanely fast alternative to Fetchmail -- Pop3 Client
 *
 * This code is LGPL - see the file COPYING.LIB for a full copy of the
 * license.
 */

#include "wvsendmail.h"
#include "wvstreamclone.h"
#include "wvstring.h"
#include "wvistreamlist.h"
#include "wvhashtable.h"
#include "wvcont.h"
#include "wvlog.h"

#ifndef WVPOPCLIENT_H
#define WVPOPCLIENT_H 1

DeclareWvDict(WvSendmailProc, int, count);

class WvPopClient : public WvStreamClone
{
public:
    WvStream *cloned;
    WvIStreamList &l;
    WvSendmailProcDict sendprocs;
    WvString username, password, deliverto, mda;
    WvLog log;
    long res1, res2;
    int  next_req, next_ack, sendmails;
    bool flushing, apop_enable, apop_enable_fallback, explode;
    WvStringList trace;
    
    struct MsgInfo
    {
	int num;                // message number
	long len;               // message length (bytes)
	bool sent,              // message _fully_ transferred to sendmail
	     deleted;           // server acknowledged DELE command
	int deletes_after_this; // number of DELE messages following this RETR
	
	MsgInfo() { num = 0; len = 0; sent = deleted = false; 
		     deletes_after_this = 0; }
    };
    MsgInfo *mess;
    
    
    // note: we take possession of 'conn' and may delete it at any time!
    WvPopClient(WvStream *conn, WvIStreamList &_l,
		WvStringParm acct, WvStringParm _password,
		WvStringParm _deliverto, WvStringParm _mda, 
		bool _flushing, bool _apop_enable,
                bool _apop_enable_fallback, bool _explode);
    virtual ~WvPopClient();

    bool never_select;
    virtual bool pre_select(SelectInfo &si);
    virtual void execute();
    
    void cmd(WvStringParm s);
    void cmd(WVSTRING_FORMAT_DECL)
	{ return cmd(WvString(WVSTRING_FORMAT_CALL)); }
    bool response();
    
    void send_done(int count, bool success);
    
private:
    WvString acctparse(WvStringParm acct);

    WvCont cont;
    void *real_execute(void *contdata = 0);
};

#endif // WVPOPCLIENT_H
