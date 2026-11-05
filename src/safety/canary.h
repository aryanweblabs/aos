#ifndef AOS_CANARY_H
#define AOS_CANARY_H
#include <stdint.h>
#include <stddef.h>

#define CANARY_MAGIC 0xDEADBEEFu

#ifdef __cplusplus
extern "C" {
#endif

void canary_init(void);
void canary_set(void *p, uint32_t bytes);
int  canary_check(void *p, uint32_t bytes);
void canary_poison(void *p, uint32_t bytes);
uint32_t canary_failures(void);

#ifdef __cplusplus
}
#endif
#endif
