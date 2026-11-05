#include "watchdog.h"

static uint32_t limit;
static uint32_t last;
static int trip;

void watchdog_init(uint32_t ticks_limit)
{
    limit = ticks_limit ? ticks_limit : 500;
    last = 0;
    trip = 0;
}

void watchdog_set_limit(uint32_t ticks) { limit = ticks; }

void watchdog_pet(uint32_t now_ticks)
{
    last = now_ticks;
    trip = 0;
}

void watchdog_check(uint32_t now_ticks)
{
    if (limit && now_ticks - last > limit)
        trip = 1;
}

int watchdog_tripped(void) { return trip; }
uint32_t watchdog_last_pet(void) { return last; }
