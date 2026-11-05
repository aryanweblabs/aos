/* SPDX-License-Identifier: MIT */
#ifndef AOS_NET_H
#define AOS_NET_H

#include "net_types.h"
#include "ethernet.h"

#define NET_DEFAULT_IP       net_ip(10, 0, 2, 15)
#define NET_DEFAULT_NETMASK  net_ip(255, 255, 255, 0)
#define NET_DEFAULT_GATEWAY  net_ip(10, 0, 2, 2)
#define NET_DEFAULT_DNS      net_ip(10, 0, 2, 3)

typedef struct {
    net_u8   mac[ETH_ADDR_LEN];
    net_u32  ip;
    net_u32  netmask;
    net_u32  gateway;
    net_u32  dns;
    net_bool link_up;
    net_bool initialized;
} net_if_t;

#ifdef __cplusplus
extern "C" {
#endif

extern net_if_t g_netif;

net_bool net_init(void);
void     net_poll(void);
void     net_on_frame(const net_u8 *frame, net_u32 len);
net_bool net_transmit(const net_u8 *frame, net_u32 len);
void     net_tick(net_u32 ticks);

void net_cmd_ifconfig(void (*printfn)(const char *fmt, ...));
void net_cmd_ping(const char *arg, void (*printfn)(const char *fmt, ...));
void net_cmd_arp(void (*printfn)(const char *fmt, ...));
void net_cmd_lspci(void (*printfn)(const char *fmt, ...));
void net_cmd_dhcp(void (*printfn)(const char *fmt, ...));
void net_cmd_nslookup(const char *arg, void (*printfn)(const char *fmt, ...));
void net_cmd_httpget(const char *arg, void (*printfn)(const char *fmt, ...));

/* provided by serial_printf.c */
void aos_net_printf(const char *fmt, ...);

#ifdef __cplusplus
}
#endif

#endif
