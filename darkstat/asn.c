/* darkstat 3
 * copyright (c) 2001-2014 Emil Mikulic.
 * copyright (c) 2017 Shaun Cummiskey <https://shaunc.com>.
 *
 * asn.c: synchronous Autonomous System Number lookups in a child process.
 *
 * You may use, modify and redistribute this file under the terms of the
 * GNU General Public License version 2. (see COPYING.GPL)
 */

#include "asn.h"
#include "cdefs.h"
#include "cap.h"
#include "conv.h"
#include "decode.h"
#include "err.h"
#include "hosts_db.h"
#include "queue.h"
#include "str.h"
#include "tree.h"
#include "bsd.h" /* for setproctitle, strlcpy */

#include <sys/param.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <assert.h>
#include <errno.h>
#include <netdb.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
/* Required for res_query */
#include <netinet/in.h>
#include <arpa/nameser.h>
#include <resolv.h> /*:TODO: for Linux, the makefile LDFLAGS must include -lresolv */

static void asn_main(void) _noreturn_; /* the child process runs this */
int ip4_reverse_octets(char *ip, char *buf, size_t buflen);
int ip4_lookup_asn(char *ipaddr, char **asn);

#define CHILD 0 /* child process uses this socket */
#define PARENT 1
#define ASN_MAXLEN 11 /* 10 characters (RFC4893 Sec. 7) + NULL */
static int asn_sock[2];
static pid_t pid = -1;

struct asn_reply {
   struct addr addr;
   int error; /* for hstrerror(), or 0 if no error */
   char asplain[ASN_MAXLEN];
};

void
asn_init(const char *privdrop_user)
{
   if (socketpair(AF_UNIX, SOCK_STREAM, 0, asn_sock) == -1)
      err(1, "socketpair");

   pid = fork();
   if (pid == -1)
      err(1, "fork");

   if (pid == 0) {
      /* We are the child. */
      privdrop(NULL /* don't chroot */, privdrop_user);
      close(asn_sock[PARENT]);
      asn_sock[PARENT] = -1;
      daemonize_finish(); /* drop our copy of the lifeline! */
      if (signal(SIGUSR1, SIG_IGN) == SIG_ERR)
         errx(1, "signal(SIGUSR1, ignore) failed");
      cap_free_args();
      asn_main();
      errx(1, "ASN child fell out of asn_main()");
   } else {
      /* We are the parent. */
      close(asn_sock[CHILD]);
      asn_sock[CHILD] = -1;
      fd_set_nonblock(asn_sock[PARENT]);
      verbosef("ASN child has PID %d", pid);
   }
}

void
asn_stop(void)
{
   if (pid == -1)
      return; /* no child was started */
   close(asn_sock[PARENT]);
   if (kill(pid, SIGINT) == -1)
      err(1, "kill");
   verbosef("asn_stop() waiting for child");
   if (waitpid(pid, NULL, 0) == -1)
      err(1, "waitpid");
   verbosef("asn_stop() done waiting for child");
}

struct tree_rec {
   RB_ENTRY(tree_rec) ptree;
   struct addr ip;
};

static int
tree_cmp(struct tree_rec *a, struct tree_rec *b)
{
   if (a->ip.family != b->ip.family)
      /* Sort IPv4 to the left of IPv6.  */
      return ((a->ip.family == IPv4) ? -1 : +1);

   if (a->ip.family == IPv4)
      return (memcmp(&a->ip.ip.v4, &b->ip.ip.v4, sizeof(a->ip.ip.v4)));
   else {
      assert(a->ip.family == IPv6);
      return (memcmp(&a->ip.ip.v6, &b->ip.ip.v6, sizeof(a->ip.ip.v6)));
   }
}

static RB_HEAD(tree_t, tree_rec) ip_asn_tree = RB_INITIALIZER(&tree_rec);
RB_GENERATE_STATIC(tree_t, tree_rec, ptree, tree_cmp)

