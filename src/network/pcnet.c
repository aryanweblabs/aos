/* SPDX-License-Identifier: MIT */
#include "../arch/x86_64/sync.h"
#include <stdint.h>
/* ============================================================================
 *  network/pcnet.c  -- Minimal AMD PCnet-PCI II / FAST driver
 *
 *  Targets VirtualBox default NIC: AMD PCnet-PCI II (vendor 0x1022 device 0x2000).
 *  Uses 32-bit software style (BCR20=2) + IO-port access via BAR0.
 *
 *  Ring sizes are power-of-two. Descriptors live in static BSS (identity map).
 * ==========================================================================*/
#include "pcnet.h"
#include "net.h"
#include "pci.h"

/* ---- PCI ---- */
#define PCNET_VENDOR  0x1022
#define PCNET_DEVICE  0x2000  /* Am79C970 / VirtualBox default */
#define PCNET_DEVICE2 0x2001  /* Am79C971 occasionally */

/* IO offsets relative to BAR0 */
#define PCNET_RDP   0x10  /* Register Data Port (32-bit style) */
#define PCNET_RAP   0x14  /* Register Address Port */
#define PCNET_RST   0x18  /* Reset */
#define PCNET_BDP   0x1C  /* BCR Data Port */

/* CSR indices */
#define CSR0   0
#define CSR1   1
#define CSR2   2
#define CSR3   3
#define CSR4   4
#define CSR12  12
#define CSR13  13
#define CSR14  14
#define CSR15  15
#define CSR88  88
#define CSR89  89

/* CSR0 bits */
#define CSR0_INIT  (1u << 0)
#define CSR0_STRT  (1u << 1)
#define CSR0_STOP  (1u << 2)
#define CSR0_TDMD  (1u << 3)
#define CSR0_TXON  (1u << 4)
#define CSR0_RXON  (1u << 5)
#define CSR0_IENA  (1u << 6)
#define CSR0_INTR  (1u << 7)
#define CSR0_IDON  (1u << 8)
#define CSR0_TINT  (1u << 9)
#define CSR0_RINT  (1u << 10)
#define CSR0_MERR  (1u << 11)
#define CSR0_MISS  (1u << 12)
#define CSR0_CERR  (1u << 13)
#define CSR0_ERR   (1u << 15)

/* BCR */
#define BCR2   2
#define BCR20  20

/* Descriptor OWN bit lives in flags */
#define RMD1_OWN   (1u << 31)
#define RMD1_ERR   (1u << 30)
#define RMD1_FRAM  (1u << 29)
#define RMD1_OFLO  (1u << 28)
#define RMD1_CRC   (1u << 27)
#define RMD1_BUFF  (1u << 26)
#define RMD1_STP   (1u << 25)
#define RMD1_ENP   (1u << 24)

#define TMD1_OWN   (1u << 31)
#define TMD1_ERR   (1u << 30)
#define TMD1_ADD_FCS (1u << 29)  /* in some docs ONE bit */
#define TMD1_STP   (1u << 25)
#define TMD1_ENP   (1u << 24)

#define NUM_RX     8
#define NUM_TX     4
#define RX_BUF_SZ  1548
#define TX_BUF_SZ  1548

/* 32-bit software style receive descriptor (16 bytes) */
typedef struct {
    net_u32 addr;       /* buffer address */
    short length;     /* 2s complement of buffer length */
    short status_hi;  /* not used the same on all; we pack flags in flags field below differently */
    /* Actually standard 32-bit SW style:
       RMD0: addr
       RMD1: flags | (bcnt as negative 12-bit in low)
       Simpler layout used by most hobby OS:
    */
    net_u32 flags;      /* OWN, ENP, STP, errors, and message length in low 12 when owned by host */
    net_u32 reserved;
} __attribute__((packed)) pcnet_rx_desc_t;

/* Cleaner classic layout matching QEMU/VirtualBox expectations for 32-bit mode:
 * struct {
 *   uint32_t tbadr;  // buffer addr
 *   int16_t  length; // 2s complement of buffer size
 *   int16_t  status; // high flags
 *   uint32_t misc;
 *   uint32_t reserved;
 * };
 */
typedef struct {
    net_u32 tbadr;
    short length;   /* 2's complement of buffer length */
    net_u16 status;   /* OWN=0x8000, ENP=0x0100, STP=0x0200, ERR=0x4000, ... */
    net_u32 misc;
    net_u32 reserved;
} __attribute__((packed)) pcnet_rdte_t;

typedef struct {
    net_u32 tbadr;
    short length;
    net_u16 status;
    net_u32 misc;
    net_u32 reserved;
} __attribute__((packed)) pcnet_tdte_t;

