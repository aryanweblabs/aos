/* SPDX-License-Identifier: MIT */
#include "../arch/x86_64/sync.h"
#include <stdint.h>
/* ============================================================================
 *  network/e1000.c  -- Minimal e1000 for QEMU + VirtualBox Intel PRO/1000
 * ==========================================================================*/
#include "e1000.h"
#include "net.h"
#include "pci.h"

#define REG_CTRL         0x0000
#define REG_STATUS       0x0008
#define REG_EECD         0x0010
#define REG_EERD         0x0014
#define REG_ICR          0x00C0
#define REG_IMS          0x00D0
#define REG_IMC          0x00D8
#define REG_RCTL         0x0100
#define REG_TCTL         0x0400
#define REG_TIPG         0x0410
#define REG_RDBAL        0x2800
#define REG_RDBAH        0x2804
#define REG_RDLEN        0x2808
#define REG_RDH          0x2810
#define REG_RDT          0x2818
#define REG_TDBAL        0x3800
#define REG_TDBAH        0x3804
#define REG_TDLEN        0x3808
#define REG_TDH          0x3810
#define REG_TDT          0x3818
#define REG_MTA          0x5200
#define REG_RAL          0x5400
#define REG_RAH          0x5404

#define CTRL_RST         (1u << 26)
#define CTRL_ASDE        (1u << 5)
#define CTRL_SLU         (1u << 6)

#define RCTL_EN          (1u << 1)
#define RCTL_UPE         (1u << 3)
#define RCTL_MPE         (1u << 4)
#define RCTL_BAM         (1u << 15)
#define RCTL_BSIZE_2048  (0u << 16)
#define RCTL_SECRC       (1u << 26)

#define TCTL_EN          (1u << 1)
#define TCTL_PSP         (1u << 3)

#define TXD_CMD_EOP      (1u << 0)
#define TXD_CMD_IFCS     (1u << 1)
#define TXD_CMD_RS       (1u << 3)
#define TXD_STAT_DD      (1u << 0)
#define RXD_STAT_DD      (1u << 0)
#define RXD_STAT_EOP     (1u << 1)

#define NUM_RX_DESC      32
#define NUM_TX_DESC      8
#define RX_BUF_SIZE      2048

typedef struct {
    net_u64 addr;
    net_u16 length;
    net_u8  cso;
    net_u8  cmd;
    net_u8  status;
    net_u8  css;
    net_u16 special;
} __attribute__((packed)) e1000_tx_desc_t;

typedef struct {
    net_u64 addr;
    net_u16 length;
    net_u16 checksum;
    net_u8  status;
    net_u8  errors;
    net_u16 special;
} __attribute__((packed)) e1000_rx_desc_t;

static e1000_rx_desc_t rx_descs[NUM_RX_DESC] __attribute__((aligned(16)));
static e1000_tx_desc_t tx_descs[NUM_TX_DESC] __attribute__((aligned(16)));
static net_u8          rx_bufs[NUM_RX_DESC][RX_BUF_SIZE] __attribute__((aligned(16)));
static net_u8          tx_bufs[NUM_TX_DESC][ETH_MAX_FRAME] __attribute__((aligned(16)));

static volatile net_u32 *mmio = NET_NULL;
static net_u8  mac_addr[ETH_ADDR_LEN];
static net_u32 rx_cur = 0;
static net_u32 tx_cur = 0;
static net_bool ready = NET_FALSE;
static net_u16  found_dev_id = 0;

/* Local PCI I/O — keep self-contained like the working version */
static inline void outl(net_u16 port, net_u32 val) {
    __asm__ volatile ("outl %0, %1" :: "a"(val), "Nd"(port));
}
static inline net_u32 inl(net_u16 port) {
    net_u32 r;
    __asm__ volatile ("inl %1, %0" : "=a"(r) : "Nd"(port));
    return r;
}

static net_u32 pci_read32(net_u8 bus, net_u8 slot, net_u8 func, net_u8 off)
{
    net_u32 addr = (1u << 31) | ((net_u32)bus << 16) | ((net_u32)slot << 11) |
                   ((net_u32)func << 8) | (off & 0xFC);
    outl(0xCF8, addr);
    return inl(0xCFC);
}
static void pci_write32(net_u8 bus, net_u8 slot, net_u8 func, net_u8 off, net_u32 val)
{
    net_u32 addr = (1u << 31) | ((net_u32)bus << 16) | ((net_u32)slot << 11) |
                   ((net_u32)func << 8) | (off & 0xFC);
    outl(0xCF8, addr);
    outl(0xCFC, val);
}

