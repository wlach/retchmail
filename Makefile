ifeq ($(TOPDIR),)
TOPDIR=.
PKGINC=/usr/include/wvstreams /usr/local/include/wvstreams
endif

include $(TOPDIR)/wvrules.mk

XPATH=.. ../wvstreams/include $(PKGINC)

# List of library directories for SSLeay.  Add yours to the list.
# Don't forget the -L before each directory name!
SSLLIB= -L/usr/lib/ssl -L/usr/lib/ssleay -L/usr/local/lib/ssleay \
	-L/usr/local/ssl -L/usr/local/ssl/lib -L/usr/local/ssleay -lssl

WVLIB= -L../wvstreams -lwvstreams -lwvutils

default: retchmail
all: retchmail

ifdef USE_EFENCE
retchmail-LIBS += $(EFENCE)
endif

LDFLAGS += -rdynamic

retchmail-LIBS+=${WVLIB} ${SSLLIB} 
retchmail:

clean:
	rm -f retchmail
