/* SPDX-License-Identifier: MIT */
#pragma once
#include <stdint.h>
#include <stddef.h>

namespace aos::net {

using u8  = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;
using u64 = uint64_t;
using bool_t = bool;

constexpr bool TRUE  = true;
constexpr bool FALSE = false;

inline u16 htons(u16 x) { return static_cast<u16>((x << 8) | (x >> 8)); }
inline u16 ntohs(u16 x) { return htons(x); }
inline u32 htonl(u32 x) {
    return ((x & 0x000000FFu) << 24) | ((x & 0x0000FF00u) << 8) |
           ((x & 0x00FF0000u) >> 8)  | ((x & 0xFF000000u) >> 24);
}
inline u32 ntohl(u32 x) { return htonl(x); }

inline u32 ip(u8 a, u8 b, u8 c, u8 d) {
    return static_cast<u32>(a) | (static_cast<u32>(b) << 8) |
           (static_cast<u32>(c) << 16) | (static_cast<u32>(d) << 24);
}

void ip_to_str(u32 ip, char* buf);
u32  str_to_ip(const char* s);

} // namespace aos::net
