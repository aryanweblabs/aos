#ifndef AOS_WATCHDOG_H
#define AOS_WATCHDOG_H
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void watchdog_init(uint32_t ticks_limit);
void watchdog_pet(uint32_t now_ticks);
void watchdog_check(uint32_t now_ticks);
int  watchdog_tripped(void);
uint32_t watchdog_last_pet(void);
void watchdog_set_limit(uint32_t ticks);

#ifdef __cplusplus
}
#endif
#endif
