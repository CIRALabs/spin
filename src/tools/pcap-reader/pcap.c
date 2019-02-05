/*
 * Copyright (c) 2017-2018 Caspar Schutijser <caspar.schutijser@sidn.nl>
 *
 * Portions of this code were taken from tcpdump, which includes this notice:
 *
 * Copyright (c) 1988, 1989, 1990, 1991, 1992, 1993, 1994, 1995, 1996, 1997
 *	The Regents of the University of California.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that: (1) source code distributions
 * retain the above copyright notice and this paragraph in its entirety, (2)
 * distributions including binary code include the above copyright notice and
 * this paragraph in its entirety in the documentation or other materials
 * provided with the distribution, and (3) all advertising materials mentioning
 * features or use of this software display the following acknowledgement:
 * ``This product includes software developed by the University of California,
 * Lawrence Berkeley Laboratory and its contributors.'' Neither the name of
 * the University nor the names of its contributors may be used to endorse
 * or promote products derived from this software without specific prior
 * written permission.
 * THIS SOFTWARE IS PROVIDED ``AS IS'' AND WITHOUT ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, WITHOUT LIMITATION, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE.
 */

#include <sys/socket.h>
#include <sys/types.h>

#include <net/if.h>
#ifdef HAVE_NET_ETHERTYPES_H
#include <net/ethertypes.h>
#endif // HAVE_NET_ETHERTYPES_H
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/ip6.h>
#include <netinet/icmp6.h>
#include <netinet/if_ether.h>
#define __FAVOR_BSD /* Who doesn't? */
#include <netinet/tcp.h>
#include <netinet/udp.h>

#include <assert.h>
#include <endian.h>
#include <err.h>
#include <errno.h>
#include <pcap.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "external/external.h"
#include "external/interface.h"
#include "external/extract.h" /* must come after interface.h */

#include "arp_table.h"
#include "dns.h"
#include "util.h"

/* Linux compat */
#ifndef IPV6_VERSION
#define IPV6_VERSION		0x60
#define IPV6_VERSION_MASK	0xf0
#endif /* IPV6_VERSION */

#ifdef __OpenBSD__
extern char *malloc_options;
#endif /* __OpenBSD__ */

const u_char *packetp;
const u_char *snapend;

static pcap_t *pd;

static struct arp_table *arp_table;

static void
sig_handler(int sig)
{
	int save_errno = errno;

	switch (sig) {
	case SIGINT:
	case SIGTERM:
		pcap_breakloop(pd);
		break;

	default:
		break;
	}

	errno = save_errno;
}

static void
usage(const char *error)
{
	extern char *__progname;

	if (error)
		fprintf(stderr, "%s\n", error);
	fprintf(stderr, "Usage: %s [-f filter] [-i interface] [-r file]\n",
	    __progname);
	exit(1);
}

static void
print_packet(const char *mac_from, const char *mac_to, const char *ip_from,
    const char *ip_to, uint8_t ip_proto, int tcp_initiated, int port_from,
    int port_to, int size, long long timestamp)
{
	printf("{");
	print_str_str("command", "traffic");
	print_str_str("argument", "");
	printf("\"result\": { ");

	printf("\"flows\": [ ");

	printf("{ ");

	print_fromto("from", mac_from, ip_from);
	print_fromto("to", mac_to, ip_to);
	print_str_n("protocol", ip_proto);
	if (tcp_initiated) {
		print_str_n("x_tcp_initiated", tcp_initiated);
	}
	print_str_n("from_port", port_from);
	print_str_n("to_port", port_to);
	print_str_n("size", size);
	print_str_n("count", 1);
	print_dummy_end();

	printf("} ");

	printf("], "); // flows

	print_str_n("timestamp", timestamp);
	print_str_n("total_size", -1);
	print_str_n("total_count", -1);
	print_dummy_end();

	printf("} "); // result

	printf("}");
	printf("\n");
}

static void
handle_icmp6(const u_char *bp, const struct ether_header *ep)
{
	const struct icmp6_hdr *dp;
	const struct nd_neighbor_advert *p;
	char ip[INET6_ADDRSTRLEN];
	struct ether_addr mac;
	char macstr[12+5+1];

	dp = (struct icmp6_hdr *)bp;

	TCHECK(dp->icmp6_code);
	switch (dp->icmp6_type) {
	case ND_NEIGHBOR_ADVERT:
		p = (const struct nd_neighbor_advert *)dp;

		TCHECK(p->nd_na_target);
		inet_ntop(AF_INET6, &p->nd_na_target, ip, sizeof(ip));

		memcpy(&mac.ether_addr_octet, ep->ether_shost,
		    sizeof(mac.ether_addr_octet));
		mactostr(&mac, macstr, sizeof(macstr));

		arp_table_add(arp_table, ip, macstr);
		break;

	default:
		break;
	}

	return;

 trunc:
	warnx("TRUNCATED");
}