void
asn_queue(const struct addr *const ipaddr)
{
   struct tree_rec *rec;
   ssize_t num_w;
   if (pid == -1)
      return; /* no child was started - we're not doing ASN lookups */

   if ((ipaddr->family != IPv4) && (ipaddr->family != IPv6)) {
      verbosef("asn_queue() for unknown family %d", ipaddr->family);
      return;
   }

   rec = xmalloc(sizeof(*rec));
   memcpy(&rec->ip, ipaddr, sizeof(rec->ip));

   if (RB_INSERT(tree_t, &ip_asn_tree, rec) != NULL) {
      /* Already queued - this happens seldom enough that we don't care about
       * the performance hit of needlessly malloc()ing. */
      verbosef("asn_queue() already queued %s", addr_to_str(ipaddr));
      free(rec);
      return;
   }

   num_w = write(asn_sock[PARENT], ipaddr, sizeof(*ipaddr)); /* won't block */
   if (num_w == 0)
      warnx("asn_queue: write: ignoring end of file");
   else if (num_w == -1)
      warn("asn_queue: ignoring write error");
   else if (num_w != sizeof(*ipaddr))
      err(1, "asn_queue: wrote %zu instead of %zu", num_w, sizeof(*ipaddr));
}

static void
asn_unqueue(const struct addr *const ipaddr)
{
   struct tree_rec tmp, *rec;
   memcpy(&tmp.ip, ipaddr, sizeof(tmp.ip));
   if ((rec = RB_FIND(tree_t, &ip_asn_tree, &tmp)) != NULL) {
      RB_REMOVE(tree_t, &ip_asn_tree, rec);
      free(rec);
   }
   else
      verbosef("couldn't unqueue %s - not in queue!", addr_to_str(ipaddr));
}

/*
 * Returns non-zero if result waiting, stores IP and ASN into given pointers
 * (asn buffer is allocated by asn_poll)
 */
static int
asn_get_result(struct addr *ipaddr, char **asn)
{
   struct asn_reply reply;
   ssize_t numread;
   
   numread = read(asn_sock[PARENT], &reply, sizeof(reply));
   if (numread == -1) {
      if (errno == EAGAIN)
         return (0); /* no input waiting */
      else
         goto error;
   }
   if (numread == 0)
      goto error; /* EOF */
   if (numread != sizeof(reply))
      errx(1, "asn_get_result read got %zu, expected %zu",
         numread, sizeof(reply));

   /* Received successful reply. */
   memcpy(ipaddr, &reply.addr, sizeof(*ipaddr));
   if (reply.error != 0) {
      /* Identify common special cases.  */
      const char *type = "none";

      if (reply.addr.family == IPv6) {
         type = "unsupported";
      } else {
         assert(reply.addr.family == IPv4);
         if (IN_MULTICAST(htonl(reply.addr.ip.v4)))
            type = "multicast";
      }
      xasprintf(asn, "(%s)", type);
   }
   else  /* Correctly resolved name.  */
      *asn = xstrdup(reply.asplain);

   asn_unqueue(&reply.addr);
   return (1);

error:
   warn("asn_get_result: ignoring read error");
   /* FIXME: re-align to stream?  restart asn child? */
   return (0);
}

void
asn_poll(void)
{
   struct addr ip;
   char *asn;
   
   if (pid == -1)
      return; /* no child was started - we're not doing ASN lookups */
  
   while (asn_get_result(&ip, &asn)) {
      /* push into hosts_db */
      struct bucket *b = host_find(&ip);

      if (b == NULL) {
         verbosef("resolved %s to AS%s but it's not in the DB!",
            addr_to_str(&ip), asn);
         return;
      }
      if (b->u.host.asn != NULL) {
         verbosef("resolved %s to AS%s but it's already in the DB!",
            addr_to_str(&ip), asn);
         return;
      }
      b->u.host.asn = asn;
   }
}

/* ------------------------------------------------------------------------ */

struct qitem {
   STAILQ_ENTRY(qitem) entries;
   struct addr ip;
};

static STAILQ_HEAD(qhead, qitem) queue = STAILQ_HEAD_INITIALIZER(queue);

static void
enqueue(const struct addr *const ip)
{
   struct qitem *i;
   i = xmalloc(sizeof(*i));
   memcpy(&i->ip, ip, sizeof(i->ip));
   STAILQ_INSERT_TAIL(&queue, i, entries);
   verbosef("ASN: enqueued %s", addr_to_str(ip));
}

