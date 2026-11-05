/* SPDX-License-Identifier: MIT */
/* Minimal DHCP client: DISCOVER -> OFFER -> REQUEST -> ACK */
#include "dhcp.h"
#include "udp.h"
#include "net.h"
#include "ethernet.h"

#define DHCP_SERVER_PORT 67
#define DHCP_CLIENT_PORT 68
#define DHCP_MAGIC 0x63825363u

#define DHCP_DISCOVER 1
#define DHCP_OFFER    2
#define DHCP_REQUEST  3
#define DHCP_ACK      5
#define DHCP_NAK      6

#define OPT_SUBNET    1
#define OPT_ROUTER    3
#define OPT_DNS       6
#define OPT_REQIP     50
#define OPT_LEASE     51
#define OPT_MSGTYPE   53
#define OPT_SERVERID  54
#define OPT_PARAMREQ  55
#define OPT_END       255

typedef struct {
    net_u8  op, htype, hlen, hops;
    net_u32 xid;
    net_u16 secs, flags;
    net_u32 ciaddr, yiaddr, siaddr, giaddr;
    net_u8  chaddr[16];
    net_u8  sname[64];
    net_u8  file[128];
    net_u32 magic;
    net_u8  options[312];
} __attribute__((packed)) dhcp_msg_t;

typedef enum { DHCP_IDLE, DHCP_WAIT_OFFER, DHCP_WAIT_ACK, DHCP_BOUND } dhcp_state_t;

static dhcp_state_t state;
static net_u32 xid;
static net_u32 offer_ip, offer_server, offer_mask, offer_gw, offer_dns;
static net_bool got_offer, got_ack, got_nak;

static net_u32 dhcp_xid_new(void)
{
    static net_u32 s = 0x5449414Eu; /* TIAN */
    s = s * 1664525u + 1013904223u;
    return s;
}

static void dhcp_build_discover(dhcp_msg_t *m)
{
    for (net_u32 i = 0; i < sizeof(*m); i++) ((net_u8 *)m)[i] = 0;
    m->op = 1; m->htype = 1; m->hlen = 6; m->hops = 0;
    m->xid = xid; /* already wire-ish LE */
    m->flags = net_htons(0x8000); /* broadcast */
    for (int i = 0; i < 6; i++) m->chaddr[i] = g_netif.mac[i];
    m->magic = net_htonl(DHCP_MAGIC);
    net_u8 *o = m->options;
    *o++ = OPT_MSGTYPE; *o++ = 1; *o++ = DHCP_DISCOVER;
    *o++ = OPT_PARAMREQ; *o++ = 3; *o++ = OPT_SUBNET; *o++ = OPT_ROUTER; *o++ = OPT_DNS;
    *o++ = OPT_END;
}

static void dhcp_build_request(dhcp_msg_t *m)
{
    for (net_u32 i = 0; i < sizeof(*m); i++) ((net_u8 *)m)[i] = 0;
    m->op = 1; m->htype = 1; m->hlen = 6;
    m->xid = xid;
    m->flags = net_htons(0x8000);
    for (int i = 0; i < 6; i++) m->chaddr[i] = g_netif.mac[i];
    m->magic = net_htonl(DHCP_MAGIC);
    net_u8 *o = m->options;
    *o++ = OPT_MSGTYPE; *o++ = 1; *o++ = DHCP_REQUEST;
    *o++ = OPT_REQIP; *o++ = 4;
    { net_u32 ip = offer_ip; *o++ = (net_u8)(ip); *o++ = (net_u8)(ip>>8); *o++ = (net_u8)(ip>>16); *o++ = (net_u8)(ip>>24); }
    *o++ = OPT_SERVERID; *o++ = 4;
    { net_u32 ip = offer_server; *o++ = (net_u8)(ip); *o++ = (net_u8)(ip>>8); *o++ = (net_u8)(ip>>16); *o++ = (net_u8)(ip>>24); }
    *o++ = OPT_PARAMREQ; *o++ = 3; *o++ = OPT_SUBNET; *o++ = OPT_ROUTER; *o++ = OPT_DNS;
    *o++ = OPT_END;
}

static void parse_options(const net_u8 *opt, net_u32 len, net_u8 *msgtype,
                          net_u32 *mask, net_u32 *gw, net_u32 *dns, net_u32 *server)
{
    *msgtype = 0;
    net_u32 i = 0;
    while (i < len) {
        net_u8 code = opt[i++];
        if (code == OPT_END) break;
        if (code == 0) continue;
        if (i >= len) break;
        net_u8 l = opt[i++];
        if (i + l > len) break;
        if (code == OPT_MSGTYPE && l >= 1) *msgtype = opt[i];
        else if (code == OPT_SUBNET && l >= 4 && mask)
            *mask = (net_u32)opt[i] | ((net_u32)opt[i+1]<<8) | ((net_u32)opt[i+2]<<16) | ((net_u32)opt[i+3]<<24);
        else if (code == OPT_ROUTER && l >= 4 && gw)
            *gw = (net_u32)opt[i] | ((net_u32)opt[i+1]<<8) | ((net_u32)opt[i+2]<<16) | ((net_u32)opt[i+3]<<24);
        else if (code == OPT_DNS && l >= 4 && dns)
            *dns = (net_u32)opt[i] | ((net_u32)opt[i+1]<<8) | ((net_u32)opt[i+2]<<16) | ((net_u32)opt[i+3]<<24);
        else if (code == OPT_SERVERID && l >= 4 && server)
            *server = (net_u32)opt[i] | ((net_u32)opt[i+1]<<8) | ((net_u32)opt[i+2]<<16) | ((net_u32)opt[i+3]<<24);
        i += l;
    }
}

