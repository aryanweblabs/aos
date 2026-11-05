/* SPDX-License-Identifier: MIT */
#pragma once
// Bridge so Titan-style network can be called from AOS 64-bit kernel.
// Full stack is in the .c files (ported from Titan, works under -m64 with identity map).

#include <stdint.h>
#include <stddef.h>

namespace aos::net {

struct NetIf {
    uint8_t  mac[6];
    uint32_t ip;
    uint32_t netmask;
    uint32_t gateway;
    uint32_t dns;
    bool     link_up;
    bool     initialized;
};

bool init();          // probe PCI, init e1000 or pcnet
void poll();          // call every frame / tick
void tick(uint64_t timer_ticks);
bool transmit(const uint8_t* frame, size_t len);
const NetIf* interface();

// Commands (optional, for terminal later)
void cmd_ifconfig();
void cmd_ping(const char* target);
void cmd_arp();
void cmd_dhcp();

} // namespace aos::net
