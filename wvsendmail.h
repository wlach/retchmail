#ifndef WVSENDMAIL_H
#define WVSENDMAIL_H 1

#include "wvpipe.h"
#include "wvcallback.h"

// parameters are: int mess_index, bool success
typedef WvCallback<void, int, bool> WvSendmailCallback;

class WvSendmailProc : public WvPipe
{
public:
    bool is_done, exited;
    static int num_sendmails;
    int count;
    WvSendmailCallback cb;
    
    WvSendmailProc(const char **argv, int _c, const WvSendmailCallback &_cb);
    virtual ~WvSendmailProc();
    
    virtual void pre_select(SelectInfo &si);
    virtual bool post_select(SelectInfo &si);
    virtual bool isok() const;
    virtual size_t uwrite(const void *buf, size_t count);
    virtual void execute();
    
    void done();
};

#endif // WVSENDMAIL_H
