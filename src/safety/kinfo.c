#include "kinfo.h"

typedef struct {
    const char *name;
    subsys_state_t st;
} kinfo_node_t;

static kinfo_node_t nodes[KINFO_MAX];
static int used;

void kinfo_init(void)
{
    used = 0;
    for (int i = 0; i < KINFO_MAX; i++) {
        nodes[i].name = 0;
        nodes[i].st = SUBSYS_OFF;
    }
}

int kinfo_register(const char *name)
{
    if (used >= KINFO_MAX || !name) return -1;
    nodes[used].name = name;
    nodes[used].st = SUBSYS_OK;
    return used++;
}

void kinfo_set(int id, subsys_state_t s)
{
    if (id < 0 || id >= used) return;
    nodes[id].st = s;
}

subsys_state_t kinfo_get(int id)
{
    if (id < 0 || id >= used) return SUBSYS_OFF;
    return nodes[id].st;
}

void kinfo_report(void (*out)(const char *fmt, ...))
{
    static const char *names[] = { "OFF", "OK", "DEGRADED", "DOWN" };
    out("kernel subsystems (%d):\n", used);
    for (int i = 0; i < used; i++) {
        int s = (int)nodes[i].st;
        if (s < 0 || s > 3) s = 0;
        out("  [%d] %s  %s\n", i, nodes[i].name, names[s]);
    }
}