static void
#ifdef __OpenBSD__ /* XXX */
handle_ip(const u_char *p, u_int length, const struct ether_header *ep,
    const struct bpf_timeval *ts)
#else
handle_ip(const u_char *p, u_int length, const struct ether_header *ep,
    const struct timeval *ts)
#endif
{
	const struct ip *ip;
	const struct ip6_hdr *ip6;
	const struct tcphdr *tp;
	const struct udphdr *up;
	const u_char *cp;
	u_int hlen, len;
	char *mac_from;
	char *mac_to;
	char ip_from[INET6_ADDRSTRLEN];
	char ip_to[INET6_ADDRSTRLEN];
	uint8_t ip_proto;
	int tcp_initiated = 0;
	int port_from = 0;
	int port_to = 0;

	switch (((struct ip *)p)->ip_v) {
	case 4:
		ip = (struct ip *)p;
		ip6 = NULL;
		break;
	case 6:
		ip = NULL;
		ip6 = (struct ip6_hdr *)p;
		break;
	default:
		DPRINTF("not an IP packet");
		return;
	}

	if (ip6) {
		if (length < sizeof(struct ip6_hdr)) {
			warnx("Truncated IPv6 packet: %d", length);
			goto out;
		}
		if ((ip6->ip6_vfc & IPV6_VERSION_MASK) != IPV6_VERSION) {
			warnx("Bad IPv6 version: %u", ip6->ip6_vfc >> 4);
			goto out;
		}
		hlen = sizeof(struct ip6_hdr);

		len = ntohs(ip6->ip6_plen);
		if (length < len + hlen) {
			warnx("Truncated IP6 packet: %d bytes missing",
			    len + hlen - length);
		}
	} else {
		TCHECK(*ip);
		len = ntohs(ip->ip_len);
		if (length < len) {
			warnx("Truncated IP packet: %d bytes missing",
			    len - length);
			len = length; // XXX
		}
		hlen = ip->ip_hl * 4;
		if (hlen < sizeof(struct ip) || hlen > len) {
			warnx("Bad header length: %d", hlen);
			goto out;
		}
		len -= hlen;
	}

	if (ip6) {
		// XXX extension headers
		ip_proto = ip6->ip6_ctlun.ip6_un1.ip6_un1_nxt;

		inet_ntop(AF_INET6, &ip6->ip6_src, ip_from, sizeof(ip_from));
		inet_ntop(AF_INET6, &ip6->ip6_dst, ip_to, sizeof(ip_to));
	} else {
		ip_proto = ip->ip_p;

		inet_ntop(AF_INET, &ip->ip_src, ip_from, sizeof(ip_from));
		inet_ntop(AF_INET, &ip->ip_dst, ip_to, sizeof(ip_to));
	}

	switch (ip_proto) {
	case IPPROTO_ICMPV6:
		if (ip6) {
			handle_icmp6((const u_char *)(ip6 + 1), ep);
		} else {
			warnx("ICMPv6 in IPv4 packet");
		}
		break;

	case IPPROTO_TCP:
		if (ip6) {
			tp = (const struct tcphdr *)((const u_char *)ip6 +
			    hlen);
		} else {
			tp = (const struct tcphdr *)((const u_char *)ip + hlen);
		}
		TCHECK(*tp);

		if ((tp->th_flags & (TH_SYN|TH_ACK)) == TH_SYN)
			tcp_initiated = 1;

		port_from = ntohs(tp->th_sport);
		port_to = ntohs(tp->th_dport);
		break;

	case IPPROTO_UDP:
		if (ip6) {
			up = (const struct udphdr *)((const u_char *)ip6 +
			    hlen);
		} else {
			up = (const struct udphdr *)((const u_char *)ip + hlen);
		}
		TCHECK(*up);

		port_from = ntohs(up->uh_sport);
		port_to = ntohs(up->uh_dport);
		break;

	default:
		DPRINTF("Unknown protocol: %d", ip_proto);
		break;
	}

	mac_from = arp_table_find_by_ip(arp_table, ip_from);
	mac_to = arp_table_find_by_ip(arp_table, ip_to);
	print_packet(mac_from, mac_to, ip_from, ip_to, ip_proto, tcp_initiated,
	    port_from, port_to, len, ts->tv_sec);

	if (port_from == 53 || port_to == 53) {
		if (up) {
			cp = (const u_char *)(up + 1);
		} else {
			cp = (const u_char *)(tp + 1);
		}
		TCHECK(*cp);
		handle_dns(cp, len, ts->tv_sec);
	}

	return;

 trunc:
	warnx("TRUNCATED\n");
	return;

 out:
	return; // XXX

}


