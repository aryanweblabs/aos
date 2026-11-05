/* SPDX-License-Identifier: MIT */
/* ============================================================================
 *  network/net_types.c
 * ==========================================================================*/
#include "net_types.h"

void net_ip_to_str(net_u32 ip, char *buf)
{
    net_u8 a = (net_u8)(ip & 0xFF);
    net_u8 b = (net_u8)((ip >> 8) & 0xFF);
    net_u8 c = (net_u8)((ip >> 16) & 0xFF);
    net_u8 d = (net_u8)((ip >> 24) & 0xFF);
    /* simple itoa without libc */
    char *p = buf;
    net_u8 vals[4] = { a, b, c, d };
    for (int i = 0; i < 4; i++) {
        net_u8 v = vals[i];
        if (v >= 100) { *p++ = '0' + (v / 100); v %= 100; *p++ = '0' + (v / 10); *p++ = '0' + (v % 10); }
        else if (v >= 10) { *p++ = '0' + (v / 10); *p++ = '0' + (v % 10); }
        else { *p++ = '0' + v; }
        if (i < 3) *p++ = '.';
    }
    *p = 0;
}

net_u32 net_str_to_ip(const char *s)
{
    net_u32 parts[4] = {0,0,0,0};
    int idx = 0;
    net_u32 cur = 0;
    int digits = 0;
    if (!s) return 0;
    while (*s && idx < 4) {
        if (*s >= '0' && *s <= '9') {
            cur = cur * 10 + (net_u32)(*s - '0');
            if (cur > 255) return 0;
            digits++;
        } else if (*s == '.') {
            if (digits == 0) return 0;
            parts[idx++] = cur;
            cur = 0;
            digits = 0;
        } else {
            return 0;
        }
        s++;
    }
    if (digits == 0 || idx != 3) return 0;
    parts[3] = cur;
    return (parts[0]) | (parts[1] << 8) | (parts[2] << 16) | (parts[3] << 24);
}
