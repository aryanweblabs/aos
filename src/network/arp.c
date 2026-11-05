/* SPDX-License-Identifier: MIT */
/* ============================================================================
 *  network/arp.c
 * ==========================================================================*/
#include "arp.h"
#include "net.h"
#include "ethernet.h"

typedef struct {
    net_u32  ip;
    net_u8   mac[ETH_ADDR_LEN];
    net_bool valid;
} arp_entry_t;

static arp_entry_t arp_cache[ARP_CACHE_SIZE];

void arp_init(void)
{
    for (int i = 0; i < ARP_CACHE_SIZE; i++)
        arp_cache[i].valid = NET_FALSE;
}

void arp_cache_update(net_u32 ip, const net_u8 *mac)
{
    /* Update existing */
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (arp_cache[i].valid && arp_cache[i].ip == ip) {
            eth_addr_copy(arp_cache[i].mac, mac);
            return;
        }
    }
    /* Find free slot */
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (!arp_cache[i].valid) {
            arp_cache[i].ip = ip;
            eth_addr_copy(arp_cache[i].mac, mac);
            arp_cache[i].valid = NET_TRUE;
            return;
        }
    }
    /* Overwrite first entry if full */
    arp_cache[0].ip = ip;
    eth_addr_copy(arp_cache[0].mac, mac);
    arp_cache[0].valid = NET_TRUE;
}

static net_bool arp_cache_lookup(net_u32 ip, net_u8 *out_mac)
{
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (arp_cache[i].valid && arp_cache[i].ip == ip) {
            eth_addr_copy(out_mac, arp_cache[i].mac);
            return NET_TRUE;
        }
    }
    return NET_FALSE;
}

static void arp_send_request(net_u32 target_ip)
{
    arp_packet_t pkt;
    net_u8 bcast[ETH_ADDR_LEN];

    pkt.htype = net_htons(ARP_HTYPE_ETH);
    pkt.ptype = net_htons(ARP_PTYPE_IPV4);
    pkt.hlen  = ETH_ADDR_LEN;
    pkt.plen  = 4;
    pkt.oper  = net_htons(ARP_OP_REQUEST);
    eth_addr_copy(pkt.sha, g_netif.mac);
    pkt.spa   = g_netif.ip;  /* already wire-order on LE */
    for (int i = 0; i < ETH_ADDR_LEN; i++) pkt.tha[i] = 0;
    pkt.tpa   = target_ip;

    eth_addr_set_broadcast(bcast);
    eth_send(bcast, ETH_TYPE_ARP, &pkt, sizeof(pkt));
}

static void arp_send_reply(net_u32 target_ip, const net_u8 *target_mac)
{
    arp_packet_t pkt;

    pkt.htype = net_htons(ARP_HTYPE_ETH);
    pkt.ptype = net_htons(ARP_PTYPE_IPV4);
    pkt.hlen  = ETH_ADDR_LEN;
    pkt.plen  = 4;
    pkt.oper  = net_htons(ARP_OP_REPLY);
    eth_addr_copy(pkt.sha, g_netif.mac);
    pkt.spa   = g_netif.ip;  /* already wire-order on LE */
    eth_addr_copy(pkt.tha, target_mac);
    pkt.tpa   = target_ip;

    eth_send(target_mac, ETH_TYPE_ARP, &pkt, sizeof(pkt));
}

void arp_receive(const net_u8 *payload, net_u32 len, const net_u8 *src_mac)
{
    (void)src_mac;
    if (len < sizeof(arp_packet_t)) return;

    const arp_packet_t *pkt = (const arp_packet_t *)payload;
    if (net_ntohs(pkt->htype) != ARP_HTYPE_ETH) return;
    if (net_ntohs(pkt->ptype) != ARP_PTYPE_IPV4) return;
    if (pkt->hlen != ETH_ADDR_LEN || pkt->plen != 4) return;

    net_u32 spa = pkt->spa;  /* internal format == wire on LE */
    net_u32 tpa = pkt->tpa;
    net_u16 oper = net_ntohs(pkt->oper);

    /* Learn the sender */
    arp_cache_update(spa, pkt->sha);

    if (oper == ARP_OP_REQUEST) {
        if (tpa == g_netif.ip) {
            arp_send_reply(spa, pkt->sha);
        }
    }
    /* Reply is already handled by cache update */
}

net_bool arp_resolve(net_u32 ip, net_u8 *out_mac)
{
    /* Broadcast? */
    if (ip == 0xFFFFFFFFu) {
        eth_addr_set_broadcast(out_mac);
        return NET_TRUE;
    }
    /* Same subnet broadcast */
    if ((ip & g_netif.netmask) == (g_netif.ip & g_netif.netmask) &&
        (ip & ~g_netif.netmask) == ~g_netif.netmask) {
        eth_addr_set_broadcast(out_mac);
        return NET_TRUE;
    }

    if (arp_cache_lookup(ip, out_mac))
        return NET_TRUE;

    /* Not in cache → send request */
    arp_send_request(ip);
    return NET_FALSE;
}

void arp_dump_cache(void (*print_fn)(const char *fmt, ...))
{
    char ipbuf[16], macbuf[18];
    print_fn("ARP cache:\n");
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (!arp_cache[i].valid) continue;
        net_ip_to_str(arp_cache[i].ip, ipbuf);
        eth_addr_to_str(arp_cache[i].mac, macbuf);
        print_fn("  %s  %s\n", ipbuf, macbuf);
    }
}