static void
handle_arp(const u_char *bp, u_int length)
{
	const struct ether_arp *ap;
	u_short op;
	char ip[INET_ADDRSTRLEN];
	struct ether_addr mac;
	char macstr[12+5+1];

	ap = (struct ether_arp *)bp;
	if ((u_char *)(ap + 1) > snapend) {
		warnx("[|arp]");
		return;
	}
	if (length < sizeof(struct ether_arp)) {
		warnx("truncated arp");
		return;
	}

	op = EXTRACT_16BITS(&ap->arp_op);
	switch (op) {
	case ARPOP_REPLY:
		memcpy(&mac.ether_addr_octet, ap->arp_sha,
		    sizeof(mac.ether_addr_octet));

		mactostr(&mac, macstr, sizeof(macstr));
		inet_ntop(AF_INET, &ap->arp_spa, ip, sizeof(ip));

		arp_table_add(arp_table, ip, macstr);
		break;

	default:
		break;
	}
}

#ifdef SLOW
static int counter = 0;
#endif /* SLOW */
/*
 * Callback for libpcap. Handles a packet.
 */
static void
callback(u_char *user, const struct pcap_pkthdr *h, const u_char *sp)
{
	const u_char *p;
	const struct ether_header *ep;
	u_short ether_type;
	u_int caplen = h->caplen;

	if (h->caplen != h->len) {
		warnx("caplen %d != len %d, ", h->caplen, h->len);
	}

#ifdef SLOW
	++counter;
	if ((counter & 0xfff) == 0) {
		sleep(3);
		counter = 0;
	}
#endif /* SLOW */

	packetp = sp;
	snapend = sp + h->caplen;

	p = sp;

	if ((snapend - p) < sizeof(struct ether_header)) {
		warnx("[|ether]");
		goto out;
	}

	ep = (const struct ether_header *)p;
	ether_type = ntohs(ep->ether_type);

	p = sp + 14; /* Move past Ethernet header */
	caplen -= 14;

	switch (ether_type) {
	case ETHERTYPE_IP:
	case ETHERTYPE_IPV6:
		handle_ip(p, caplen, ep, &h->ts);
		break;

	case ETHERTYPE_ARP:
		handle_arp(p, caplen);
		break;

	default:
		DPRINTF("unknown ether type");
		break;
	}

 out:
	return; // XXX
}

int
main(int argc, char *argv[])
{
	int ch;
	char *device = NULL;
	char *file = NULL;
	char *pcap_errbuf;
	char *filter = "";
	struct bpf_program fp;

#ifdef __OpenBSD__
	/* Configure malloc on OpenBSD; enables security auditing options */
	malloc_options = "S";
#endif /* __OpenBSD__ */

	while ((ch = getopt(argc, argv, "f:hi:r:")) != -1) {
		switch(ch) {
		case 'f':
			filter = optarg;
			break;
		case 'h':
			usage(NULL);
		case 'i':
			device = optarg;
			break;
		case 'r':
			file = optarg;
			break;
		default:
			usage(NULL);
		}
	}

	if (device && file)
		usage("cannot specify both an interface and a file");
	if (!device && !file)
		device = "eth0";

	if ((pcap_errbuf = malloc(PCAP_ERRBUF_SIZE)) == NULL)
		err(1, "malloc");

	if (device) {
		pd = pcap_create(device, pcap_errbuf);
		if (!pd)
			errx(1, "pcap_create: %s", pcap_errbuf);

		if (pcap_set_snaplen(pd, 1514) != 0)
			errx(1, "pcap_set_snaplen");

		if (pcap_set_promisc(pd, 1) != 0)
			errx(1, "pcap_set_promisc");

		if (pcap_set_timeout(pd, 1000) != 0)
			errx(1, "pcap_set_timeout");

		if (pcap_activate(pd) != 0)
			errx(1, "pcap_activate: %s", pcap_geterr(pd));
	} else {
#ifdef HAVE_PLEDGE
		if (pledge("stdio bpf rpath", NULL) == -1)
			err(1, "pledge");
#endif /* HAVE_PLEDGE */

		pd = pcap_open_offline(file, pcap_errbuf);
		if (!pd)
			errx(1, "pcap_open_offline: %s", pcap_errbuf);
	}

	if (pcap_datalink(pd) != DLT_EN10MB)
		errx(1, "the device is not an Ethernet device");

	if (pcap_compile(pd, &fp, filter, 1, 0) == -1 ||
	    pcap_setfilter(pd, &fp) == -1)
		errx(1, "could not set filter: %s", pcap_geterr(pd));

#ifdef HAVE_PLEDGE
	if (pledge("stdio bpf", NULL) == -1)
		err(1, "pledge");
#endif /* HAVE_PLEDGE */

	arp_table = arp_table_create();

	(void)signal(SIGTERM, sig_handler);
	(void)signal(SIGINT, sig_handler);

	if (pcap_loop(pd, -1, callback, NULL) == -1)
		errx(1, "pcap_loop: %s", pcap_geterr(pd));

	/*
	 * Done, clean up.
	 */
	arp_table_destroy(arp_table);

	if (pd)
		pcap_close(pd);

	return 0;
}
