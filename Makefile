TOPDIR=../..
include $(TOPDIR)/rules.mk

XPATH=.. ../crypto ../ipstreams ../configfile ../streams ../utils

# List of library directories for SSLeay.  Add yours to the list.
# Don't forget the -L before each directory name!
SSLLIB= -L/usr/lib/ssl -L/usr/lib/ssleay -L/usr/local/lib/ssleay \
	-L/usr/local/ssl -L/usr/local/ssl/lib -L/usr/local/ssleay -lssl

default: retchmail
all: 

#LIBS = ${EFENCE}

retchmail-LIBS=${SSLLIB}
retchmail: ../crypto/crypto.a ../libwvstreams.a

clean:
	rm -f retchmail
