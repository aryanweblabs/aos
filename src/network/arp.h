/* SPDX-License-Identifier: MIT */
/* ============================================================================
 *  network/arp.h
 * ==========================================================================*/
#ifndef TITAN_ARP_H
#define TITAN_ARP_H

#include "net_types.h"
#include "ethernet.h"

#define ARP_HTYPE_ETH    1
#define ARP_PTYPE_IPV4   0x0800
#define ARP_OP_REQUEST   1
#define ARP_OP_REPLY     2

#define ARP_CACHE_SIZE   16
#define ARP_CACHE_TTL    300   /* seconds (approx, using ticks later) */

typedef struct {
    net_u16 htype;
    net_u16 ptype;
    net_u8  hlen;
    net_u8  plen;
    net_u16 oper;
    net_u8  sha[ETH_ADDR_LEN];  /* sender hardware */
    net_u32 spa;                /* sender protocol (IP) - network order on wire */
    net_u8  tha[ETH_ADDR_LEN];  /* target hardware */
    net_u32 tpa;                /* target protocol */
} __attribute__((packed)) arp_packet_t;

void arp_init(void);
void arp_receive(const net_u8 *payload, net_u32 len, const net_u8 *src_mac);

/* Resolve IP → MAC. Returns NET_TRUE if found (or broadcast).
 * If not in cache, sends ARP request and returns NET_FALSE (caller should retry later). */
net_bool arp_resolve(net_u32 ip, net_u8 *out_mac);

/* Force add / update cache entry */
void arp_cache_update(net_u32 ip, const net_u8 *mac);

/* Debug: print cache */
void arp_dump_cache(void (*print_fn)(const char *fmt, ...));

#endif /* TITAN_ARP_H */
