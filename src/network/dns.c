/* SPDX-License-Identifier: MIT */
/* Minimal DNS A-record resolver (UDP) — robust for QEMU/VirtualBox NAT */
#include "dns.h"
#include "udp.h"
#include "net.h"
#include "arp.h"
#include "ethernet.h"

#define DNS_CLIENT_PORT  49152

static net_u16 dns_id;
static net_u32 dns_result;
static net_bool dns_done;
static net_u16 dns_expect_id;
static net_u8  dns_rcode;

void dns_init(void)
{
    dns_id = 0x2000;
    dns_result = 0;
    dns_done = NET_FALSE;
    dns_rcode = 0;
}

void dns_udp_receive(const net_u8 *payload, net_u32 len, net_u32 src_ip)
{
    (void)src_ip;
    if (len < 12) return;

    net_u16 id = ((net_u16)payload[0] << 8) | payload[1];
    if (id != dns_expect_id) return;

    net_u16 flags = ((net_u16)payload[2] << 8) | payload[3];
    if ((flags & 0x8000) == 0) return; /* not a response */

    dns_rcode = (net_u8)(flags & 0xF);
    net_u16 ancount = ((net_u16)payload[6] << 8) | payload[7];

    if (dns_rcode != 0 || ancount == 0) {
        dns_result = 0;
        dns_done = NET_TRUE;
        return;
    }

    /* Skip question section */
    net_u32 i = 12;
    while (i < len) {
        net_u8 lab = payload[i];
        if (lab == 0) { i++; break; }
        if ((lab & 0xC0) == 0xC0) { i += 2; break; }
        i += 1u + lab;
        if (i >= len) return;
    }
    if (i + 4 > len) return;
    i += 4; /* qtype + qclass */

    /* Walk answers */
    for (net_u16 a = 0; a < ancount && i < len; a++) {
        /* name (labels or compression pointer) */
        if (i >= len) return;
        if ((payload[i] & 0xC0) == 0xC0) {
            i += 2;
        } else {
            while (i < len) {
                net_u8 lab = payload[i];
                if (lab == 0) { i++; break; }
                if ((lab & 0xC0) == 0xC0) { i += 2; break; }
                i += 1u + lab;
            }
        }
        if (i + 10 > len) return;

        net_u16 type  = ((net_u16)payload[i] << 8) | payload[i + 1];
        /* class at i+2, ttl at i+4 */
        net_u16 rdlen = ((net_u16)payload[i + 8] << 8) | payload[i + 9];
        i += 10;
        if (i + rdlen > len) return;

        if (type == 1 && rdlen == 4) {
            /* A record: wire order octets -> our net_ip layout */
            dns_result = (net_u32)payload[i]
                       | ((net_u32)payload[i + 1] << 8)
                       | ((net_u32)payload[i + 2] << 16)
                       | ((net_u32)payload[i + 3] << 24);
            dns_done = NET_TRUE;
            return;
        }
        i += rdlen; /* skip CNAME etc. */
    }

    dns_result = 0;
    dns_done = NET_TRUE;
}

static net_u32 encode_name(net_u8 *dst, const char *host)
{
    net_u32 pos = 0;
    while (*host) {
        const char *dot = host;
        while (*dot && *dot != '.') dot++;
        net_u32 lab = (net_u32)(dot - host);
        if (lab == 0 || lab > 63) return 0;
        dst[pos++] = (net_u8)lab;
        for (net_u32 j = 0; j < lab; j++)
            dst[pos++] = (net_u8)host[j];
        host = *dot ? dot + 1 : dot;
    }
    dst[pos++] = 0;
    return pos;
}

static void busy_poll(net_u32 n)
{
    for (volatile net_u32 i = 0; i < n; i++) {
        if ((i & 0xFF) == 0)
            net_poll();
    }
}

