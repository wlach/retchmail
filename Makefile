TOPDIR=../..
include $(TOPDIR)/rules.mk

XPATH=.. ../ipstreams ../configfile ../streams ../utils

default: retchmail
all: 

#LIBS = ${EFENCE}

retchmail: ../libwvstreams.a

clean:
	rm -f retchmail
