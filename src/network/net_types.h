/* SPDX-License-Identifier: MIT */
#ifndef AOS_NET_TYPES_H
#define AOS_NET_TYPES_H

typedef unsigned char      net_u8;
typedef unsigned short     net_u16;
typedef unsigned int       net_u32;
typedef unsigned long long net_u64;
typedef int                net_bool;

#define NET_TRUE  1
#define NET_FALSE 0
#define NET_NULL  ((void*)0)

static inline net_u16 net_htons(net_u16 x) {
    return (net_u16)((x << 8) | (x >> 8));
}
static inline net_u16 net_ntohs(net_u16 x) { return net_htons(x); }
static inline net_u32 net_htonl(net_u32 x) {
    return ((x & 0x000000FFu) << 24) | ((x & 0x0000FF00u) << 8) |
           ((x & 0x00FF0000u) >> 8)  | ((x & 0xFF000000u) >> 24);
}
static inline net_u32 net_ntohl(net_u32 x) { return net_htonl(x); }
static inline net_u32 net_ip(net_u8 a, net_u8 b, net_u8 c, net_u8 d) {
    return (net_u32)a | ((net_u32)b << 8) | ((net_u32)c << 16) | ((net_u32)d << 24);
}

void net_ip_to_str(net_u32 ip, char *buf);
net_u32 net_str_to_ip(const char *s);

#endif