/* Return non-zero and populate <ip> pointer if queue isn't empty. */
static int
dequeue(struct addr *ip)
{
   struct qitem *i;

   i = STAILQ_FIRST(&queue);
   if (i == NULL)
      return (0);
   STAILQ_REMOVE_HEAD(&queue, entries);
   memcpy(ip, &i->ip, sizeof(*ip));
   free(i);
   verbosef("ASN: dequeued %s", addr_to_str(ip));
   return 1;
}

static void
xwrite(const int d, const void *buf, const size_t nbytes)
{
   ssize_t ret = write(d, buf, nbytes);

   if (ret == -1)
      err(1, "write");
   if (ret != (ssize_t)nbytes)
      err(1, "wrote %d bytes instead of all %d bytes", (int)ret, (int)nbytes);
}

static void
asn_main(void)
{
   struct addr ip;

   setproctitle("ASN child");
   fd_set_nonblock(asn_sock[CHILD]);
   verbosef("ASN child entering main ASN loop");
   for (;;) {
      int blocking;

      if (STAILQ_EMPTY(&queue)) {
         blocking = 1;
         fd_set_block(asn_sock[CHILD]);
         verbosef("asn_main entering blocking read loop");
      } else {
         blocking = 0;
         fd_set_nonblock(asn_sock[CHILD]);
         verbosef("asn_main non-blocking poll");
      }
      for (;;) {
         /* While we have input to process... */
         ssize_t numread = read(asn_sock[CHILD], &ip, sizeof(ip));
         if (numread == 0)
            exit(0); /* end of file, nothing more to do here. */
         if (numread == -1) {
            if (!blocking && (errno == EAGAIN))
               break; /* ran out of input */
            /* else error */
            err(1, "ASN: read failed");
         }
         if (numread != sizeof(ip))
            err(1, "ASN: read got %zu bytes, expecting %zu",
               numread, sizeof(ip));
         enqueue(&ip);
         if (blocking) {
            /* After one blocking read, become non-blocking so that when we
             * run out of input we fall through to queue processing.
             */
            blocking = 0;
            fd_set_nonblock(asn_sock[CHILD]);
         }
      }

      /* Process queue. */
      if (dequeue(&ip)) {
         struct asn_reply reply;
         char *asn;
         int ret;
         uint32_t hostlong;
    
         reply.addr = ip;
         
         /* Filter some common reserved IPs that won't have ASNs */
         hostlong = ntohl(ip.ip.v4);
         if (  ((hostlong & 0xffff0000) == 0xc0a80000) /* RFC1918 192.168. */
            || ((hostlong & 0xff000000) == 0x0a000000) /* RFC1918 10. */
            || ((hostlong & 0xfff00000) == 0xac100000) /* RFC1918 172.16. */
            || ((hostlong & 0xf0000000) == 0xe0000000) /* RFC5771 Multicast */
            || ((hostlong & 0xffff0000) == 0xa9fe0000) /* RFC3927 Link-local */
            || ((hostlong & 0xff000000) == 0x00000000) /* RFC1122 0. */
            ) {

            ret = 0;
            reply.error = 0;
            strlcpy(reply.asplain, "(none)", sizeof(reply.asplain));
            verbosef("ASN: skipping private/reserved address %s", addr_to_str(&ip));
         }
         else {
            switch (ip.family) {
               case IPv4:
                  ret = ip4_lookup_asn((char *)addr_to_str(&ip), &asn);
                  break;
               case IPv6:
                  ret = EAI_FAMILY;
                  verbosef("ASN: only supports IPv4 addresses at this time");
                  break;
               default:
                  errx(1, "unexpected ip.family = %d", ip.family);
            }
        
            if (ret != 0) {
               reply.asplain[0] = '\0';
               reply.error = ret;
            } else {
               assert(sizeof(reply.asplain) > sizeof(char *)); /* not just a ptr */
               strlcpy(reply.asplain, asn, sizeof(reply.asplain));
               reply.error = 0;
            }
         }
         
         fd_set_block(asn_sock[CHILD]);
         xwrite(asn_sock[CHILD], &reply, sizeof(reply));
         verbosef("ASN: %s is on AS %s", addr_to_str(&reply.addr),
            (ret == 0) ? reply.asplain : "(none)");
      }
   }
}

/*
 * Look up the Autonomous System Number for a given IPv4 address using the 
 * Team Cymru ASN DNS service. http://www.team-cymru.org/IP-ASN-mapping.html#dns
 *
 * Accepts: an IPv4 address, a buffer for the result
 *
 * Returns: 0 on success, or -1 on error
 */
