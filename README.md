# darkstat-with-asn
This is a copy of the [darkstat](http://unix4lyfe.org/darkstat/) network 
traffic analyzer which has been patched to 
support [Autonomous System Numbers](https://en.wikipedia.org/wiki/Autonomous_system_\(Internet\)). 

After cloning this repo, build as usual: `./configure`, `make`, `make install`. 
(Got [compiler errors](#libresolv)?)

Or, you can apply the [patch](https://raw.githubusercontent.com/parseword/darkstat-with-asn/master/patch.txt) to 
an existing source tree and compile. 

To enable ASN support, use the new `--with-asn` command line option. darkstat 
will query the [Team Cymru IP to ASN DNS service](http://www.team-cymru.org/IP-ASN-mapping.html#dns) to 
determine the AS Number of each IPv4 host. The AS Numbers are displayed in 
darkstat's web interface, both as a new column in the "hosts" list and also on 
the host detail pages.

![Host summary page with AS numbers displayed](https://i.imgur.com/m21Nk2O.png)

In order to reduce DNS query load, an effort is made to skip AS Number lookups 
for IP addresses in certain reserved ranges (RFC1918, multicast, etc.). IPv6 
hosts are also skipped for the time being. The ASN column will display "(none)" 
or "(unsupported)" for these hosts.

ASN support adds a second child thread to darkstat, mirroring the behavior of 
the main DNS resolution thread. In testing, there was no noticeable additional 
overhead from the extra process; it only does anything when you load the 
web interface.

This build has been tested on several Linux 2.6 kernels and on FreeBSD 11.1. 
Feedback from users of other operating systems is welcome.

### Changes

The following files are new (italic) or modified from the original source:

* Makefile.in
* *asn.c*
* *asn.h*
* configure
* configure.ac
* darkstat.8.in
* darkstat.c
* dns.c
* hosts_db.c
* hosts_db.h
* opt.h

You can review the [patch](https://raw.githubusercontent.com/parseword/darkstat-with-asn/master/patch.txt) to 
see what's changed.

### Caveats

#### IPv4 only

At present, AS Numbers will only be looked up for IPv4 hosts.

#### libresolv

Querying for DNS TXT resource records uses several functions from the resolver 
library. Modern unix-based operating systems support these functions either 
natively or through libresolv.

Linux users will need to link against libresolv. I've tried to make the configure 
script detect this automatically. If you receive errors like this when compiling,

    asn.o: In function `ip4_lookup_asn':
    asn.c:420: undefined reference to `__res_nquery'
    asn.c:451: undefined reference to `ns_initparse'
    asn.c:461: undefined reference to `ns_parserr'
    asn.c:462: undefined reference to `ns_sprintrr'
    collect2: ld returned 1 exit status
    make: *** [darkstat] Error 1

...open the Makefile and append `-lresolv` to the `LIBS = ` line, then 
recompile.

#### Google's name servers (8.8.8.8, 8.8.4.4)

Enabling ASN lookups will nearly double the number of DNS queries generated 
by darkstat. Google's public name servers are rate limited, and may refuse 
your queries for awhile when you send a rapid burst of traffic. If you use 
Google DNS, and darkstat shows a lot of "(none)" where you know there should be 
valid data, this is likely due to a Google rate limit. Consider using your 
ISP's DNS servers or running one of your own.

### TODO

* Support IPv6 addresses.

* Maybe refactor the ASN into an integer (I *know*!) and support sorting by it.
