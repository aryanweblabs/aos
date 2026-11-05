/* SPDX-License-Identifier: MIT */
/* ============================================================================
 *  network/pcnet.h  -- AMD PCnet-PCI (Am79C970A / 1022:2000) for VirtualBox
 * ==========================================================================*/
#ifndef TITAN_PCNET_H
#define TITAN_PCNET_H

#include "net_types.h"
#include "ethernet.h"

net_bool pcnet_init(void);
void     pcnet_poll(void);
net_bool pcnet_transmit(const net_u8 *data, net_u32 len);
void     pcnet_get_mac(net_u8 *mac_out);

#endif
