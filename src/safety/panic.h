#ifndef TITAN_PANIC_H
#define TITAN_PANIC_H
#include "../network/net_types.h"

void panic_init(void);
void panic_note_irq(net_u32 vec);
void panic_dump_log(void (*out)(const char *fmt, ...));
net_u32 panic_trap_count(void);

#endif
