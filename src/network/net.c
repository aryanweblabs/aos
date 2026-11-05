/* SPDX-License-Identifier: MIT */
/* ============================================================================
 *  network/net.c  -- Core network stack + shell commands
 * ==========================================================================*/
#include "net.h"
#include "e1000.h"
#include "pcnet.h"
#include "ethernet.h"
#include "arp.h"
#include "ipv4.h"
#include "icmp.h"
#include "pci.h"
#include "udp.h"
#include "dhcp.h"
#include "dns.h"
#include "tcp.h"

net_if_t g_netif;

typedef void (*net_print_fn)(const char *fmt, ...);

/* crude delay — busy loop (no calibrated timer needed) */
static void net_delay(net_u32 loops)
{
    /* Poll NIC periodically; pure busy-wait otherwise.
       Avoid hammering IO ports every iteration (can freeze some NICs). */
    for (volatile net_u32 i = 0; i < loops; i++) {
        if ((i & 0x3FF) == 0)
            net_poll();
    }
}

/* which NIC driver is active: 0=none, 1=e1000, 2=pcnet */
static int g_nic_driver = 0;

net_bool net_init(void)
{
    g_netif.ip       = NET_DEFAULT_IP;
    g_netif.netmask  = NET_DEFAULT_NETMASK;
    g_netif.gateway  = NET_DEFAULT_GATEWAY;
    g_netif.dns      = NET_DEFAULT_DNS;
    g_netif.link_up  = NET_FALSE;
    g_netif.initialized = NET_FALSE;
    g_nic_driver = 0;

    for (int i = 0; i < ETH_ADDR_LEN; i++)
        g_netif.mac[i] = 0;

    arp_init();
    ipv4_init();
    icmp_init();
    udp_init();
    dhcp_init();
    dns_init();
    tcp_init();

    /* Prefer Intel e1000 (QEMU default); fall back to AMD PCnet (VirtualBox default) */
    if (e1000_init()) {
        e1000_get_mac(g_netif.mac);
        g_nic_driver = 1;
    } else if (pcnet_init()) {
        pcnet_get_mac(g_netif.mac);
        g_nic_driver = 2;
    } else {
        return NET_FALSE;
    }

    g_netif.link_up = NET_TRUE;
    g_netif.initialized = NET_TRUE;
    return NET_TRUE;
}

void net_poll(void)
{
    if (!g_netif.initialized) return;
    if (g_nic_driver == 1) e1000_poll();
    else if (g_nic_driver == 2) pcnet_poll();
    tcp_poll();
}

void net_on_frame(const net_u8 *frame, net_u32 len)
{
    eth_receive(frame, len);
}

net_bool net_transmit(const net_u8 *frame, net_u32 len)
{
    if (g_nic_driver == 1) return e1000_transmit(frame, len);
    if (g_nic_driver == 2) return pcnet_transmit(frame, len);
    return NET_FALSE;
}

void net_tick(net_u32 ticks)
{
    if (!g_netif.initialized) return;
    ping_tick(ticks);
}

/* -------------------- Shell commands -------------------- */

void net_cmd_ifconfig(net_print_fn printfn)
{
    char ip[16], mask[16], gw[16], mac[18];

    if (!g_netif.initialized) {
        printfn("network: not initialized (no NIC found)\n");
        printfn("Supported NICs: Intel e1000 (QEMU) or AMD PCnet (VirtualBox default)\n");
        return;
    }

    net_ip_to_str(g_netif.ip, ip);
    net_ip_to_str(g_netif.netmask, mask);
    net_ip_to_str(g_netif.gateway, gw);
    eth_addr_to_str(g_netif.mac, mac);

    printfn("eth0: %s (%s)\n", g_netif.link_up ? "UP" : "DOWN",
            g_nic_driver == 1 ? "e1000" : (g_nic_driver == 2 ? "pcnet/AMD" : "?"));
    printfn("  MAC:     %s\n", mac);
    printfn("  inet:    %s\n", ip);
    printfn("  netmask: %s\n", mask);
    printfn("  gateway: %s\n", gw);
    {
        char dns[16];
        net_ip_to_str(g_netif.dns, dns);
        printfn("  dns:     %s\n", dns);
    }
}

void net_cmd_arp(net_print_fn printfn)
{
    if (!g_netif.initialized) {
        printfn("network: not initialized\n");
        return;
    }
    arp_dump_cache(printfn);
}

