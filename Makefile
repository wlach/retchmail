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

WVLIB= -L../wvstreams $(LIBUNICONF)

default: retchmail
all: retchmail

#LIBS += ${EFENCE}
LDFLAGS += -rdynamic

retchmail-LIBS+=${WVLIB} ${SSLLIB} 
retchmail: retchmail.o wvpopclient.o wvsendmail.o

install: install-bin install-man

install-bin: all
	[ -d ${prefix}/bin ] || install -d ${prefix}/bin
	install -m 0755 retchmail ${prefix}/bin

install-man:
	[ -d ${prefix}/share/man ] || install -d ${prefix}/share/man
	[ -d ${prefix}/share/man/man1 ] || install -d ${prefix}/share/man/man1
	[ -d ${prefix}/share/man/man5 ] || install -d ${prefix}/share/man/man5
	install -m 0644 retchmail.1 ${prefix}/share/man/man1
	install -m 0644 retchmailrc.5 ${prefix}/share/man/man5

clean:
	rm -f retchmail

.PHONY: install install-bin install-man clean
