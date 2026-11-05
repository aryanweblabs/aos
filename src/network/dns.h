/* SPDX-License-Identifier: MIT */
#ifndef TITAN_DNS_H
#define TITAN_DNS_H
#include "net_types.h"

void dns_init(void);
void dns_udp_receive(const net_u8 *payload, net_u32 len, net_u32 src_ip);
/* Resolve hostname to IPv4. Returns 0 on failure. Uses g_netif DNS. */
net_u32 dns_resolve(const char *hostname, void (*printfn)(const char *fmt, ...));
void dns_cmd_lookup(const char *arg, void (*printfn)(const char *fmt, ...));

#endif
