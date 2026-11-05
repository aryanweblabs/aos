#include "panic.h"

#define LOG_N 32
static net_u32 logv[LOG_N];
static net_u32 log_i;
static net_u32 traps;

void panic_note_irq(net_u32 vec)
{
    logv[log_i % LOG_N] = vec;
    log_i++;
    traps++;
}

net_u32 panic_trap_count(void) { return traps; }

void panic_dump_log(void (*out)(const char *fmt, ...))
{
    net_u32 n = log_i < LOG_N ? log_i : LOG_N;
    out("irq log (%u):\n", (unsigned)log_i);
    for (net_u32 k = 0; k < n; k++) {
        net_u32 idx = (log_i - n + k) % LOG_N;
        out("  [%u] %u\n", (unsigned)k, (unsigned)logv[idx]);
    }
}

void panic_init(void)
{
    log_i = 0;
    traps = 0;
}
