#ifndef TITAN_UDP_H
#define TITAN_UDP_H
#include "net_types.h"
#include "ipv4.h"

#define UDP_HEADER_LEN 8

typedef struct {
    net_u16 src_port;
    net_u16 dst_port;
    net_u16 length;
    net_u16 checksum;
} __attribute__((packed)) udp_header_t;

void udp_init(void);
void udp_receive(const net_u8 *packet, net_u32 len, net_u32 src_ip, net_u32 dst_ip);
net_bool udp_send(net_u32 dst_ip, net_u16 src_port, net_u16 dst_port,
                  const void *payload, net_u32 payload_len);
#endif
