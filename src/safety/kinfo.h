#ifndef AOS_KINFO_H
#define AOS_KINFO_H
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SUBSYS_OFF = 0,
    SUBSYS_OK,
    SUBSYS_DEGRADED,
    SUBSYS_DOWN
} subsys_state_t;

#define KINFO_MAX 12

void kinfo_init(void);
int  kinfo_register(const char *name);
void kinfo_set(int id, subsys_state_t s);
subsys_state_t kinfo_get(int id);
void kinfo_report(void (*out)(const char *fmt, ...));

#ifdef __cplusplus
}
#endif
#endif
