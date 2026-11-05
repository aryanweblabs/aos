/* SPDX-License-Identifier: MIT */
#ifndef TITAN_DHCP_H
#define TITAN_DHCP_H
#include "net_types.h"

void dhcp_init(void);
/* blocking-ish: poll until done or timeout; print via printfn */
net_bool dhcp_run(void (*printfn)(const char *fmt, ...));
void dhcp_udp_receive(const net_u8 *payload, net_u32 len, net_u32 src_ip, net_u16 src_port);
void dhcp_poll(void);
net_bool dhcp_is_bound(void);

#endif