/* Initialization block (32-bit mode) */
typedef struct {
    net_u16 mode;
    net_u8  rlen;   /* encoded log2(rx)-4 in top nibble for some; for 32-bit: */
    net_u8  tlen;
    net_u8  padr[6];
    net_u16 reserved;
    net_u64 ladr;   /* logical addr filter — we use 0 */
    net_u32 rdra;   /* rx ring phys addr */
    net_u32 tdra;   /* tx ring phys addr */
} __attribute__((packed)) pcnet_init_block_t;

/* Actually the 32-bit init block is:
 * mode:16
 * rlen:4 (log2 rx ring in high nibble of next byte style)
 * Standard from OSDev / Linux:
 *   u16 mode;
 *   u8  rlen; // bits 7-4 = log2(rx)-0 for some cards; VirtualBox accepts:
 *   u8  tlen;
 *   u8  padr[6];
 *   u16 reserved;
 *   u64 ladr;
 *   u32 rdra;
 *   u32 tdra;
 *
 * rlen/tlen: upper 4 bits = log2(ring size)
 */

static net_u16 io_base;
static net_bool active = NET_FALSE;
static net_u8  mac[6];

static pcnet_rdte_t rx_ring[NUM_RX] __attribute__((aligned(16)));
static pcnet_tdte_t tx_ring[NUM_TX] __attribute__((aligned(16)));
static net_u8       rx_buf[NUM_RX][RX_BUF_SZ] __attribute__((aligned(16)));
static net_u8       tx_buf[NUM_TX][TX_BUF_SZ] __attribute__((aligned(16)));
static pcnet_init_block_t init_block __attribute__((aligned(16)));

static net_u32 rx_cur = 0;
static net_u32 tx_cur = 0;

/* port I/O — match style used in e1000/pci */
static inline void outw_p(net_u16 port, net_u16 val) {
    __asm__ volatile ("outw %0, %1" :: "a"(val), "Nd"(port));
}
static inline net_u16 inw_p(net_u16 port) {
    net_u16 r; __asm__ volatile ("inw %1, %0" : "=a"(r) : "Nd"(port)); return r;
}
static inline void outl_p(net_u16 port, net_u32 val) {
    __asm__ volatile ("outl %0, %1" :: "a"(val), "Nd"(port));
}
static inline net_u32 inl_p(net_u16 port) {
    net_u32 r; __asm__ volatile ("inl %1, %0" : "=a"(r) : "Nd"(port)); return r;
}

static void pcnet_write_rap(net_u32 val) { outl_p(io_base + PCNET_RAP, val); }
static net_u32 pcnet_read_csr(net_u32 index) {
    pcnet_write_rap(index);
    return inl_p(io_base + PCNET_RDP);
}
static void pcnet_write_csr(net_u32 index, net_u32 val) {
    pcnet_write_rap(index);
    outl_p(io_base + PCNET_RDP, val);
}
static net_u32 pcnet_read_bcr(net_u32 index) {
    pcnet_write_rap(index);
    return inl_p(io_base + PCNET_BDP);
}
static void pcnet_write_bcr(net_u32 index, net_u32 val) {
    pcnet_write_rap(index);
    outl_p(io_base + PCNET_BDP, val);
}

static void pcnet_reset(void) {
    /* read reset port */
    (void)inl_p(io_base + PCNET_RST);
    /* also 16-bit style reset at +0x14 for some cards — skip */
    for (volatile int i = 0; i < 100000; i++) {}
}

static net_bool pcnet_find(net_u8 *bus, net_u8 *slot, net_u8 *func)
{
    if (pci_find_device(PCNET_VENDOR, PCNET_DEVICE, bus, slot, func))
        return NET_TRUE;
    if (pci_find_device(PCNET_VENDOR, PCNET_DEVICE2, bus, slot, func))
        return NET_TRUE;
    return NET_FALSE;
}