int ip4_lookup_asn(char *ipaddr, char **asn) {
   
   int i = 0, len = 0;
   char reverse_ip[256];
   char query_host[256];     /* Hostname we send to the name server */
   char txt_plain[512];      /* Plain text from TXT RR */
   char *txt_asn;            /* Extracted ASN */
   u_char answer[PACKETSZ];  /* Raw response from the name server */
   ns_msg msg;               /* DNS message, which contains a... */
   ns_rr rr;                 /* DNS resource record */
   
   /* Build the hostname to look up */
   if (ip4_reverse_octets(ipaddr, reverse_ip, sizeof(reverse_ip)) < 0) {
      warnx("ip4_lookup_asn: reversing IP address failed");
      return -1;
   }
   sprintf(query_host, "%s.origin.asn.cymru.com", reverse_ip);
   
   /* Send a DNS TXT query */
   struct __res_state resstate;
   memset(&resstate, 0, sizeof(resstate));
   res_ninit(&resstate);
   
   if ((len = res_nquery(&resstate, query_host, C_IN, T_TXT, answer, 
       sizeof(answer))) < 0) {

      /* Try to figure out the exact error */
      HEADER *hans = (HEADER*)answer;
      
      switch (hans->rcode) {
         case FORMERR:
             verbosef("ASN: ip4_lookup_asn FORMERR for %s", query_host);
             break;
         case SERVFAIL:
             verbosef("ASN: ip4_lookup_asn SERVFAIL for %s", query_host);
             break;
         case NXDOMAIN:
             verbosef("ASN: ip4_lookup_asn NXDOMAIN for %s", query_host);
             break;
         case NOTIMP:
             verbosef("ASN: ip4_lookup_asn NOTIMP for %s", query_host);
             break;
         case REFUSED:
             verbosef("ASN: ip4_lookup_asn REFUSED for %s", query_host);
             break;
         default:
             verbosef("ASN: ip4_lookup_asn UNKNOWN for %s", query_host);
             break;
      }
      return -1;
   }
   
   /* Parse the response */
   if (ns_initparse(answer, len, &msg) != 0) {
      warnx("ip4_lookup_asn: ns_initparse() failed");
      return -1;
   }
   if (ns_msg_count(msg, ns_s_an) < 1) {
      warnx("ip4_lookup_asn: ns_msg_count() < 1");
      return -1;
   }
   
   /* We can only handle one ASN, so grab only the first RR */
   ns_parserr(&msg, ns_s_an, 0, &rr);
   ns_sprintrr(&msg, &rr, NULL, NULL, txt_plain, sizeof(txt_plain));
   
   /*
    * Parse out e.g. "3356" from txt_plain, which is formatted like this:
    * 4.2.2.4.origin.asn.cymru.com.  10M IN TXT  "3356 | 4.0.0.0/9 | US | arin | 1992-12-01"
    */
   if ((txt_asn = strtok(txt_plain, "\"")) != NULL) {
      if ((txt_asn = strtok(NULL, " ")) != NULL) {
         /* Success */
         *asn = xstrdup(txt_asn);
         return 0;
      }
   }

   return -1;
}

/*
 * Reverse the octets of an IPv4 address
 *
 * Accepts: an IPv4 address, a buffer for the result, the length of that buffer
 *
 * Returns: the length of the result in bytes, or -1 on error
 */
int ip4_reverse_octets(char *ip, char *buf, size_t buflen) {
   
   char reverse[15];
   int o1 = 0, o2 = 0, o3 = 0, o4 = 0;
   
   if (strlen(ip) > 15) {
      warnx("ip4_reverse_octets: supplied ip exceeds max IPv4 length");
      return -1;
   }
   
   sscanf(ip, "%d.%d.%d.%d", &o1, &o2, &o3, &o4); /* extract octets */
   sprintf(reverse, "%d.%d.%d.%d", o4, o3, o2, o1);
   
   if (strlen(reverse) > buflen) {
      warnx("ip4_reverse_octets: result was too large for buffer");
      return -1;   
   }
   
   strlcpy(buf, reverse, buflen);
   return strlen(reverse);
}

/* vim:set ts=3 sw=3 tw=78 expandtab: */
