/* ============================================================================
 *  network/tcp.h  -- Minimal TCP client (one connection)
 * ==========================================================================*/
#ifndef TITAN_TCP_H
#define TITAN_TCP_H

#include "net_types.h"

void     tcp_init(void);
void     tcp_poll(void);
void     tcp_receive(const net_u8 *segment, net_u32 len, net_u32 src_ip, net_u32 dst_ip);

/* Client API — blocking-style via net_poll loops inside */
net_bool tcp_connect(net_u32 dst_ip, net_u16 dst_port,
                     void (*printfn)(const char *fmt, ...));
net_bool tcp_send(const void *data, net_u32 len);
/* Copy up to max_len received bytes; returns count (0 if none yet) */
net_u32  tcp_recv(void *buf, net_u32 max_len);
net_bool tcp_is_established(void);
void     tcp_close(void);

/* Shell: httpget HOST [PATH]  — TCP:80 GET */
void     tcp_cmd_httpget(const char *arg, void (*printfn)(const char *fmt, ...));
void     tcp_cmd_connect(const char *arg, void (*printfn)(const char *fmt, ...));

#endif