net_bool pcnet_init(void)
{
    net_u8 bus, slot, func;
    active = NET_FALSE;
    io_base = 0;

    if (!pcnet_find(&bus, &slot, &func))
        return NET_FALSE;

    /* Enable bus master + IO space */
    net_u32 cmd = pci_config_read(bus, slot, func, 0x04);
    cmd |= 0x5; /* IO + BusMaster */
    pci_config_write(bus, slot, func, 0x04, cmd);

    /* BAR0 — IO */
    net_u32 bar0 = pci_config_read(bus, slot, func, 0x10);
    if (!(bar0 & 1)) {
        /* prefer IO; if MMIO only, fail for now */
        return NET_FALSE;
    }
    io_base = (net_u16)(bar0 & ~0x3u);

    pcnet_reset();

    /* ---- 16-bit mode window (default after reset) ----
       Offsets: RDP=0x10, RAP=0x12, RST=0x14, BDP=0x16
       Read MAC from APROM at base+0..5, then switch DWIO. */
    {
        for (int i = 0; i < 6; i++) {
            net_u8 b;
            __asm__ volatile ("inb %1, %0" : "=a"(b) : "Nd"((net_u16)(io_base + i)));
            mac[i] = b;
        }

        /* BCR20 = 2 via 16-bit RAP/BDP to enter 32-bit software style */
        outw_p((net_u16)(io_base + 0x12), 20);           /* RAP = BCR20 */
        outw_p((net_u16)(io_base + 0x16), 2);             /* BDP = 2 (DWIO / 32-bit) */
    }

    /* ---- now 32-bit mode: RDP=0x10 RAP=0x14 RST=0x18 BDP=0x1C ---- */

    /* BCR2 autoselect */
    {
        net_u32 bcr2 = pcnet_read_bcr(BCR2);
        pcnet_write_bcr(BCR2, bcr2 | 0x2);
    }

    /* STOP chip before programming init block */
    pcnet_write_csr(CSR0, CSR0_STOP);
    for (volatile int i = 0; i < 10000; i++) {}

    /* If APROM MAC invalid, try CSR12-14 */
    {
        net_u8 z = 0, f = 0;
        for (int i = 0; i < 6; i++) {
            if (mac[i] == 0x00) z++;
            if (mac[i] == 0xFF) f++;
        }
        if (z == 6 || f == 6) {
            net_u32 csr12 = pcnet_read_csr(CSR12);
            net_u32 csr13 = pcnet_read_csr(CSR13);
            net_u32 csr14 = pcnet_read_csr(CSR14);
            mac[0] = (net_u8)(csr12 & 0xFF);
            mac[1] = (net_u8)((csr12 >> 8) & 0xFF);
            mac[2] = (net_u8)(csr13 & 0xFF);
            mac[3] = (net_u8)((csr13 >> 8) & 0xFF);
            mac[4] = (net_u8)(csr14 & 0xFF);
            mac[5] = (net_u8)((csr14 >> 8) & 0xFF);
        }
        z = 0; f = 0;
        for (int i = 0; i < 6; i++) {
            if (mac[i] == 0x00) z++;
            if (mac[i] == 0xFF) f++;
        }
        if (z == 6 || f == 6) {
            mac[0] = 0x02; mac[1] = 0x00; mac[2] = 0x52;
            mac[3] = (net_u8)((io_base >> 8) & 0xFF);
            mac[4] = (net_u8)(io_base & 0xFF);
            mac[5] = 0x01;
        }
        /* ensure unicast (clear multicast bit) unless we still have broadcast */
        {
            net_u8 f = 0, z = 0;
            for (int i = 0; i < 6; i++) {
                if (mac[i] == 0xFF) f++;
                if (mac[i] == 0x00) z++;
            }
            if (f == 6 || z == 6) {
                mac[0] = 0x02; mac[1] = 0x00; mac[2] = 0x52;
                mac[3] = (net_u8)((io_base >> 8) & 0xFF);
                mac[4] = (net_u8)(io_base & 0xFF);
                mac[5] = 0x01;
            } else {
                mac[0] &= (net_u8)0xFE;
            }
        }
    }

    /* Setup rings */
    for (int i = 0; i < NUM_RX; i++) {
        rx_ring[i].tbadr = (net_u32)aos_virt_to_phys(&rx_buf[i][0]);
        rx_ring[i].length = (short)(0xF000 | ((-RX_BUF_SZ) & 0x0FFF));
        rx_ring[i].status = 0x8000; /* OWN to card */
        rx_ring[i].misc = 0;
        rx_ring[i].reserved = 0;
    }
    for (int i = 0; i < NUM_TX; i++) {
        tx_ring[i].tbadr = (net_u32)aos_virt_to_phys(&tx_buf[i][0]);
        tx_ring[i].length = 0;
        tx_ring[i].status = 0;
        tx_ring[i].misc = 0;
        tx_ring[i].reserved = 0;
    }

    /* Init block */
    for (net_u32 i = 0; i < sizeof(init_block); i++)
        ((net_u8 *)&init_block)[i] = 0;

    init_block.mode = 0x8000; /* PROM promiscuous — accept all until stable */
    /* rlen/tlen: high nibble = log2(ring size) */
    init_block.rlen = (net_u8)(3 << 4); /* log2(8)=3 */
    init_block.tlen = (net_u8)(2 << 4); /* log2(4)=2 */
    for (int i = 0; i < 6; i++) init_block.padr[i] = mac[i];
    init_block.ladr = 0;
    init_block.rdra = (net_u32)aos_virt_to_phys(&rx_ring[0]);
    init_block.tdra = (net_u32)aos_virt_to_phys(&tx_ring[0]);

    net_u32 ib_addr = (net_u32)aos_virt_to_phys(&init_block);
    pcnet_write_csr(CSR1, ib_addr & 0xFFFF);
    pcnet_write_csr(CSR2, (ib_addr >> 16) & 0xFFFF);

    /* CSR3: mask some interrupts, enable BAM style options */
    pcnet_write_csr(CSR3, 0x0000);

    /* INIT */
    pcnet_write_csr(CSR0, CSR0_INIT);
    /* wait IDON */
    for (int t = 0; t < 100000; t++) {
        net_u32 csr0 = pcnet_read_csr(CSR0);
        if (csr0 & CSR0_IDON) break;
    }
    /* CSR4: enable auto-pad, disable TX poll timer issues */
    pcnet_write_csr(CSR4, 0x0915);

    /* clear IDON, start (poll mode) */
    pcnet_write_csr(CSR0, CSR0_IDON | CSR0_STRT);

    rx_cur = 0;
    tx_cur = 0;
    active = NET_TRUE;
    return NET_TRUE;
}

