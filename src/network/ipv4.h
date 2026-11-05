/* SPDX-License-Identifier: MIT */
/* ============================================================================
 *  network/ipv4.h
 * ==========================================================================*/
#ifndef TITAN_IPV4_H
#define TITAN_IPV4_H

#include "net_types.h"
#include "ethernet.h"

#define IPV4_VERSION     4
#define IPV4_IHL_MIN     5          /* 20 bytes */
#define IPV4_HEADER_LEN  20

#define IP_PROTO_ICMP    1
#define IP_PROTO_TCP     6
#define IP_PROTO_UDP     17

typedef struct {
    net_u8  ver_ihl;        /* version (4) | IHL */
    net_u8  tos;
    net_u16 total_len;
    net_u16 id;
    net_u16 frag_off;
    net_u8  ttl;
    net_u8  protocol;
    net_u16 checksum;
    net_u32 src_ip;
    net_u32 dst_ip;
} __attribute__((packed)) ipv4_header_t;

void ipv4_init(void);

/* Called from eth_receive when ethertype == IPv4 */
void ipv4_receive(const net_u8 *packet, net_u32 len);

/* Send an IPv4 packet (payload after IP header).
 * dst_ip in host order. protocol = IP_PROTO_*.
 * Returns NET_TRUE if queued/sent. */
net_bool ipv4_send(net_u32 dst_ip, net_u8 protocol,
                   const void *payload, net_u32 payload_len);

/* Internet checksum (RFC 1071) */
net_u16 ipv4_checksum(const void *data, net_u32 len);

#endif /* TITAN_IPV4_H */
