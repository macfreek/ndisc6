/*
 * traceroute.c - TCP/IPv6 traceroute tool
 * $Id$
 */

/***********************************************************************
 *  Copyright (C) 2005 Remi Denis-Courmont.                            *
 *  This program is free software; you can redistribute and/or modify  *
 *  it under the terms of the GNU General Public License as published  *
 *  by the Free Software Foundation; version 2 of the license.         *
 *                                                                     *
 *  This program is distributed in the hope that it will be useful,    *
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of     *
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.               *
 *  See the GNU General Public License for more details.               *
 *                                                                     *
 *  You should have received a copy of the GNU General Public License  *
 *  along with this program; if not, you can get it from:              *
 *  http://www.gnu.org/copyleft/gpl.html                               *
 ***********************************************************************/

#if HAVE_CONFIG_H
# include <config.h>
#endif

#define N_( str ) (str)
#define _( str ) (str)

#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <stdlib.h> /* div() */

#include <unistd.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <netinet/ip6.h>
#include <netinet/tcp.h>
#include <netinet/icmp6.h>
#include <netdb.h>
#include <arpa/inet.h> /* inet_ntop() */

static void
drop_priv (void)
{
	setuid (getuid ());
}


static int
send_tcp_probe (int fd, unsigned ttl, unsigned n, uint16_t port)
{
	struct tcphdr th = { };

	th.source = htons (1024);
	th.dest = port;
	th.seq = htonl ((ttl << 24) | (n << 16) | getpid ());
	th.doff = sizeof (th) / 4;
	th.syn = 1;
	th.window = htons (4096);

	return (send (fd, &th, sizeof (th), 0) == sizeof (th)) ? 0 : -1;
}


static int
parse_tcp_error (const void *data, size_t len, unsigned *ttl, unsigned *n,
                 uint16_t port)
{
	const struct tcphdr *pth = (const struct tcphdr *)data;
	uint32_t seq;

	if ((len < 8)
	 || (pth->source != htons (1024))
	 || (pth->dest != port))
		return -1;

	seq = ntohl (pth->seq);
	if ((seq & 0xffff) != getpid ())
		return -1;

	*ttl = seq >> 24;
	*n = (seq >> 16) & 0xff;
	return 0;
}


static int
parse_tcp_resp (const void *data, size_t len, unsigned *ttl, unsigned *n,
                uint16_t port)
{
	const struct tcphdr *pth = (const struct tcphdr *)data;
	uint32_t seq;

	if ((len < sizeof (*pth))
	 || (pth->dest != htons (1024))
	 || (pth->source != port)
	 || (pth->ack == 0)
	 || (pth->syn == pth->rst)
	 || (pth->doff < (sizeof (*pth) / 4)))
		return -1;

	seq = ntohl (pth->ack_seq) - 1;
	if ((seq & 0xffff) != getpid ())
		return -1;

	*ttl = seq >> 24;
	*n = (seq >> 16) & 0xff;
	return 1 + pth->syn;
}


static const struct tracetype
{
	int res_socktype;
	int protocol;
	int check_offset;
	int (*send_probe) (int fd, unsigned ttl, unsigned n, uint16_t port);
	int (*parse_resp) (const void *data, size_t len, unsigned *ttl,
	                   unsigned *n, uint16_t port);
	int (*parse_err) (const void *data, size_t len, unsigned *ttl,
	                   unsigned *n, uint16_t port);
} types[] =
{
	{ SOCK_STREAM,	IPPROTO_TCP, 16,
	  send_tcp_probe, parse_tcp_resp, parse_tcp_error },
	{ SOCK_DGRAM,	IPPROTO_RAW, -1, NULL, NULL , NULL }
};


/* Performs reverse lookup; print hostname and address */
static void
printname (const struct sockaddr *addr, size_t addrlen, int flags)
{
	char name[NI_MAXHOST];
	int val;

	val = getnameinfo (addr, addrlen, name, sizeof (name), NULL, 0, flags);
	if (!val)
		printf (" %s", name);

	val = getnameinfo (addr, addrlen, name, sizeof (name), NULL, 0,
	                   NI_NUMERICHOST | flags);
	if (!val)
		printf (" (%s) ", name);
}


