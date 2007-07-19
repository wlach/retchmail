#include "wvsendmail.h"

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
    if (!exited && cb)
	cb(count, (ret == 0));
    
    num_sendmails--;
}


void WvSendmailProc::pre_select(SelectInfo &si)
{
    if (is_done && !exited && si.msec_timeout > 20)
	si.msec_timeout = 20;
    
    if (child_exited() && !exited && is_done)
	si.msec_timeout = 0;
    
    // another hack because isok() returns true when it shouldn't really
    if ((exited || is_done) && si.wants.writable)
	si.msec_timeout = 0;
    
    WvPipe::pre_select(si);
}


bool WvSendmailProc::post_select(SelectInfo &si)
{
    bool must = false;

    if (child_exited() && !exited && is_done)
    {
	exited = true;
	
	// call the callback
	if (cb)
	    cb(count, !exit_status());
	
	must = true;
    }
    
    // another hack because isok() returns true when it shouldn't really
    if ((exited || is_done) && si.wants.writable)
	must = true;
    
    return must || WvPipe::post_select(si);
}


bool WvSendmailProc::isok() const
{
    // note: this means people will try to write to us even if the pipe
    // says it's invalid!
    return WvPipe::isok() || !exited;
}


size_t WvSendmailProc::uwrite(const void *buf, size_t count)
{
    if (child_exited())
	return count; // fake it, because isok() is also faking it
    return WvPipe::uwrite(buf, count);
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
