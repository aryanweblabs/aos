/* SPDX-License-Identifier: MIT */
/* ============================================================================
 *  network/ipv4.c
 * ==========================================================================*/
#include "ipv4.h"
#include "net.h"
#include "ethernet.h"
#include "arp.h"
#include "icmp.h"
#include "udp.h"
#include "tcp.h"

static net_u16 ip_id_counter = 1;

void ipv4_init(void)
{
    ip_id_counter = 1;
}

net_u16 ipv4_checksum(const void *data, net_u32 len)
{
    const net_u16 *p = (const net_u16 *)data;
    net_u32 sum = 0;
    while (len > 1) {
        sum += *p++;
        len -= 2;
    }
    if (len) sum += *(const net_u8 *)p;
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (net_u16)~sum;
}

void ipv4_receive(const net_u8 *packet, net_u32 len)
{
    if (len < IPV4_HEADER_LEN) return;

    const ipv4_header_t *hdr = (const ipv4_header_t *)packet;
    net_u8 version = hdr->ver_ihl >> 4;
    net_u8 ihl = hdr->ver_ihl & 0x0F;
    if (version != 4 || ihl < 5) return;

    net_u32 header_len = (net_u32)ihl * 4;
    if (len < header_len) return;

    net_u16 total_len = net_ntohs(hdr->total_len);
    if (total_len < header_len || total_len > len) return;

    /* Checksum */
    if (ipv4_checksum(hdr, header_len) != 0) return;

    net_u32 dst = hdr->dst_ip;
    net_u32 src = hdr->src_ip;

    /* Accept packets for us, broadcast, or any when unconfigured (DHCP) */
    if (g_netif.ip != 0 && dst != g_netif.ip && dst != 0xFFFFFFFFu &&
        dst != net_ip(255,255,255,255)) {
        if ((dst & g_netif.netmask) != (g_netif.ip & g_netif.netmask) ||
            (dst & ~g_netif.netmask) != ~g_netif.netmask)
            return;
    }

    const net_u8 *payload = packet + header_len;
    net_u32 plen = total_len - header_len;

    if (hdr->protocol == IP_PROTO_ICMP) {
        icmp_receive(payload, plen, src);
    } else if (hdr->protocol == IP_PROTO_UDP) {
        udp_receive(payload, plen, src, dst);
    } else if (hdr->protocol == IP_PROTO_TCP) {
        tcp_receive(payload, plen, src, dst);
    }
}

net_bool ipv4_send(net_u32 dst_ip, net_u8 protocol,
                   const void *payload, net_u32 payload_len)
{
    net_u8 buf[ETH_MTU];
    ipv4_header_t *hdr = (ipv4_header_t *)buf;
    net_u32 total = IPV4_HEADER_LEN + payload_len;
    if (total > ETH_MTU) return NET_FALSE;

    hdr->ver_ihl    = (IPV4_VERSION << 4) | IPV4_IHL_MIN;
    hdr->tos        = 0;
    hdr->total_len  = net_htons((net_u16)total);
    hdr->id         = net_htons(ip_id_counter++);
    hdr->frag_off   = 0;
    hdr->ttl        = 64;
    hdr->protocol   = protocol;
    hdr->checksum   = 0;
    hdr->src_ip     = g_netif.ip;
    hdr->dst_ip     = dst_ip;

    for (net_u32 i = 0; i < payload_len; i++)
        buf[IPV4_HEADER_LEN + i] = ((const net_u8 *)payload)[i];

    hdr->checksum = ipv4_checksum(hdr, IPV4_HEADER_LEN);

    /* Resolve next hop */
    net_u8 dst_mac[ETH_ADDR_LEN];
    if (dst_ip == net_ip(255,255,255,255) || dst_ip == 0xFFFFFFFFu) {
        eth_addr_set_broadcast(dst_mac);
    } else {
        net_u32 next_hop = dst_ip;
        if (g_netif.ip != 0 &&
            (dst_ip & g_netif.netmask) != (g_netif.ip & g_netif.netmask)) {
            next_hop = g_netif.gateway;
        }
        if (!arp_resolve(next_hop, dst_mac)) {
            return NET_FALSE;
        }
    }

    return eth_send(dst_mac, ETH_TYPE_IPV4, buf, total);
}