void dhcp_init(void)
{
    state = DHCP_IDLE;
    got_offer = got_ack = got_nak = NET_FALSE;
    offer_ip = offer_server = offer_mask = offer_gw = offer_dns = 0;
}

net_bool dhcp_is_bound(void) { return state == DHCP_BOUND; }

void dhcp_udp_receive(const net_u8 *payload, net_u32 len, net_u32 src_ip, net_u16 src_port)
{
    (void)src_port;
    if (len < 240) return;
    const dhcp_msg_t *m = (const dhcp_msg_t *)payload;
    if (m->op != 2) return;
    if (m->xid != xid) return;
    if (net_ntohl(m->magic) != DHCP_MAGIC && m->magic != net_htonl(DHCP_MAGIC)) {
        /* try raw compare for LE magic field written with htonl */
        if (m->magic != 0x63538263 && m->magic != 0x63825363) return;
    }

    net_u8 msgtype = 0;
    net_u32 mask = 0, gw = 0, dns = 0, server = src_ip;
    net_u32 opt_len = len - 240;
    if (opt_len > 312) opt_len = 312;
    parse_options(m->options, opt_len, &msgtype, &mask, &gw, &dns, &server);

    if (msgtype == DHCP_OFFER && state == DHCP_WAIT_OFFER) {
        offer_ip = m->yiaddr; /* already internal/wire LE format */
        offer_server = server ? server : src_ip;
        if (mask) offer_mask = mask;
        if (gw) offer_gw = gw;
        if (dns) offer_dns = dns;
        got_offer = NET_TRUE;
    } else if (msgtype == DHCP_ACK && state == DHCP_WAIT_ACK) {
        offer_ip = m->yiaddr;
        if (mask) offer_mask = mask;
        if (gw) offer_gw = gw;
        if (dns) offer_dns = dns;
        got_ack = NET_TRUE;
    } else if (msgtype == DHCP_NAK) {
        got_nak = NET_TRUE;
    }
}

void dhcp_poll(void)
{
    /* receive path is via udp_receive -> dhcp_udp_receive */
}

static void busy_wait_poll(net_u32 n)
{
    for (volatile net_u32 i = 0; i < n; i++) {
        if ((i & 0x1FF) == 0) net_poll();
    }
}

net_bool dhcp_run(void (*printfn)(const char *fmt, ...))
{
    if (!g_netif.initialized) {
        printfn("dhcp: no NIC\n");
        return NET_FALSE;
    }

    dhcp_init();
    xid = dhcp_xid_new();
    /* temporarily use 0.0.0.0 for DHCP */
    net_u32 saved_ip = g_netif.ip;
    g_netif.ip = 0;

    dhcp_msg_t msg;
    dhcp_build_discover(&msg);
    printfn("DHCP DISCOVER...\n");
    state = DHCP_WAIT_OFFER;
    got_offer = NET_FALSE;

    for (int attempt = 0; attempt < 4 && !got_offer; attempt++) {
        udp_send(net_ip(255,255,255,255), DHCP_CLIENT_PORT, DHCP_SERVER_PORT,
                 &msg, 240 + 16);
        busy_wait_poll(400000);
    }
    if (!got_offer) {
        printfn("dhcp: no OFFER\n");
        g_netif.ip = saved_ip;
        state = DHCP_IDLE;
        return NET_FALSE;
    }

    char ipbuf[16];
    net_ip_to_str(offer_ip, ipbuf);
    printfn("DHCP OFFER %s\n", ipbuf);

    dhcp_build_request(&msg);
    printfn("DHCP REQUEST...\n");
    state = DHCP_WAIT_ACK;
    got_ack = got_nak = NET_FALSE;
    for (int attempt = 0; attempt < 4 && !got_ack && !got_nak; attempt++) {
        udp_send(net_ip(255,255,255,255), DHCP_CLIENT_PORT, DHCP_SERVER_PORT,
                 &msg, 240 + 24);
        busy_wait_poll(400000);
    }
    if (!got_ack) {
        printfn("dhcp: no ACK\n");
        g_netif.ip = saved_ip;
        state = DHCP_IDLE;
        return NET_FALSE;
    }

    g_netif.ip = offer_ip;
    if (offer_mask) g_netif.netmask = offer_mask;
    if (offer_gw) g_netif.gateway = offer_gw;
    if (offer_dns) g_netif.dns = offer_dns;
    else g_netif.dns = NET_DEFAULT_DNS;

    state = DHCP_BOUND;
    net_ip_to_str(g_netif.ip, ipbuf);
    printfn("DHCP ACK bound %s\n", ipbuf);
    return NET_TRUE;
}
