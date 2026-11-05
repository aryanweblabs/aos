/* SPDX-License-Identifier: MIT */
/* ============================================================================
 *  network/icmp.h  -- ICMP Echo (ping)
 * ==========================================================================*/
#ifndef TITAN_ICMP_H
#define TITAN_ICMP_H

#include "net_types.h"

#define ICMP_TYPE_ECHO_REPLY    0
#define ICMP_TYPE_ECHO_REQUEST  8

#define ICMP_HEADER_LEN         8
#define ICMP_PAYLOAD_MAX        56   /* classic ping size */

typedef struct {
    net_u8  type;
    net_u8  code;
    net_u16 checksum;
    net_u16 id;
    net_u16 seq;
} __attribute__((packed)) icmp_header_t;

void icmp_init(void);

/* Called from ipv4_receive when protocol == ICMP */
void icmp_receive(const net_u8 *packet, net_u32 len, net_u32 src_ip);

/* Send ICMP Echo Request. Returns NET_TRUE if packet was sent. */
net_bool icmp_send_echo(net_u32 dst_ip, net_u16 id, net_u16 seq,
                        const void *payload, net_u32 payload_len);

/* High-level ping state (used by shell command) */
typedef struct {
    net_bool active;
    net_u32  target_ip;
    net_u16  id;
    net_u16  seq;
    net_u32  sent_tick;
    net_u32  replies;
    net_u32  timeouts;
    net_bool waiting;
} ping_state_t;

extern ping_state_t g_ping;

void ping_start(net_u32 ip);
void ping_stop(void);
void ping_tick(net_u32 current_ticks);   /* call periodically from main loop */

#endif /* TITAN_ICMP_H */
