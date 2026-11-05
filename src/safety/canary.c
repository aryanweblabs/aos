#include "canary.h"

static uint32_t fails;

void canary_init(void) { fails = 0; }

void canary_poison(void *p, uint32_t bytes)
{
    uint8_t *b = (uint8_t *)p;
    for (uint32_t i = 0; i < bytes; i++)
        b[i] = 0xA5;
}

void canary_set(void *p, uint32_t bytes)
{
    if (!p || bytes < 8) return;
    uint32_t *f = (uint32_t *)((uint8_t *)p + bytes - 4);
    *f = CANARY_MAGIC ^ bytes;
}

int canary_check(void *p, uint32_t bytes)
{
    if (!p || bytes < 8) return 1;
    uint32_t *f = (uint32_t *)((uint8_t *)p + bytes - 4);
    if (*f != (CANARY_MAGIC ^ bytes)) {
        fails++;
        return 0;
    }
    return 1;
}

uint32_t canary_failures(void) { return fails; }