/* Prints delay between two dates */
static void
printdelay (const struct timeval *from, const struct timeval *to)
{
	div_t d;
	if (to->tv_usec < from->tv_usec)
	{
		d = div (1000000 + to->tv_usec - from->tv_usec, 1000);
		d.quot -= 1000;
	}
	else
		d = div (to->tv_usec - from->tv_usec, 1000);

	printf (_(" %u.%03u ms "),
	        (unsigned)(d.quot + 1000 * (to->tv_sec - from->tv_sec)),
	        (unsigned)d.rem);
}


static int
traceroute (const char *hostname, const char *service, unsigned timeout,
            unsigned retries, unsigned min_ttl, unsigned max_ttl, int niflags)
{
	struct sockaddr_in6 dst = { };
	const struct tracetype *t = types;
	int protofd, icmpfd, maxfd, found;
	unsigned ttl, n;

	/* Creates ICMPv6 socket to collect error packets */
	icmpfd = socket (AF_INET6, SOCK_RAW, IPPROTO_ICMPV6);
	if (icmpfd == -1)
	{
		perror (_("Raw socket"));
		return -1;
	}

	/* Creates protocol-specific socket */
	protofd = socket (AF_INET6, SOCK_RAW, t->protocol);
	if (protofd == -1)
	{
		perror (_("Raw socket"));
		close (icmpfd);
		return -1;
	}

	/* Drops privileges permanently */
	drop_priv ();

	/* Set ICMPv6 filter */
	{
		struct icmp6_filter f;

		ICMP6_FILTER_SETBLOCKALL (&f);
		ICMP6_FILTER_SETPASS (ICMP6_DST_UNREACH, &f);
		ICMP6_FILTER_SETPASS (ICMP6_TIME_EXCEEDED, &f);
		setsockopt (icmpfd, SOL_ICMPV6, ICMP6_FILTER, &f, sizeof (f));
	}

	/* Defines protocol-specific checksum offset */
	if ((t->check_offset != -1)
	 && setsockopt (protofd, SOL_IPV6, IPV6_CHECKSUM, &t->check_offset,
	                sizeof (int)))
	{
		perror ("setsockopt(IPV6_CHECKSUM)");
		goto error;
	}
	else
	{
		/* Defines destination */
		struct addrinfo hints = { }, *res;
		int val;

		hints.ai_family = AF_INET6;
		hints.ai_socktype = t->res_socktype;

		val = getaddrinfo (hostname, service, &hints, &res);
		if (val)
		{
			if (service != NULL)
				fprintf (stderr, _("%s port %s: %s\n"), hostname, service,
				         gai_strerror (val));
			else
				fprintf (stderr, _("%s: %s\n"), hostname, gai_strerror (val));

			goto error;
		}

		if (res->ai_addrlen > sizeof (dst))
			goto error;

		if (connect (protofd, res->ai_addr, res->ai_addrlen))
		{
			perror (hostname);
			freeaddrinfo (res);
			goto error;
		}
		else
		{
			char buf[INET6_ADDRSTRLEN];
			socklen_t len = sizeof (dst);

			fputs (_("traceroute to"), stdout);
			printname (res->ai_addr, res->ai_addrlen, niflags);
			if ((getsockname (protofd, (struct sockaddr *)&dst, &len) == 0)
			 && inet_ntop (AF_INET6, &dst.sin6_addr, buf, sizeof (buf)))
				printf (_("from %s, "), buf);

			/* TODO: print port number or packet length */
			memcpy (&dst, res->ai_addr, res->ai_addrlen);
			printf (_("port %u, "), ntohs (dst.sin6_port));
			printf (_("%u hops max\n"), max_ttl);
		}

		freeaddrinfo (res);
	}

	maxfd = 1 + ((icmpfd > protofd) ? icmpfd : protofd);

	for (ttl = min_ttl, found = 0; (ttl <= max_ttl) && !found; ttl++)
	{
		struct in6_addr hop = { }; /* hop if known from previous probes */
		int state = -1; /* type of response received so far (-1: none,
			0: normal, 1: closed, 2: open) */
		/* see also: found (0: not found, <0: unreachable, >0: reached) */

		printf ("%2d ", ttl);
		if (setsockopt (protofd, SOL_IPV6, IPV6_UNICAST_HOPS,
		                &ttl, sizeof (ttl)))
			goto error;

		for (n = 0; n < retries; n++)
		{
			fd_set rdset;
			struct timeval tv = { timeout, 0 }, sent, recvd;
			unsigned pttl, pn;
			int val;

			FD_ZERO (&rdset);
			FD_SET (protofd, &rdset);
			FD_SET (icmpfd, &rdset);

			gettimeofday (&sent, NULL);
			if (t->send_probe (protofd, ttl, n, dst.sin6_port))
			{
				perror (_("Cannot send packet"));
				goto error;
			}

			do
			{
				val = select (maxfd, &rdset, NULL, NULL, &tv);
				if (val < 0) /* interrupted by signal - well, not really */
					goto error;

				if (val == 0)
				{
					fputs (" *", stdout);
					continue;
				}

				gettimeofday (&recvd, NULL);

				/* Receive final packet when host reached */
				if (val && FD_ISSET (protofd, &rdset))
				{
					uint8_t buf[1240];
					int len;

					len = recv (protofd, buf, sizeof (buf), 0);
					if (len < 0)
						continue;

					len = t->parse_resp (buf, len, &pttl, &pn, dst.sin6_port);
					if ((len >= 0) && (n == pn) && (pttl = ttl))
					{
						/* Route determination complete! */
						if (state == -1)
							printname ((struct sockaddr *)&dst, sizeof (dst),
							           niflags);

						if (len != state)
						{
							const char *msg = NULL;

							switch (len)
							{
								case 1:
									msg = N_("closed");
									break;

								case 2:
									msg = N_("open");
									break;
							}

							if (msg != NULL)
								printf ("[%s] ", msg);

							state = len;
						}

						printdelay (&sent, &recvd);
						val = 0;
						found = ttl;
					}
				}

				/* Receive ICMP errors along the way */
				if (val && FD_ISSET (icmpfd, &rdset))
				{
					struct
					{
						struct icmp6_hdr hdr;
						struct ip6_hdr inhdr;
						uint8_t buf[1192];
					} pkt;
					struct sockaddr_in6 peer;
					socklen_t peerlen = sizeof (peer);
					int len;

					len = recvfrom (icmpfd, &pkt, sizeof (pkt), 0,
					                (struct sockaddr *)&peer, &peerlen);

					/* FIXME: support further (all?) ICMPv6 errors */
					if ((len < (sizeof (pkt.hdr) + sizeof (pkt.inhdr)))
					 || ((pkt.hdr.icmp6_type != ICMP6_DST_UNREACH)
					  && ((pkt.hdr.icmp6_type != ICMP6_TIME_EXCEEDED)
					   || (pkt.hdr.icmp6_code != ICMP6_TIME_EXCEED_TRANSIT)))
					 || memcmp (&pkt.inhdr.ip6_dst, &dst.sin6_addr, 16)
					 || (pkt.inhdr.ip6_nxt != t->protocol))
						continue;
					len -= sizeof (pkt.hdr) + sizeof (pkt.inhdr);

					len = t->parse_err (pkt.buf, len, &pttl, &pn,
					                    dst.sin6_port);
					if ((len < 0) || (pttl != ttl) || (pn != n))
						continue;

					/* genuine ICMPv6 error that concerns us */
					if ((state == -1) || memcmp (&hop, &peer.sin6_addr, 16))
					{
						memcpy (&hop, &peer.sin6_addr, 16);
						printname ((struct sockaddr *)&peer, peerlen,
						           niflags);
						state = 0;
					}
					printdelay (&sent, &recvd);
					val = 0;

					if (pkt.hdr.icmp6_type == ICMP6_DST_UNREACH)
					{
						/* No path to destination */
						char c = '\0';
						found = -ttl;

						switch (pkt.hdr.icmp6_code)
						{
							case ICMP6_DST_UNREACH_NOROUTE:
								c = 'N';
								break;

							case ICMP6_DST_UNREACH_ADMIN:
								c = 'S';
								break;

							case ICMP6_DST_UNREACH_ADDR:
								c = 'H';
								break;

							case ICMP6_DST_UNREACH_NOPORT:
								found = ttl; /* success! */
								break;
						}

						if (c)
							printf ("!%c ", c);
					}
				}
			}
			while (val > 0);
		}
		puts ("");
	}

	close (protofd);
	close (icmpfd);
	return found > 0 ? 0 : -2;

error:
	close (protofd);
	close (icmpfd);
	return -1;
}


