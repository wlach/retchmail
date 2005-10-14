/*
 * Worldvisions Weaver Software:
 *   Copyright (C) 1997-2005 Net Integration Technologies, Inc.
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
#include "wvargs.h"
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
    WvString src;
};

DeclareWvDict(LogNum, WvString, src);


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
    if (!strncmp(last_source, "PopRetriever ", 13))
    {
	LogNum *lognum = lognums[last_source];
	if (!lognum)
	{
	    lognum = new LogNum;
	    lognum->num = maxnum++;
	    lognum->src = last_source;
	    lognums.add(lognum, true);
	}
	
	// identify the connection without being too verbose
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
                           bool apop_fall_en, bool explode, bool safemode,
			   bool ignorerp)

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
    
    return new WvPopClient(conn, acct, pass, deliverto, mda, flush, apop_en, apop_fall_en, explode, safemode, ignorerp);
}


static bool dec_log_level_cb(void *userdata)
{
    WvLog::LogLevel level = *static_cast<WvLog::LogLevel *>(userdata);
    if ((int)level > (int)WvLog::Critical)
	level = (WvLog::LogLevel)((int)level - 1);
    return true;
}


static bool inc_log_level_cb(void *userdata)
{
    WvLog::LogLevel level = *static_cast<WvLog::LogLevel *>(userdata);
    if ((int)level < (int)WvLog::Debug5)
	level = (WvLog::LogLevel)((int)level + 1);
    return true;
}


int main(int argc, char **argv)
{
    bool flush = false, explode = false;
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

    WvArgs args;

    args.add_option('q', "quiet",
		    "Decrease log level (can be used multiple times)",
		    dec_log_level_cb, &lvl);
    
    args.add_option('d', "debug",
		    "Increase log level (can be used multiple times)",
		    inc_log_level_cb, &lvl);

    args.add_set_bool_option('F', "flush", 
			     "Flush (delete) messages after downloading",
			     flush);

    args.add_set_bool_option('E', "explode", 
			     "Send mail to the user on the system corresponding to the user the mail is sent to",
			     explode);
    
    args.add_option('t', "to", 
		    WvString("Deliver mail to <deliverto> (default '%s')", deliverto),
		    "deliverto", deliverto);

    args.add_option('c', "config",
		    "Use <moniker> instead of ini:~/.retchmail/retchmail",
		    "moniker", confmoniker);
    
    args.set_version("Retchmail version " RETCHMAIL_VER_STRING "\n");
    
    WvStringList arguments;
    if (!args.process(argc, argv, &arguments))
	return 255;
    
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

    WvPopClient *cli = NULL;
    bool apop_enable = cfg["retchmail"]["Enable APOP"].getmeint(0);
    bool apop_enable_fallback = cfg["retchmail"]["Enable APOP Fallback"].getmeint(0);
    bool safemode = cfg["retchmail"]["Safe Mode"].getmeint(0);
    bool ignorerp = cfg["retchmail"]["Ignore Return-Path"].getmeint(0);
  
    if (arguments.isempty())	    
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
			     explode ? : cfg["Explode"][i->key()].getmeint(0),
			     safemode, ignorerp);
		WvIStreamList::globallist.append(cli, true, "client");
	    }
	}
	else
	{
	    fprintf(stderr, "\n-- No config file and no accounts given on the "
		    "command line!\n");
	    args.print_usage(argc, argv);
	}
    }
    else
    {
	while (!arguments.isempty());
	{
	    WvString user(arguments.popstr());
	    WvString pass = cfg["POP Servers"][user].getme();
	    if (!pass)
	    {
		wvcon->print("Password for <%s>: ", user);
		
		system("stty -echo 2>/dev/null");
		pass = wvcon->blocking_getline(-1);
		system("stty echo 2>/dev/null");
		
		wvcon->print("\n");
	    }
	    
	    cli = newpop(user, pass,
			 cfg["POP Targets"][user].getme(deliverto),
			 cfg["MDA Override"][user].getme(
				 "/usr/sbin/sendmail"),
			 flush, apop_enable, apop_enable_fallback, explode,
			 safemode, ignorerp);
	    WvIStreamList::globallist.append(cli, true, "client");
	}
    }
    
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    while (!WvIStreamList::globallist.isempty() && !want_to_die)
    	WvIStreamList::globallist.runonce();

    lockfile.unlock();
}
