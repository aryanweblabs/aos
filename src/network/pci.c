/* SPDX-License-Identifier: MIT */
/* ============================================================================
 *  network/pci.c
 * ==========================================================================*/
#include "pci.h"

#define PCI_CONFIG_ADDR  0xCF8
#define PCI_CONFIG_DATA  0xCFC

static inline void outl(net_u16 port, net_u32 val) {
    __asm__ volatile ("outl %0, %1" :: "a"(val), "Nd"(port));
}
static inline net_u32 inl(net_u16 port) {
    net_u32 r;
    __asm__ volatile ("inl %1, %0" : "=a"(r) : "Nd"(port));
    return r;
}

net_u32 pci_config_read(net_u8 bus, net_u8 slot, net_u8 func, net_u8 offset)
{
    net_u32 addr = (1u << 31) | ((net_u32)bus << 16) | ((net_u32)slot << 11) |
                   ((net_u32)func << 8) | (offset & 0xFC);
    outl(PCI_CONFIG_ADDR, addr);
    return inl(PCI_CONFIG_DATA);
}

void pci_config_write(net_u8 bus, net_u8 slot, net_u8 func, net_u8 offset, net_u32 val)
{
    net_u32 addr = (1u << 31) | ((net_u32)bus << 16) | ((net_u32)slot << 11) |
                   ((net_u32)func << 8) | (offset & 0xFC);
    outl(PCI_CONFIG_ADDR, addr);
    outl(PCI_CONFIG_DATA, val);
}

/* Titan printf only supports %s %d %u %x %c — no width flags.
 * Manual hex into buf (up to 8 hex digits + NUL). */
static void hex4(net_u16 v, char *buf)
{
    static const char h[] = "0123456789abcdef";
    buf[0] = h[(v >> 12) & 0xF];
    buf[1] = h[(v >> 8) & 0xF];
    buf[2] = h[(v >> 4) & 0xF];
    buf[3] = h[v & 0xF];
    buf[4] = 0;
}
static void hex2(net_u8 v, char *buf)
{
    static const char h[] = "0123456789abcdef";
    buf[0] = h[(v >> 4) & 0xF];
    buf[1] = h[v & 0xF];
    buf[2] = 0;
}
static void hex8(net_u32 v, char *buf)
{
    static const char h[] = "0123456789abcdef";
    for (int i = 0; i < 8; i++)
        buf[i] = h[(v >> (28 - i * 4)) & 0xF];
    buf[8] = 0;
}

net_bool pci_find_device(net_u16 vendor, net_u16 device,
                         net_u8 *bus_out, net_u8 *slot_out, net_u8 *func_out)
{
    for (int bus = 0; bus < 256; bus++) {
        for (int slot = 0; slot < 32; slot++) {
            net_u32 id = pci_config_read((net_u8)bus, (net_u8)slot, 0, 0x00);
            net_u16 v = (net_u16)(id & 0xFFFF);
            if (v == 0xFFFF) continue;
            net_u16 d = (net_u16)(id >> 16);
            if (v == vendor && d == device) {
                *bus_out = (net_u8)bus;
                *slot_out = (net_u8)slot;
                *func_out = 0;
                return NET_TRUE;
            }
        }
    }
    return NET_FALSE;
}

static const net_u16 e1000_ids[] = {
    0x100E, 0x100F, 0x1004, 0x1008, 0x100C, 0x1015, 0x1016,
    0x1076, 0x107C, 0x109A, 0x10D3, 0x10F5, 0
};

net_bool pci_find_e1000_family(net_u8 *bus, net_u8 *slot, net_u8 *func, net_u16 *dev_id_out)
{
    for (int i = 0; e1000_ids[i]; i++) {
        if (pci_find_device(0x8086, e1000_ids[i], bus, slot, func)) {
            *dev_id_out = e1000_ids[i];
            return NET_TRUE;
        }
    }
    for (int b = 0; b < 256; b++) {
        for (int s = 0; s < 32; s++) {
            net_u32 id = pci_config_read((net_u8)b, (net_u8)s, 0, 0x00);
            if ((net_u16)(id & 0xFFFF) != 0x8086) continue;
            net_u32 classreg = pci_config_read((net_u8)b, (net_u8)s, 0, 0x08);
            if (((classreg >> 24) & 0xFF) == 0x02 && ((classreg >> 16) & 0xFF) == 0x00) {
                *bus = (net_u8)b;
                *slot = (net_u8)s;
                *func = 0;
                *dev_id_out = (net_u16)(id >> 16);
                return NET_TRUE;
            }
        }
    }
    return NET_FALSE;
}

void pci_list_devices(void (*printfn)(const char *fmt, ...))
{
    int count = 0;
    int found_pcnet = 0;
    int found_intel = 0;

    printfn("PCI devices:\n");
    for (int bus = 0; bus < 256; bus++) {
        for (int slot = 0; slot < 32; slot++) {
            net_u32 id = pci_config_read((net_u8)bus, (net_u8)slot, 0, 0x00);
            net_u16 vendor = (net_u16)(id & 0xFFFF);
            if (vendor == 0xFFFF) continue;
            net_u16 device = (net_u16)(id >> 16);
            net_u32 classreg = pci_config_read((net_u8)bus, (net_u8)slot, 0, 0x08);
            net_u8 class_code = (net_u8)(classreg >> 24);
            net_u8 subclass   = (net_u8)(classreg >> 16);
            net_u32 bar0 = pci_config_read((net_u8)bus, (net_u8)slot, 0, 0x10);

            char bs[3], ss[3], vs[5], ds[5], cs[3], sc[3], b0[9];
            hex2((net_u8)bus, bs);
            hex2((net_u8)slot, ss);
            hex4(vendor, vs);
            hex4(device, ds);
            hex2(class_code, cs);
            hex2(subclass, sc);
            hex8(bar0, b0);

            printfn("  %s:%s.0  %s:%s  class %s%s  BAR0=%s",
                    bs, ss, vs, ds, cs, sc, b0);

            if (vendor == 0x8086 && class_code == 0x02) {
                printfn("  [Intel NIC - OK]");
                found_intel = 1;
            } else if (vendor == 0x1022 && (device == 0x2000 || device == 0x2001)) {
                printfn("  [AMD PCnet - supported]");
                found_pcnet = 1;
            } else if (vendor == 0x10EC) {
                printfn("  [Realtek]");
            }
            printfn("\n");
            count++;
        }
    }
    if (count == 0)
        printfn("  (none found)\n");

    printfn("\n");
    if (found_pcnet && !found_intel) {
        printfn("*** AMD PCnet detected (VirtualBox default) — driver loaded if init OK.\n");
        printfn("*** Try: ifconfig / ping 10.0.2.2\n");
        printfn("*** Fallback: Adapter Type = Intel PRO/1000 MT Desktop\n");
    } else if (found_intel) {
        printfn("Intel NIC found. If ifconfig still fails, reboot once.\n");
    } else if (found_pcnet) {
        printfn("AMD PCnet + Intel present. Driver auto-selects at boot.\n");
    } else {
        printfn("No known NIC found. Enable Network Adapter in VM settings.\n");
    }
}
