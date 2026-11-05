/* SPDX-License-Identifier: MIT */
/* ============================================================================
 *  network/icmp.c  -- Echo Request / Reply + ping state machine
 * ==========================================================================*/
#include "icmp.h"
#include "ipv4.h"
#include "net.h"

ping_state_t g_ping;

void icmp_init(void)
{
    g_ping.active   = NET_FALSE;
    g_ping.waiting  = NET_FALSE;
    g_ping.replies  = 0;
    g_ping.timeouts = 0;
    g_ping.seq      = 0;
}

static net_u16 icmp_checksum(const void *data, net_u32 len)
{
    return ipv4_checksum(data, len);
}

void icmp_receive(const net_u8 *packet, net_u32 len, net_u32 src_ip)
{
    if (len < ICMP_HEADER_LEN) return;

    const icmp_header_t *hdr = (const icmp_header_t *)packet;

    if (hdr->type == ICMP_TYPE_ECHO_REQUEST) {
        /* Reply to ping directed at us */
        net_u8 reply[ICMP_HEADER_LEN + ICMP_PAYLOAD_MAX];
        net_u32 plen = len;
        if (plen > sizeof(reply)) plen = sizeof(reply);

        for (net_u32 i = 0; i < plen; i++)
            reply[i] = packet[i];

        icmp_header_t *rh = (icmp_header_t *)reply;
        rh->type = ICMP_TYPE_ECHO_REPLY;
        rh->code = 0;
        rh->checksum = 0;
        rh->checksum = icmp_checksum(reply, plen);

        ipv4_send(src_ip, IP_PROTO_ICMP, reply, plen);
        return;
    }

    if (hdr->type == ICMP_TYPE_ECHO_REPLY) {
        if (!g_ping.active || !g_ping.waiting) return;
        if (net_ntohs(hdr->id) != g_ping.id) return;
        if (net_ntohs(hdr->seq) != g_ping.seq) return;

        g_ping.replies++;
        g_ping.waiting = NET_FALSE;
        /* The shell side will print via net_tick / command */
    }
}

net_bool icmp_send_echo(net_u32 dst_ip, net_u16 id, net_u16 seq,
                        const void *payload, net_u32 payload_len)
{
    net_u8 buf[ICMP_HEADER_LEN + ICMP_PAYLOAD_MAX];
    if (payload_len > ICMP_PAYLOAD_MAX) payload_len = ICMP_PAYLOAD_MAX;

    icmp_header_t *hdr = (icmp_header_t *)buf;
    hdr->type     = ICMP_TYPE_ECHO_REQUEST;
    hdr->code     = 0;
    hdr->checksum = 0;
    hdr->id       = net_htons(id);
    hdr->seq      = net_htons(seq);

    for (net_u32 i = 0; i < payload_len; i++)
        buf[ICMP_HEADER_LEN + i] = ((const net_u8 *)payload)[i];

    net_u32 total = ICMP_HEADER_LEN + payload_len;
    hdr->checksum = icmp_checksum(buf, total);

    return ipv4_send(dst_ip, IP_PROTO_ICMP, buf, total);
}

void ping_start(net_u32 ip)
{
    g_ping.active     = NET_TRUE;
    g_ping.target_ip  = ip;
    g_ping.id         = 0x5449; /* 'TI' */
    g_ping.seq        = 0;
    g_ping.replies    = 0;
    g_ping.timeouts   = 0;
    g_ping.waiting    = NET_FALSE;
    g_ping.sent_tick  = 0;
}

void ping_stop(void)
{
    g_ping.active  = NET_FALSE;
    g_ping.waiting = NET_FALSE;
}

void ping_tick(net_u32 current_ticks)
{
    if (!g_ping.active) return;

    /* Timeout after ~2 seconds (assuming 100 Hz timer) */
    if (g_ping.waiting) {
        if (current_ticks - g_ping.sent_tick > 200) {
            g_ping.timeouts++;
            g_ping.waiting = NET_FALSE;
        }
        return;
    }

    /* Send next probe (max 4 packets) */
    if (g_ping.seq >= 4) {
        g_ping.active = NET_FALSE;
        return;
    }

    g_ping.seq++;
    static const net_u8 payload[32] = {
        'T','i','t','a','n','P','i','n','g','!',
        0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21
    };

    if (icmp_send_echo(g_ping.target_ip, g_ping.id, g_ping.seq, payload, 32)) {
        g_ping.waiting   = NET_TRUE;
        g_ping.sent_tick = current_ticks;
    } else {
        /* ARP not ready yet — stay on same seq, retry next tick */
        g_ping.seq--;
    }
}
