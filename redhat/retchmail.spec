Name: retchmail
Version: 0.2
Release: 1

Summary: World's most stupidly fast POP retriever

Source: http://open.nit.ca/download/%{name}-%{version}.tar.gz
URL: http://open.nit.ca/retchmail
Group: Applications/Internet
BuildRoot: %{_tmppath}/retchmail-root
Copyright: LGPL

Prefix: /usr

%description
 Supports simultaneous pop3 and pop3s mail retrieval from multiple sites to
 multiple mailboxes, and has as it's primary goal to do it as fast and as
 accurately as possible.

%prep


%setup


%build
make

%install
rm -rf $RPM_BUILD_ROOT
make install PREFIX=$RPM_BUILD_ROOT/usr

%clean
rm -rf $RPM_BUILD_ROOT

make clean

%files
%defattr(-,root,root)
%doc README ANNOUNCE
/usr/bin/retchmail
/usr/share/man/man1/retchmail.1.gz
/usr/share/man/man5/retchmailrc.5.gz

%changelog
* Mon Mar 18 2002 Patrick Patterson <ppatters@nit.ca>

Synchronise with libwvstreams

RPM Packaging Fixes

* Tue Jan 29 2002 Patrick Patterson <ppatters@nit.ca>

Initial Release of Retchmail