/* Ensure we can reach DNS server at L2 */
static net_bool ensure_dns_arp(net_u32 dns_ip)
{
    net_u8 mac[6];
    if (arp_resolve(dns_ip, mac))
        return NET_TRUE;
    for (int t = 0; t < 8; t++) {
        busy_poll(200000);
        if (arp_resolve(dns_ip, mac))
            return NET_TRUE;
    }
    return NET_FALSE;
}

static net_u32 dns_query_one(const char *hostname, net_u32 dns_server,
                             void (*printfn)(const char *fmt, ...))
{
    net_u8 buf[512];
    for (int i = 0; i < 512; i++) buf[i] = 0;

    dns_id++;
    if (dns_id == 0) dns_id = 1;
    dns_expect_id = dns_id;
    dns_done = NET_FALSE;
    dns_result = 0;
    dns_rcode = 0;

    buf[0] = (net_u8)(dns_id >> 8);
    buf[1] = (net_u8)(dns_id & 0xFF);
    buf[2] = 0x01; /* RD */
    buf[3] = 0x00;
    buf[4] = 0x00; buf[5] = 0x01; /* 1 question */
    buf[6] = 0; buf[7] = 0;
    buf[8] = 0; buf[9] = 0;
    buf[10] = 0; buf[11] = 0;

    net_u32 nlen = encode_name(buf + 12, hostname);
    if (!nlen) return 0;
    net_u32 pos = 12 + nlen;
    buf[pos++] = 0x00; buf[pos++] = 0x01; /* TYPE A */
    buf[pos++] = 0x00; buf[pos++] = 0x01; /* CLASS IN */

    if (!ensure_dns_arp(dns_server)) {
        if (printfn) printfn("dns: ARP failed for server\n");
        return 0;
    }
    /* let ARP reply settle, then query */
    busy_poll(100000);

    for (int try = 0; try < 6 && !dns_done; try++) {
        udp_send(dns_server, DNS_CLIENT_PORT, 53, buf, pos);
        busy_poll(800000);
    }

    if (!dns_done) {
        if (printfn) printfn("dns: timeout\n");
        return 0;
    }
    if (dns_rcode != 0) {
        if (printfn) printfn("dns: rcode %u\n", (unsigned)dns_rcode);
        return 0;
    }
    return dns_result;
}

net_u32 dns_resolve(const char *hostname, void (*printfn)(const char *fmt, ...))
{
    if (!hostname || !hostname[0]) return 0;
    if (!g_netif.initialized) {
        if (printfn) printfn("dns: no NIC\n");
        return 0;
    }

    net_u32 direct = net_str_to_ip(hostname);
    if (direct) return direct;

    net_u32 servers[3];
    int nsrv = 0;
    if (g_netif.dns) servers[nsrv++] = g_netif.dns;
    servers[nsrv++] = NET_DEFAULT_DNS;          /* 10.0.2.3 */
    servers[nsrv++] = net_ip(8, 8, 8, 8);       /* fallback */

    for (int s = 0; s < nsrv; s++) {
        /* skip duplicates */
        int dup = 0;
        for (int j = 0; j < s; j++) if (servers[j] == servers[s]) dup = 1;
        if (dup) continue;

        char d[16];
        net_ip_to_str(servers[s], d);
        if (printfn) printfn("DNS query %s via %s\n", hostname, d);

        net_u32 ip = dns_query_one(hostname, servers[s], printfn);
        if (ip) {
            if (printfn) {
                char ib[16];
                net_ip_to_str(ip, ib);
                printfn("dns: %s -> %s\n", hostname, ib);
            }
            return ip;
        }
    }

    if (printfn) printfn("dns: failed for %s\n", hostname);
    return 0;
}

void dns_cmd_lookup(const char *arg, void (*printfn)(const char *fmt, ...))
{
    while (arg && *arg == ' ') arg++;
    if (!arg || !*arg) {
        printfn("usage: nslookup <hostname>\n");
        return;
    }
    dns_resolve(arg, printfn);
}
