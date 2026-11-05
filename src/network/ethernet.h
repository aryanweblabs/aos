/* SPDX-License-Identifier: MIT */
/* ============================================================================
 *  network/ethernet.h
 * ==========================================================================*/
#ifndef TITAN_ETHERNET_H
#define TITAN_ETHERNET_H

#include "net_types.h"

#define ETH_ADDR_LEN     6
#define ETH_HEADER_LEN   14
#define ETH_MIN_FRAME    60
#define ETH_MAX_FRAME    1514
#define ETH_MTU          1500

#define ETH_TYPE_IPV4    0x0800
#define ETH_TYPE_ARP     0x0806

typedef struct {
    net_u8  dst[ETH_ADDR_LEN];
    net_u8  src[ETH_ADDR_LEN];
    net_u16 type;               /* big-endian on wire */
} __attribute__((packed)) eth_header_t;

void eth_addr_copy(net_u8 *dst, const net_u8 *src);
net_bool eth_addr_equal(const net_u8 *a, const net_u8 *b);
net_bool eth_addr_is_broadcast(const net_u8 *a);
void eth_addr_set_broadcast(net_u8 *a);
void eth_addr_to_str(const net_u8 *mac, char *buf); /* buf >= 18 */

/* Build + send an Ethernet frame (payload already filled after header) */
net_bool eth_send(const net_u8 *dst_mac, net_u16 ethertype,
                  const void *payload, net_u32 payload_len);

/* Called by NIC driver when a frame arrives */
void eth_receive(const net_u8 *frame, net_u32 len);

#endif /* TITAN_ETHERNET_H */
