TOPDIR=..
include $(TOPDIR)/wvrules.mk

XPATH=.. ../wvstreams/include $(PKGINC)

# List of library directories for SSLeay.  Add yours to the list.
# Don't forget the -L before each directory name!
SSLLIB= -L/usr/lib/ssl -L/usr/lib/ssleay -L/usr/local/lib/ssleay \
	-L/usr/local/ssl -L/usr/local/ssl/lib -L/usr/local/ssleay -lssl

WVLIB= -L../wvstreams -lwvutils -lwvstreams -lwvcrypto

default: retchmail
all: retchmail

#LIBS = ${EFENCE}

retchmail-LIBS=${SSLLIB} ${WVLIB}
retchmail:

clean:
	rm -f retchmail
