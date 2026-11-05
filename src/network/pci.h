/* SPDX-License-Identifier: MIT */
/* ============================================================================
 *  network/pci.h  -- Minimal PCI helpers + device list
 * ==========================================================================*/
#ifndef TITAN_PCI_H
#define TITAN_PCI_H

#include "net_types.h"

net_u32 pci_config_read(net_u8 bus, net_u8 slot, net_u8 func, net_u8 offset);
void    pci_config_write(net_u8 bus, net_u8 slot, net_u8 func, net_u8 offset, net_u32 val);

/* Find first matching vendor:device. Returns NET_TRUE and fills bus/slot/func. */
net_bool pci_find_device(net_u16 vendor, net_u16 device,
                         net_u8 *bus, net_u8 *slot, net_u8 *func);

/* Find any Intel e1000-family NIC. Returns device ID found. */
net_bool pci_find_e1000_family(net_u8 *bus, net_u8 *slot, net_u8 *func, net_u16 *dev_id_out);

/* Debug: list all PCI devices via printfn */
void pci_list_devices(void (*printfn)(const char *fmt, ...));

#endif
