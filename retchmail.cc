/*
 * Worldvisions Weaver Software:
 *   Copyright (C) 1997-2004 Net Integration Technologies, Inc.
 *
 * Avery's insanely fast alternative to Fetchmail
 *
 * This code is LGPL - see the file COPYING.LIB for a full copy of the
 * license.
 */

// WvStreams headers
#include "strutils.h"
#include "uniconfroot.h"
#include "wvlockfile.h"
#include "wvver.h"
#include "wvtcp.h"
#include "wvlog.h"
#include "wvpipe.h"
#include "wvistreamlist.h"
#include "wvlogrcv.h"
#include "wvsslstream.h"
#include "wvhashtable.h"
#include "wvcrash.h"
#include "wvhex.h"

// Retchmail headers
#include "wvsendmail.h"
#include "wvpopclient.h"

#include <signal.h>
#include <assert.h>
#include <pwd.h>
#include <sys/types.h>

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


static WvPopClient *newpop(WvStringParm acct,
			   WvStringParm _pass, WvStringParm _deliverto,
                           WvStringParm _mda, bool flush, bool apop_en,
                           bool apop_fall_en, bool explode)

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
    
    WvTCPConn *tcp = new WvTCPConn(serv);
    WvStream *conn = tcp;
    if (ssl) // FIXME: ssl verify should probably be set to something.
	conn = new WvSSLStream(tcp, NULL);
    
    return new WvPopClient(conn, acct, pass, deliverto, mda, flush, apop_en, apop_fall_en, explode);
}


static void usage(char *argv0, WvStringParm deliverto)
{
    wvcon->print("Usage: %s [-d] [-dd] [-q] [-qq] [-V] [-F] [-c moniker ] "
		 "[-t deliverto] [acct...]\n"
		 "     -d   Print debug messages\n"
		 "     -dd  Print lots of debug messages\n"
		 "     -q   Quieter: don't print every message header\n"
		 "     -qq  Way quieter: only print errors\n"
		 "     -V   Print version and exit\n"
		 "     -c   Use <moniker> instead of "
		 "ini:~/.retchmail/retchmail\n"
		 "     -F   Flush (delete) messages after downloading\n"
	         "     -E   Send mail to the user on the system corresponding to the user the mail is sent to\n"
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
    bool flush = false, explode = false;
    int c, count;
    WvLog::LogLevel lvl = WvLog::Debug1;
    WvString deliverto("");
    WvString confmoniker("");
    
    // Set up WvCrash
    wvcrash_setup(argv[0]);
    
    // make sure electric fence works
    free(malloc(1));

    signal(SIGPIPE, SIG_IGN);
    
    struct passwd *pw = getpwuid(getuid());
    if (pw)
	deliverto = pw->pw_name;

    while ((c = getopt(argc, argv, "dqVFEt:c:h?")) >= 0)
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
	    wvcon->print("Retchmail version %s\n", RETCHMAIL_VER_STRING);
	    return 2;
	case 'F':
	    flush = true;
	    break;
	case 't':
	    deliverto = optarg;
	    break;
	case 'c':
	    confmoniker = optarg;
	    break;
	case 'E':
	  explode = true;
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

    if (!lockfile.lock())
    {
        if (lockfile.readpid() == -1)
            fprintf(stderr, "retchmail: can't access lockfile at %s.\n",
		    lockname.cstr());
        else
            fprintf(stderr, "retchmail: already running (%i).\n",
		    lockfile.readpid());
        return 3;
    }

    if (!deliverto)
    {
	fprintf(stderr, "retchmail: can't get username for uid#%u.  "
		"You must give the -t option.\n", getuid());
	return 3;
    }
    
    
    RetchLog logrcv(lvl);
    
    if (!confmoniker)
    {
	// attempting to use $HOME/.retchmail/retchmail.conf\n");
        confmoniker = "ini:";
        confmoniker.append(getenv("HOME"));
        confmoniker.append("/.retchmail/retchmail.conf");
    }

    UniConfRoot cfg(confmoniker);

    if (!cfg.haschildren())
    {
        wvcon->print("No data found, aborting\n");
	exit(1);
    }

    WvPopClient *cli;
    bool apop_enable = cfg["retchmail"]["Enable APOP"].getmeint(0);
    bool apop_enable_fallback = cfg["retchmail"]["Enable APOP Fallback"].getmeint(0);
  
    if (optind == argc)	    
    {
        UniConf sect = cfg["POP Servers"];
	if (sect.haschildren())
	{
	    UniConf::Iter i(sect);
	    for (i.rewind(); i.next(); )
	    {
		cli = newpop(i->key(), i->getme(),
			     cfg["POP Targets"][i->key()].getme(deliverto),
                             cfg["MDA Override"][i->key()].getme(
				     "/usr/sbin/sendmail"),
                             flush, apop_enable, apop_enable_fallback,
			     explode ? : cfg["Explode"][i->key()].getmeint(0));
		WvIStreamList::globallist.append(cli, true, "client");
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
	    WvString pass = cfg["POP Servers"][argv[count]].getme();
	    if (!pass)
	    {
		wvcon->print("Password for <%s>: ", argv[count]);
		
		system("stty -echo 2>/dev/null");
		pass = wvcon->blocking_getline(-1);
		system("stty echo 2>/dev/null");
		
		wvcon->print("\n");
	    }
	    
	    cli = newpop(argv[count], pass,
			 cfg["POP Targets"][argv[count]].getme(deliverto),
			 cfg["MDA Override"][argv[count]].getme(
				 "/usr/sbin/sendmail"),
			 flush, apop_enable, apop_enable_fallback, explode);
	    WvIStreamList::globallist.append(cli, true, "client");
	}
    }
    
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    while (!WvIStreamList::globallist.isempty() && !want_to_die)
    	WvIStreamList::globallist.runonce();

    lockfile.unlock();
}
