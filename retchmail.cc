
#include "wvtcp.h"
#include "wvconf.h"
#include "wvlog.h"
#include "wvpipe.h"
#include "wvstreamlist.h"
#include "wvlogrcv.h"
#include "wvsslstream.h"
#include "wvhashtable.h"
#include <signal.h>
#include <assert.h>
#include <pwd.h>
#include <sys/types.h>

#define MAX_PROCESSES 1
#define MAX_REQUESTS 10

// parameters are: int mess_index, bool success
DeclareWvCallback(2, void, WvSendmailCallback, int, bool);

class WvSendmailProc : public WvPipe
{
public:
    bool is_done, exited;
    static int num_sendmails;
    int count;
    WvSendmailCallback cb;
    
    WvSendmailProc(const char **argv, int _c, const WvSendmailCallback &_cb);
    virtual ~WvSendmailProc();
    
    virtual bool select_setup(SelectInfo &si);
    virtual bool isok() const;
    virtual void execute();
    void done();
};

int WvSendmailProc::num_sendmails = 0;


WvSendmailProc::WvSendmailProc(const char **argv, int _count,
			       const WvSendmailCallback &_cb)
    : WvPipe(argv[0], argv, true, false, false), count(_count), cb(_cb)
{
    is_done = exited = false;
    num_sendmails++;
}


WvSendmailProc::~WvSendmailProc()
{
    close();
    
    int ret = finish();

    // be certain the callback gets called
    if (!exited)
	cb(count, (ret == 0));
    
    num_sendmails--;
}


bool WvSendmailProc::select_setup(SelectInfo &si)
{
    bool must = false;

    if (is_done && !exited && si.msec_timeout > 20)
	si.msec_timeout = 20;
    
    if (child_exited() && !exited)
    {
	exited = true;
	
	// call the callback
	if (is_done)
	    cb(count, !exit_status());
	
	si.msec_timeout = 0;
	must = true;
    }
    return WvPipe::select_setup(si) || must;
}


bool WvSendmailProc::isok() const
{
    return WvPipe::isok() || !exited;
}


void WvSendmailProc::execute()
{
    WvPipe::execute();
    
    if (is_done && select(0, false, true, false))
	close();
}


void WvSendmailProc::done()
{
    is_done = true;
    force_select(false, true, false);
}







class WvPopClient : public WvStreamClone
{
public:
    WvStream *cloned;
    WvStreamList &l;
    WvString username, password, deliverto;
    WvLog log;
    long res1, res2;
    int  next_req, next_ack, sendmails;
    bool flushing;
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
    WvPopClient(WvStream *conn, WvStreamList &_l,
		const WvString &acct, const WvString &_password,
		const WvString &_deliverto, bool _flushing);
    virtual ~WvPopClient();

    bool never_select;
    virtual bool select_setup(SelectInfo &si);
    virtual void execute();
    
    void cmd(const WvString &s);
    void cmd(WVSTRING_FORMAT_DECL)
	{ return cmd(WvString(WVSTRING_FORMAT_CALL)); }
    bool response();
    
    void send_done(int count, bool success);
    
private:
    WvString acctparse(const WvString &acct);
};


WvPopClient::WvPopClient(WvStream *conn, WvStreamList &_l,
			 const WvString &acct, const WvString &_password,
			 const WvString &_deliverto, bool _flushing)
    : WvStreamClone(&cloned), l(_l),
	username(acctparse(acct)), // I hate constructors!
	password(_password), deliverto(_deliverto), 
        log(WvString("PopRetriever %s", acct), WvLog::Debug3)
{
    uses_continue_select = true;
    personal_stack_size = 8192;
    cloned = conn;
    mess = NULL;
    never_select = false;
    flushing = _flushing;
    next_req = next_ack = sendmails = 0;
    
    log(WvLog::Info, "Retrieve mail from %s into %s.\n", acct, deliverto);
}


WvPopClient::~WvPopClient()
{
    terminate_continue_select();
    
    if (geterr())
	log(WvLog::Error, "Aborted.  Error was: %s\n", errstr());
    else
	log(WvLog::Info, "Done.\n");
    
    if (mess)
	delete[] mess;
}


WvString WvPopClient::acctparse(const WvString &acct)
{
    WvString u(acct);
    char *cptr = strchr(u.edit(), '@');
    
    if (cptr)
	*cptr = 0;
    return u;
}