static inline net_u32 er32(net_u32 reg) { return mmio[reg / 4]; }
static inline void ew32(net_u32 reg, net_u32 val) { mmio[reg / 4] = val; }

/* Device IDs: VirtualBox + QEMU */
static const net_u16 e1000_ids[] = {
    0x100E, 0x100F, 0x1004, 0x1008, 0x100C, 0x1015, 0x1016,
    0x1076, 0x107C, 0x109A, 0x10D3, 0x10F5, 0
};

static net_bool find_nic(net_u8 *bus, net_u8 *slot, net_u16 *dev_out)
{
    for (int b = 0; b < 256; b++) {
        for (int s = 0; s < 32; s++) {
            net_u32 id = pci_read32((net_u8)b, (net_u8)s, 0, 0x00);
            net_u16 vendor = (net_u16)(id & 0xFFFF);
            net_u16 device = (net_u16)(id >> 16);
            if (vendor != 0x8086) continue;
            for (int i = 0; e1000_ids[i]; i++) {
                if (device == e1000_ids[i]) {
                    *bus = (net_u8)b;
                    *slot = (net_u8)s;
                    *dev_out = device;
                    return NET_TRUE;
                }
            }
            /* any Intel network class 02:00 */
            net_u32 cr = pci_read32((net_u8)b, (net_u8)s, 0, 0x08);
            if (((cr >> 24) & 0xFF) == 0x02 && ((cr >> 16) & 0xFF) == 0x00) {
                *bus = (net_u8)b;
                *slot = (net_u8)s;
                *dev_out = device;
                return NET_TRUE;
            }
        }
    }
    return NET_FALSE;
}

static net_u16 eeprom_read(net_u8 addr)
{
    ew32(REG_EERD, ((net_u32)addr << 8) | 1);
    for (int i = 0; i < 100000; i++) {
        net_u32 val = er32(REG_EERD);
        if (val & (1u << 4))
            return (net_u16)(val >> 16);
    }
    return 0;
}

static void read_mac(void)
{
    net_u16 w0 = eeprom_read(0);
    net_u16 w1 = eeprom_read(1);
    net_u16 w2 = eeprom_read(2);
    if (w0 | w1 | w2) {
        mac_addr[0] = (net_u8)(w0 & 0xFF);
        mac_addr[1] = (net_u8)(w0 >> 8);
        mac_addr[2] = (net_u8)(w1 & 0xFF);
        mac_addr[3] = (net_u8)(w1 >> 8);
        mac_addr[4] = (net_u8)(w2 & 0xFF);
        mac_addr[5] = (net_u8)(w2 >> 8);
        return;
    }
    net_u32 ral = er32(REG_RAL);
    net_u32 rah = er32(REG_RAH);
    mac_addr[0] = (net_u8)(ral);
    mac_addr[1] = (net_u8)(ral >> 8);
    mac_addr[2] = (net_u8)(ral >> 16);
    mac_addr[3] = (net_u8)(ral >> 24);
    mac_addr[4] = (net_u8)(rah);
    mac_addr[5] = (net_u8)(rah >> 8);
    if (!(mac_addr[0]|mac_addr[1]|mac_addr[2]|mac_addr[3]|mac_addr[4]|mac_addr[5])) {
        mac_addr[0]=0x52; mac_addr[1]=0x54; mac_addr[2]=0x00;
        mac_addr[3]=0x12; mac_addr[4]=0x34; mac_addr[5]=0x56;
    }
}