static int
quick_usage (const char *path)
{
	drop_priv ();

	fprintf (stderr, _("Try \"%s -h\" for more information.\n"), path);
	return 2;
}


static int
usage (const char *path)
{
	drop_priv ();

	fprintf (stderr,
	         _("Usage: %s [options] <IPv6 hostname/address> [port number]\n"
	         /* [packet len]*/
	         "Print IPv6 network route to a host\n"), path);

	fputs (_("\n"
/*"  -A  send TCP ACK probes\n"*/
/*"  -d  enable debugging\n"*/
/*"  -E  enable TCP Explicit Congestion Notification\n"*/
"  -f  specify the initial hop limit (default: 1)\n"
"  -h  display this help and exit\n"
/*"  -I  use ICMPv6 Echo packets as probes\n"*/
/*"  -i  specify outgoing interface\n"*/
/*"  -l  display incoming packets hop limit\n"*/
"  -m  set the maximum hop limit (default: 30)\n"
"  -n  don't perform reverse name lookup on addresses\n"
/*"  -p  override base destination UDP port\n"*/
/*"  -p  override source TCP port\n"*/
"  -q  override the number of probes per hop (default: 3)\n"
/*"  -S  send TCP SYN probes (default for TCP probes)\n"*/
/*"  -s  specify the source IPv6 address of probe packets\n"*/
"  -V, --version  display program version and exit\n"
/*"  -v, --verbose  display all kind of ICMPv6 errors\n"*/
"  -w  override the timeout for response in seconds (default: 5)\n"
/*"  -z  specify a time to wait (in ms) between each probes (default: 0)\n"*/
	"\n"), stderr);

	return 0;
}


