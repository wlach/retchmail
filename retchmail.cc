#include "wvlockfile.h"
#include "wvver.h"
#include "wvtcp.h"
#include "wvconf.h"
#include "wvlog.h"
#include "wvpipe.h"
#include "wvstreamlist.h"
#include "wvlogrcv.h"
#include "wvsslstream.h"
#include "wvcrypto.h"
#include "wvhashtable.h"
#include "wvcrash.h"
#include <signal.h>
#include <assert.h>
#include <pwd.h>
#include <sys/types.h>

#define MAX_PROCESSES 5
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
    
    virtual bool pre_select(SelectInfo &si);
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


bool WvSendmailProc::pre_select(SelectInfo &si)
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
    return WvPipe::pre_select(si) || must;
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
    force_select(false, true);
}





class WvPopClient : public WvStreamClone
{
public:
    WvStreamList &l;
    WvString username, password, deliverto, mda;
    WvLog log;
    long res1, res2;
    int  next_req, next_ack, sendmails;
    bool flushing, apop_enable, apop_enable_fallback;
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
		WvStringParm acct, WvStringParm _password,
		WvStringParm _deliverto, WvStringParm _mda, 
		bool _flushing, bool _apop_enable,
                bool _apop_enable_fallback);
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
};


WvPopClient::WvPopClient(WvStream *conn, WvStreamList &_l,
			 WvStringParm acct, WvStringParm _password,
			 WvStringParm _deliverto, WvStringParm _mda, 
                         bool _flushing, bool _apop_enable,
                         bool _apop_enable_fallback )
    : WvStreamClone(conn), l(_l),
	username(acctparse(acct)), // I hate constructors!
	password(_password), deliverto(_deliverto), mda(_mda),
        log(WvString("PopRetriever %s", acct), WvLog::Debug3)
{
    uses_continue_select = true;
    personal_stack_size = 8192;
    mess = NULL;
    never_select = false;
    flushing = _flushing;
    apop_enable = _apop_enable;
    apop_enable_fallback = _apop_enable_fallback;
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
    if ((!greeting) || (strncmp(greeting,"+OK",3))) goto fail;
    // if (!response()) goto fail;
    log(WvLog::Debug1, "Recv(HELLO): %s", greeting);

    // log in
    if (apop_enable && (strrchr(greeting,'>'))) 
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
        log(WvLog::Debug2, "Using APOP seed: %s\n", start);

        /* copy timestamp and password into digestion buffer */
        WvString digestsecret("%s%s",start,password);

        WvMD5 resp(digestsecret,false);
        log(WvLog::Debug2, "Using APOP response: %s\n", (resp.md5_hash()));
        cmd("apop %s %s",username,(resp.md5_hash()));
    } 

    if ((!apop_enable) || (!strrchr(greeting,'>')) || ((!response())&&apop_enable_fallback)) {
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
	
	const char *argv[] = {mda, deliverto, NULL};
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


static WvPopClient *newpop(WvStreamList &l, WvStringParm acct,
			   WvStringParm _pass, WvStringParm _deliverto,
                           WvStringParm _mda, bool flush, bool apop_en,
                           bool apop_fall_en)

{
    WvString user(acct), serv, pass(_pass), deliverto(_deliverto),
            mda(_mda);
    bool ssl = false;
    
    char *cptr = strrchr(user.edit(), '@');
    if (cptr)
    {
	*cptr++ = 0;
	serv = cptr;
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
    
    return new WvPopClient(conn, l, acct, pass, deliverto, mda, flush, apop_en, apop_fall_en);
}


static void usage(char *argv0, WvStringParm deliverto)
{
    wvcon->print("Usage: %s [-d] [-dd] [-q] [-qq] [-V] [-F] [-c configfile ] [-t deliverto] [acct...]\n"
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

    // Initialize wvcrash
    wvcrash_setup(argv[0]);

    signal(SIGPIPE, SIG_IGN);
    
    struct passwd *pw = getpwuid(getuid());
    if (pw)
	deliverto = pw->pw_name;

    while ((c = getopt(argc, argv, "dqVFt:c:h?")) >= 0)
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
	    wvcon->print("Retchmail version %s\n",RETCHMAIL_VER_STRING);
	    exit(2);
	case 'F':
	    flush = true;
	    break;
	case 't':
	    deliverto = optarg;
	    break;
	case 'c':
	    conffile = optarg;
	    break;
	case 'h':
	case '?':
	default:
	    usage(argv[0], deliverto);
	    break;
	}
    }

    WvString lockname("/tmp/retchmail.%s.pid", getlogin());
    WvLockFile lockfile(lockname);

    if (!lockfile.lock(getpid()))
    {
        if (lockfile.getpid() == -1)
            fprintf(stderr, "Can't access lockfile at %s.\n", lockname.cstr());
        else
            fprintf(stderr, "Already running (%i).\n", lockfile.getpid());
        exit(3);
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
	if (fopen(conffile,"r") == NULL) // check and see if this exists...
	// If not, then set conffile to $HOME/.retchmailrc
	// Which is the recommended value anyways....
	{
	    conffile = getenv("HOME");
	    conffile.append("/.retchmailrc");
	}
    }

    WvConf cfg(conffile, 0600);
    WvStreamList l;
    WvPopClient *cli;
    bool apop_enable = cfg.get("retchmail", "Enable APOP", 0);
    bool apop_enable_fallback = cfg.get("retchmail", "Enable APOP Fallback", 0);
  
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
                             cfg.get("MDA Override", i->name,
				     "/usr/sbin/sendmail"),
                             flush, apop_enable, apop_enable_fallback);
		l.append(cli, true, "client");
	    }
	}
	else
	{
	    fprintf(stderr, "\n-- No config file and no accounts given on the "
		    "command line!\n");
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
			 cfg.get("MDA Override", argv[count],
				 "/usr/sbin/sendmail"),
			 flush, apop_enable, apop_enable_fallback);
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

    lockfile.unlock();
}
