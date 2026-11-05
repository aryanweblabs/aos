/* SPDX-License-Identifier: MIT */
/* ============================================================================
 *  network/e1000.h  -- Minimal e1000 driver (QEMU + VirtualBox Intel PRO/1000)
 * ==========================================================================*/
#ifndef TITAN_E1000_H
#define TITAN_E1000_H

#include "net_types.h"
#include "ethernet.h"

net_bool e1000_init(void);
void     e1000_poll(void);
net_bool e1000_transmit(const net_u8 *data, net_u32 len);
void     e1000_get_mac(net_u8 *mac_out);
net_u16  e1000_device_id(void);

#endif