void WvPopClient::cmd(const WvString &s)
{
    print("%s\r\n", s);
    
    if (!strncasecmp(s, "pass ", 5))
	trace.append(new WvString("pass"), true);
    else
	trace.append(new WvString(s), true);
    
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


bool WvPopClient::select_setup(SelectInfo &si)
{
    bool val, oldrd = si.readable;

    if (never_select)
	si.readable = false;
    val = WvStreamClone::select_setup(si);
    si.readable = oldrd;
    return val;
}


void WvPopClient::execute()
{
    const char format[] = "%20.20s> %-40.40s\n";
    char *line, *cptr;
    int count, nmsgs;
    WvString from, subj;
    bool printed, in_head;
    
    WvStreamClone::execute();
    
    // read hello header
    trace.append(new WvString("HELLO"), true);
    if (!response()) goto fail;
    
    // log in
    cmd("user %s", username);
    cmd("pass %s", password);
    if (!response()) goto fail;
    
    if (!response())
    {
	seterr("Server denied access.  Wrong password?");
	return;
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
	log(WvLog::Debug4, "next_ack=%s dels=%s mails=%s/%s\n",
	    next_ack, mess[0].deletes_after_this, sendmails,
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
	
	const char *argv[] = {"/usr/sbin/sendmail", deliverto, NULL};
	//const char *argv[] = {"dd", "of=/dev/null", NULL};
	WvSendmailProc *p = new WvSendmailProc(argv, next_ack-1,
	       wvcallback(WvSendmailCallback, *this, WvPopClient::send_done));
	sendmails++;
	
	while ((line = getline(60*1000)) != NULL)
	{
	    if (!isok())
	    {
		p->kill(SIGTERM);
		seterr("Connection dropped while reading message contents!");
		return;
	    }
	    if (!line)
	    {
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
	    if (line[0]=='.' && (!strcmp(line, ".") || !strcmp(line, ".\r")))
		break; // done
	    
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
		    from.unique();
		}
		else
		    from = line+6;
		trim_string(from.edit());
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
	    
	    p->print("%s\n", line);
	}
	
	if (isok() && !printed)
	{
	    if (!subj) subj = "<NO SUBJECT>";
	    if (!from) from = "<NO SENDER";
	    log(WvLog::Debug1, format, from, subj);
	}
	
	p->done();
	l.append(p, true, "sendmail");
    }
    
    cmd("quit");
    if (!response()) goto fail;
    
    close();
    return;
    
fail:
    seterr("Protocol error from server!");
    return;
}


void WvPopClient::send_done(int count, bool success)
{
    assert(mess);
    log(WvLog::Debug4, "send_done %s\n", count+1);
    
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


struct LogNum
{
    int num;
    const WvLog *src;
};

// cheesy hash function to do the job, basically
unsigned int WvHash(const WvLog *x)
{
    return WvHash((int)x);
}


DeclareWvDict(LogNum, const WvLog *, src);


class RetchLog : public WvLogConsole
{
public:
    RetchLog(WvLog::LogLevel _max_level) 
	: WvLogConsole(1, _max_level), lognums(5)
	{ maxnum = 0; }
    virtual ~RetchLog() { }
    
    virtual void _begin_line();
    
    int maxnum;
    LogNumDict lognums;
};


void RetchLog::_begin_line()
{
    if (!strncmp(last_source->app, "PopRetriever ", 13))
    {
	LogNum *lognum = lognums[last_source];
	if (!lognum)
	{
	    lognum = new LogNum;
	    lognum->num = maxnum++;
	    lognum->src = last_source;
	    lognums.add(lognum, true);
	}
	
	// identify the connectionw without being too verbose
	if (lognum->num < 26)
	{
	    char str[2];
	    str[0] = 'A' + lognum->num;
	    str[1] = 0;
	    write(str);
	}
	else
	    write(lognum->num);
	if (last_level <= WvLog::Info)
	    write("* ");
	else
	    write("  ");
    }
    else
	WvLogConsole::_begin_line(); // just print the whole thing
}



static volatile bool want_to_die = false;
void signal_handler(int signum)
{
    fprintf(stderr, "\nCaught signal %d; cleaning up and terminating.\n",
	    signum);
    want_to_die = true;
    signal(signum, SIG_DFL);
}


static WvPopClient *newpop(WvStreamList &l, const WvString &acct,
			   const WvString &_pass, const WvString &_deliverto,
			   bool flush)
{
    WvString user(acct), serv, pass(_pass), deliverto(_deliverto);
    bool ssl = false;
    
    pass.unique();
    deliverto.unique();
    
    char *cptr = strchr(user.edit(), '@');
    if (cptr)
    {
	*cptr++ = 0;
	serv = cptr;
	serv.unique();
    }
    else
	serv = "localhost";
    
    cptr = strchr(serv.edit(), ':');
    if (!cptr)
    {
	serv.append(":110");
	cptr = strchr(serv.edit(), ':'); // guaranteed to work now!
    }
    
    if (atoi(cptr+1) == 995)
	ssl = true;
    
    WvStream *conn = new WvTCPConn(serv);
    if (ssl) // FIXME: ssl verify should probably be 'true'
	conn = new WvSSLStream(conn, NULL, false); 
    
    return new WvPopClient(conn, l, acct, pass, deliverto, flush);
}


static void usage(char *argv0, const WvString &deliverto)
{
    fprintf(stderr,
	    "Usage: %s [-d] [-dd] [-q] [-qq] [-V] [-F] [-c configfile ] [-t deliverto] [acct...]\n"
	    "     -d   Print debug messages\n"
	    "     -dd  Print lots of debug messages\n"
	    "     -q   Quieter: don't print every message header\n"
	    "     -qq  Way quieter: only print errors\n"
	    "     -V   Print version and exit\n"
	    "     -c   Use <configfile> instead of $HOME/.retchmail/retchmail\n"
	    "     -F   Flush (delete) messages after downloading\n"
	    "     -t   Deliver mail to <deliverto> (default '%s')\n"
	    "  acct... list of email accounts (username@host) to "
	               "retrieve from\n",
	    argv0, (const char *)deliverto);
    exit(1);
}

extern char *optarg;
extern int optind;


int main(int argc, char **argv)
{
    bool flush = false;
    int c, count;
    WvLog::LogLevel lvl = WvLog::Debug1;
    WvString deliverto("");
    WvString conffile("");
    
    // make sure electric fence works
    free(malloc(1));
    signal(SIGPIPE, SIG_IGN);
    
    struct passwd *pw = getpwuid(getuid());
    if (pw)
    {
	deliverto = pw->pw_name;
	deliverto.unique();
    }
    
    while ((c = getopt(argc, argv, "dqVFt:?c:")) >= 0)
    {
	switch (c)
	{
	case 'd':
	    if (lvl <= WvLog::Debug1)
		lvl = WvLog::Debug2;
	    else
		lvl = WvLog::Debug5;
	    break;
	case 'q':
	    if (lvl >= WvLog::Debug1)
		lvl = WvLog::Info;
	    else
		lvl = WvLog::Notice;
	    break;
	case 'V':
	    fprintf(stderr, "Retchmail version 0.0\n");
	    exit(2);
	case 'F':
	    flush = true;
	    break;
	case 't':
	    deliverto = optarg;
	    deliverto.unique();
	    break;
	case '?':
	    usage(argv[0], deliverto);
	    break;
	case 'c':
	    conffile = optarg;
	    conffile.unique();
	    break;
	}
    }
    
    if (!deliverto)
    {
	fprintf(stderr, "Can't detect username for uid#%u.  "
		"You must give the -t option.\n", getuid());
	exit(3);
    }
    
    
    RetchLog logrcv(lvl);
    
    if (fopen(conffile,"r") == NULL) // Is conffile a valid file??
    // If not, set it to $HOME/.retchmail/retchmail.conf
    {
	conffile = getenv("HOME");
	conffile.append("/.retchmail/retchmail.conf");
	conffile.unique();
    }

    WvConf cfg(conffile, 0600);
    WvStreamList l;
    WvPopClient *cli;
    
    if (optind == argc)
    {
	WvConfigSection *sect = cfg["POP Servers"];
	if (sect && !sect->isempty())
	{
	    WvConfigSection::Iter i(*sect);
	    for (i.rewind(); i.next(); )
	    {
		cli = newpop(l, i->name, i->value,
			     cfg.get("POP Targets", i->name, deliverto),
			     flush);
		l.append(cli, true, "client");
	    }
	}
	else
	{
	    fprintf(stderr, "\n-- No config file and no accounts given on the "
		    "command line!\n\n");
	    usage(argv[0], deliverto);
	}
    }
    else
    {
	for (count = optind; count < argc; count++)
	{
	    const char *pass = cfg.get("POP Servers", argv[count], NULL);
	    if (!pass)
	    {
		wvcon->print("Password for <%s>: ", argv[count]);
		
		system("stty -echo 2>/dev/null");
		pass = wvcon->getline(-1);
		system("stty echo 2>/dev/null");
		
		wvcon->print("\n");
	    }
	    
	    cli = newpop(l, argv[count], pass,
			 cfg.get("POP Targets", argv[count], deliverto),
			 flush);
	    l.append(cli, true, "client");
	}
    }
    
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    while (!l.isempty() && !want_to_die)
    {
	if (l.select(1000))
	    l.callback();
    }
}