void pcnet_get_mac(net_u8 *mac_out)
{
    for (int i = 0; i < 6; i++) mac_out[i] = mac[i];
}

net_bool pcnet_transmit(const net_u8 *data, net_u32 len)
{
    if (!active || !data || len == 0) return NET_FALSE;
    if (len > TX_BUF_SZ) len = TX_BUF_SZ;

    pcnet_tdte_t *d = &tx_ring[tx_cur];

    /* short bounded wait for descriptor */
    for (int t = 0; t < 50000; t++) {
        if (!(d->status & 0x8000)) break;
    }
    if (d->status & 0x8000)
        return NET_FALSE;

    net_u32 send_len = len < 60 ? 60 : len;
    for (net_u32 i = 0; i < len; i++)
        tx_buf[tx_cur][i] = data[i];
    for (net_u32 i = len; i < send_len; i++)
        tx_buf[tx_cur][i] = 0;

    d->tbadr  = (net_u32)aos_virt_to_phys(&tx_buf[tx_cur][0]);
    d->length = (short)(0xF000 | ((-(int)send_len) & 0x0FFF));
    d->misc   = 0;
    /* OWN | STP | ENP  (do not set ADD_FCS; chip adds CRC) */
    __asm__ volatile ("" ::: "memory");
    d->status = 0x8300;
    __asm__ volatile ("" ::: "memory");

    /* Demand TX; keep STRT+IENA. Writing 1 to status flags clears them. */
    pcnet_write_csr(CSR0, CSR0_TDMD | CSR0_STRT);

    tx_cur = (tx_cur + 1) % NUM_TX;
    return NET_TRUE;
}

void pcnet_poll(void)
{
    if (!active) return;

    /* Clear interrupt flags by writing 1s; never clear STRT accidentally. */
    net_u32 csr0 = pcnet_read_csr(CSR0);
    net_u32 to_clear = csr0 & (CSR0_RINT | CSR0_TINT | CSR0_IDON |
                               CSR0_MERR | CSR0_MISS | CSR0_CERR | CSR0_ERR);
    if (to_clear) {
        pcnet_write_csr(CSR0, to_clear | CSR0_STRT);
    } else if (!(csr0 & CSR0_STRT)) {
        /* recover if chip stopped */
        pcnet_write_csr(CSR0, CSR0_STRT);
    }

    for (int n = 0; n < NUM_RX; n++) {
        pcnet_rdte_t *d = &rx_ring[rx_cur];
        if (d->status & 0x8000)
            break; /* card still owns it */

        net_u32 mlen = d->misc & 0x0FFF;
        if (mlen < 14 || mlen > RX_BUF_SZ) {
            /* recovery: just recycle descriptor */
            d->length = (short)(0xF000 | ((-RX_BUF_SZ) & 0x0FFF));
            d->misc = 0;
            __asm__ volatile ("" ::: "memory");
            d->status = 0x8000;
            rx_cur = (rx_cur + 1) % NUM_RX;
            continue;
        }

        net_u32 plen = (mlen >= 4) ? (mlen - 4) : mlen;
        if (plen >= 14)
            net_on_frame(rx_buf[rx_cur], plen);

        d->length = (short)(0xF000 | ((-RX_BUF_SZ) & 0x0FFF));
        d->misc = 0;
        __asm__ volatile ("" ::: "memory");
        d->status = 0x8000;
        rx_cur = (rx_cur + 1) % NUM_RX;
    }
}
