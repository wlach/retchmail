
prefix=/usr/local

ifeq ($(WVSTREAMS),)
    WVSTREAMS_INC=
    WVSTREAMS_LIB=
    WVSTREAMS_BIN=$(prefix)/bin
    WVSTREAMS_SRC=.

    PC_CFLAGS=$(shell pkg-config --cflags libwvstreams)
    ifeq ($(PC_CFLAGS),)
     $(error WvStreams does not appear to be installed)
    endif
    CPPFLAGS+=$(PC_CFLAGS)

    PC_LIBS=$(shell pkg-config --libs libwvstreams)
    ifeq ($(PC_LIBS),)
     $(error WvStreams does not appear to be installed)
    endif
    LIBS+=$(PC_LIBS)
endif

include wvrules.mk
include config.mk

config.mk:
	@echo "Please run ./configure. Stop."
	@exit 1

default: retchmail
all: retchmail

#LIBS += ${EFENCE}
LDFLAGS += -rdynamic

ifneq ($(WVSTREAMS_LIB),)
retchmail-LIBS+=$(LIBUNICONF) ${LIBWVSTREAMS} $(LIBWVUTILS)
else
retchmail: LDFLAGS=-luniconf
endif
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
	install -m 0644 retchmail.conf.5 ${prefix}/share/man/man5

clean:
	rm -f retchmail

.PHONY: install install-bin install-man clean