net_bool e1000_init(void)
{
    net_u8 bus = 0, slot = 0;
    ready = NET_FALSE;
    mmio = NET_NULL;
    found_dev_id = 0;

    if (!find_nic(&bus, &slot, &found_dev_id))
        return NET_FALSE;

    /* Enable bus master + memory space (exactly like working version) */
    net_u32 cmd = pci_read32(bus, slot, 0, 0x04);
    cmd |= 0x06;
    pci_write32(bus, slot, 0, 0x04, cmd);

    net_u32 bar0 = pci_read32(bus, slot, 0, 0x10);
    if (bar0 & 1)
        return NET_FALSE; /* IO BAR */
    net_u32 base = bar0 & ~0xFu;
    if (!base)
        return NET_FALSE;

    extern _Bool aos_vmm_map_mmio_uc(unsigned long long, unsigned long long);
    aos_vmm_map_mmio_uc(base, 0x20000);      /* 128 KiB register BAR: must not be cached */
    mmio = (volatile net_u32 *)(uintptr_t)base;

    ew32(REG_IMC, 0xFFFFFFFF);
    ew32(REG_CTRL, er32(REG_CTRL) | CTRL_RST);
    for (volatile int i = 0; i < 150000; i++) {}
    ew32(REG_IMC, 0xFFFFFFFF);

    ew32(REG_CTRL, er32(REG_CTRL) | CTRL_SLU | CTRL_ASDE);

    read_mac();

    for (int i = 0; i < 128; i++)
        ew32(REG_MTA + i * 4, 0);

    net_u32 ral = (net_u32)mac_addr[0] | ((net_u32)mac_addr[1]<<8) |
                  ((net_u32)mac_addr[2]<<16) | ((net_u32)mac_addr[3]<<24);
    net_u32 rah = (net_u32)mac_addr[4] | ((net_u32)mac_addr[5]<<8) | (1u<<31);
    ew32(REG_RAL, ral);
    ew32(REG_RAH, rah);

    for (int i = 0; i < NUM_RX_DESC; i++) {
        rx_descs[i].addr = (net_u64)aos_virt_to_phys(&rx_bufs[i][0]);
        rx_descs[i].status = 0;
    }
    ew32(REG_RDBAL, (net_u32)aos_virt_to_phys(&rx_descs[0]));
    ew32(REG_RDBAH, 0);
    ew32(REG_RDLEN, NUM_RX_DESC * (net_u32)sizeof(e1000_rx_desc_t));
    ew32(REG_RDH, 0);
    ew32(REG_RDT, NUM_RX_DESC - 1);
    rx_cur = 0;

    for (int i = 0; i < NUM_TX_DESC; i++) {
        tx_descs[i].addr = 0;
        tx_descs[i].status = TXD_STAT_DD;
    }
    ew32(REG_TDBAL, (net_u32)aos_virt_to_phys(&tx_descs[0]));
    ew32(REG_TDBAH, 0);
    ew32(REG_TDLEN, NUM_TX_DESC * (net_u32)sizeof(e1000_tx_desc_t));
    ew32(REG_TDH, 0);
    ew32(REG_TDT, 0);
    tx_cur = 0;

    ew32(REG_TCTL, TCTL_EN | TCTL_PSP | (0x10u << 4) | (0x40u << 12));
    ew32(REG_TIPG, 0x0060200A);
    ew32(REG_RCTL, RCTL_EN | RCTL_BAM | RCTL_SECRC | RCTL_BSIZE_2048 | RCTL_UPE | RCTL_MPE);

    ready = NET_TRUE;
    return NET_TRUE;
}

void e1000_get_mac(net_u8 *mac_out)
{
    for (int i = 0; i < 6; i++) mac_out[i] = mac_addr[i];
}

net_u16 e1000_device_id(void) { return found_dev_id; }

net_bool e1000_transmit(const net_u8 *data, net_u32 len)
{
    if (!ready || !len || len > ETH_MAX_FRAME) return NET_FALSE;
    e1000_tx_desc_t *d = &tx_descs[tx_cur];
    if (!(d->status & TXD_STAT_DD)) return NET_FALSE;
    for (net_u32 i = 0; i < len; i++) tx_bufs[tx_cur][i] = data[i];
    d->addr = (net_u64)aos_virt_to_phys(&tx_bufs[tx_cur][0]);
    d->length = (net_u16)len;
    d->cmd = TXD_CMD_EOP | TXD_CMD_IFCS | TXD_CMD_RS;
    d->status = 0;
    tx_cur = (tx_cur + 1) % NUM_TX_DESC;
    ew32(REG_TDT, tx_cur);
    return NET_TRUE;
}

void e1000_poll(void)
{
    if (!ready) return;
    for (int n = 0; n < NUM_RX_DESC; n++) {
        e1000_rx_desc_t *d = &rx_descs[rx_cur];
        if (!(d->status & RXD_STAT_DD)) break;
        if (d->length && (d->status & RXD_STAT_EOP))
            net_on_frame(rx_bufs[rx_cur], d->length);
        d->status = 0;
        ew32(REG_RDT, rx_cur);
        rx_cur = (rx_cur + 1) % NUM_RX_DESC;
    }
}
