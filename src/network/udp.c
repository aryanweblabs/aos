#include "udp.h"
#include "net.h"
#include "dhcp.h"
#include "dns.h"

void udp_init(void) {}

void udp_receive(const net_u8 *packet, net_u32 len, net_u32 src_ip, net_u32 dst_ip)
{
    (void)dst_ip;
    if (len < UDP_HEADER_LEN) return;
    const udp_header_t *hdr = (const udp_header_t *)packet;
    net_u16 sport = net_ntohs(hdr->src_port);
    net_u16 dport = net_ntohs(hdr->dst_port);
    net_u16 ulen  = net_ntohs(hdr->length);
    if (ulen < UDP_HEADER_LEN || ulen > len) return;
    const net_u8 *payload = packet + UDP_HEADER_LEN;
    net_u32 plen = ulen - UDP_HEADER_LEN;

    if (dport == 68)
        dhcp_udp_receive(payload, plen, src_ip, sport);
    else if (dport == 53 || sport == 53 || dport == 49152)
        dns_udp_receive(payload, plen, src_ip);
}

net_bool udp_send(net_u32 dst_ip, net_u16 src_port, net_u16 dst_port,
                  const void *payload, net_u32 payload_len)
{
    net_u8 buf[1500];
    if (payload_len + UDP_HEADER_LEN > sizeof(buf)) return NET_FALSE;
    udp_header_t *hdr = (udp_header_t *)buf;
    hdr->src_port = net_htons(src_port);
    hdr->dst_port = net_htons(dst_port);
    hdr->length   = net_htons((net_u16)(UDP_HEADER_LEN + payload_len));
    hdr->checksum = 0; /* optional for IPv4 */
    for (net_u32 i = 0; i < payload_len; i++)
        buf[UDP_HEADER_LEN + i] = ((const net_u8 *)payload)[i];
    return ipv4_send(dst_ip, IP_PROTO_UDP, buf, UDP_HEADER_LEN + payload_len);
}
