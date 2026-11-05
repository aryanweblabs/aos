/* SPDX-License-Identifier: MIT */
/* ============================================================================
 *  network/ethernet.c
 * ==========================================================================*/
#include "ethernet.h"
#include "net.h"
#include "arp.h"
#include "ipv4.h"

void eth_addr_copy(net_u8 *dst, const net_u8 *src)
{
    for (int i = 0; i < ETH_ADDR_LEN; i++) dst[i] = src[i];
}

net_bool eth_addr_equal(const net_u8 *a, const net_u8 *b)
{
    for (int i = 0; i < ETH_ADDR_LEN; i++)
        if (a[i] != b[i]) return NET_FALSE;
    return NET_TRUE;
}

net_bool eth_addr_is_broadcast(const net_u8 *a)
{
    for (int i = 0; i < ETH_ADDR_LEN; i++)
        if (a[i] != 0xFF) return NET_FALSE;
    return NET_TRUE;
}

void eth_addr_set_broadcast(net_u8 *a)
{
    for (int i = 0; i < ETH_ADDR_LEN; i++) a[i] = 0xFF;
}

void eth_addr_to_str(const net_u8 *mac, char *buf)
{
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < 6; i++) {
        buf[i*3]   = hex[(mac[i] >> 4) & 0xF];
        buf[i*3+1] = hex[mac[i] & 0xF];
        buf[i*3+2] = (i < 5) ? ':' : 0;
    }
}

net_bool eth_send(const net_u8 *dst_mac, net_u16 ethertype,
                  const void *payload, net_u32 payload_len)
{
    net_u8 frame[ETH_MAX_FRAME];
    eth_header_t *hdr = (eth_header_t *)frame;

    if (payload_len > ETH_MTU) return NET_FALSE;

    eth_addr_copy(hdr->dst, dst_mac);
    eth_addr_copy(hdr->src, g_netif.mac);
    hdr->type = net_htons(ethertype);

    for (net_u32 i = 0; i < payload_len; i++)
        frame[ETH_HEADER_LEN + i] = ((const net_u8 *)payload)[i];

    net_u32 total = ETH_HEADER_LEN + payload_len;
    /* Pad to minimum Ethernet frame size */
    while (total < ETH_MIN_FRAME) {
        frame[total++] = 0;
    }

    return net_transmit(frame, total);
}

void eth_receive(const net_u8 *frame, net_u32 len)
{
    if (len < ETH_HEADER_LEN) return;

    const eth_header_t *hdr = (const eth_header_t *)frame;
    net_u16 type = net_ntohs(hdr->type);
    const net_u8 *payload = frame + ETH_HEADER_LEN;
    net_u32 plen = len - ETH_HEADER_LEN;

    /* Accept frames for us or broadcast */
    if (!eth_addr_equal(hdr->dst, g_netif.mac) &&
        !eth_addr_is_broadcast(hdr->dst)) {
        return;
    }

    if (type == ETH_TYPE_ARP) {
        arp_receive(payload, plen, hdr->src);
    } else if (type == ETH_TYPE_IPV4) {
        ipv4_receive(payload, plen);
    }
}