static int
version (void)
{
	drop_priv ();

	puts (
"tcptraceroute6 : TCP/IPv6 traceroute tool "PACKAGE_VERSION
" ($Rev$)\n built "__DATE__"\n"
"Copyright (C) 2005 Remi Denis-Courmont");
	puts (_(
"This is free software; see the source for copying conditions.\n"
"There is NO warranty; not even for MERCHANTABILITY or\n"
"FITNESS FOR A PARTICULAR PURPOSE.\n"));
	printf (_("Written by %s.\n"), "Remi Denis-Courmont");
	return 0;
}


static int
parse_hlim (const char *str)
{
	char *end;
	unsigned long u;

	u = strtoul (str, &end, 0);
	if ((u > 255) || *end)
	{
		fprintf (stderr, _("%s: invalid hop limit\n"), str);
		return -1;
	}
	return u;
}


int
main (int argc, char *argv[])
{
	int val;
	unsigned retries = 3, niflags = 0, wait = 5, minhlim = 1, maxhlim = 30;
	const char *hostname, *port;

	while ((val = getopt (argc, argv, "f:hm:nq:Vw:")) != EOF)
	{
		switch (val)
		{
			case 'f':
				if ((minhlim = parse_hlim (optarg)) < 0)
					return 1;
				break;

			case 'h':
				return usage (argv[0]);

			case 'm':
				if ((maxhlim = parse_hlim (optarg)) < 0)
					return 1;
				break;

			case 'n':
				niflags |= NI_NUMERICHOST;
				break;

			case 'q':
			{
				unsigned long l;
				char *end;

				l = strtoul (optarg, &end, 0);
				if (*end || l > UINT_MAX)
					return quick_usage (argv[0]);
				retries = l;
				break;
			}
				
			case 'V':
				return version ();

			case 'w':
			{
				unsigned long l;
				char *end;

				l = strtoul (optarg, &end, 0);
				if (*end || l > UINT_MAX)
					return quick_usage (argv[0]);
				wait = l;
				break;
			}

			case '?':
			default:
				return quick_usage (argv[0]);
		}
	}

	if (optind < argc)
	{
		hostname = argv[optind++];

		if (optind < argc)
			port = argv[optind++];
		else
			port = "80";
	}
	else
		return quick_usage (argv[0]);

	setvbuf (stdout, NULL, _IONBF, 0);
	return -traceroute (hostname, port, wait, retries, minhlim, maxhlim,
	                    niflags);
}