void net_cmd_lspci(net_print_fn printfn)
{
    pci_list_devices(printfn);
}

/* Resolve IP -> MAC with retries. next_hop for off-subnet. */
static net_bool resolve_mac(net_u32 dst_ip, net_u8 *mac_out, net_print_fn printfn)
{
    net_u32 next_hop = dst_ip;
    if ((dst_ip & g_netif.netmask) != (g_netif.ip & g_netif.netmask))
        next_hop = g_netif.gateway;

    char hopstr[16];
    net_ip_to_str(next_hop, hopstr);

    for (int attempt = 0; attempt < 8; attempt++) {
        if (arp_resolve(next_hop, mac_out))
            return NET_TRUE;

        if (attempt == 0)
            printfn("ARP: who-has %s ...\n", hopstr);

        /* wait for reply while polling NIC */
        net_delay(300000);
    }
    printfn("ARP: timeout for %s\n", hopstr);
    return NET_FALSE;
}

void net_cmd_ping(const char *arg, net_print_fn printfn)
{
    if (!g_netif.initialized) {
        printfn("network: not initialized (no NIC)\n");
        return;
    }

    if (!arg || !*arg) {
        printfn("usage: ping <ip>\n");
        printfn("example: ping 10.0.2.2\n");
        return;
    }

    while (*arg == ' ') arg++;

    net_u32 ip = net_str_to_ip(arg);
    if (ip == 0) {
        ip = dns_resolve(arg, printfn);
        if (ip == 0) {
            printfn("ping: cannot resolve '%s'\n", arg);
            return;
        }
    }

    char ipstr[16];
    net_ip_to_str(ip, ipstr);
    printfn("PING %s 32 bytes of data.\n", ipstr);

    /* 1) Resolve next-hop MAC first */
    net_u8 dst_mac[6];
    if (!resolve_mac(ip, dst_mac, printfn)) {
        printfn("--- %s ping statistics ---\n", ipstr);
        printfn("0 packets transmitted (ARP failed)\n");
        return;
    }

    char macstr[18];
    eth_addr_to_str(dst_mac, macstr);
    printfn("next-hop MAC %s\n", macstr);

    /* 2) Send 4 echo requests */
    net_u32 sent = 0, recv = 0, lost = 0;
    net_u16 id = 0x5449; /* 'TI' */
    static const net_u8 payload[32] = {
        'T','i','t','a','n','P','i','n','g','!',
        0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21
    };

    ping_start(ip); /* sets g_ping for reply matching */

    for (net_u16 seq = 1; seq <= 4; seq++) {
        g_ping.seq = seq;
        g_ping.waiting = NET_TRUE;
        g_ping.replies = recv; /* track new replies */

        net_u32 before = g_ping.replies;
        if (!icmp_send_echo(ip, id, seq, payload, 32)) {
            printfn("ping: send failed (seq %u)\n", (unsigned)seq);
            lost++;
            continue;
        }
        sent++;

        /* wait up to ~1s for reply */
        net_bool got = NET_FALSE;
        for (int w = 0; w < 40; w++) {
            net_delay(80000);
            if (g_ping.replies > before) {
                got = NET_TRUE;
                break;
            }
        }

        if (got) {
            recv++;
            printfn("64 bytes from %s: icmp_seq=%u ttl=64\n",
                    ipstr, (unsigned)seq);
        } else {
            lost++;
            printfn("Request timeout for icmp_seq %u\n", (unsigned)seq);
        }
        g_ping.waiting = NET_FALSE;

        /* small gap between probes */
        net_delay(100000);
    }

    ping_stop();

    printfn("--- %s ping statistics ---\n", ipstr);
    printfn("%u packets transmitted, %u received, %u lost\n",
            (unsigned)sent, (unsigned)recv, (unsigned)lost);
}


void net_cmd_dhcp(void (*printfn)(const char *fmt, ...))
{
    dhcp_run(printfn);
    net_cmd_ifconfig(printfn);
}

void net_cmd_nslookup(const char *arg, void (*printfn)(const char *fmt, ...))
{
    dns_cmd_lookup(arg, printfn);
}

void net_cmd_httpget(const char *arg, void (*printfn)(const char *fmt, ...))
{
    tcp_cmd_httpget(arg, printfn);
}
