/* SPDX-License-Identifier: MIT */
#ifndef AOS_CPU_H
#define AOS_CPU_H

typedef unsigned char      cpu_u8;
typedef unsigned short     cpu_u16;
typedef unsigned int       cpu_u32;
typedef int                cpu_bool;
#define CPU_TRUE  1
#define CPU_FALSE 0

#define CPU_VENDOR_LEN  13
#define CPU_BRAND_LEN   49

typedef struct {
    char     vendor[CPU_VENDOR_LEN];
    char     brand[CPU_BRAND_LEN];
    cpu_u32  max_basic;
    cpu_u32  max_ext;
    cpu_u32  family;
    cpu_u32  model;
    cpu_u32  stepping;
    cpu_u32  features_edx;
    cpu_u32  features_ecx;
    cpu_bool has_cpuid;
    cpu_bool initialized;
} cpu_info_t;

#ifdef __cplusplus
extern "C" {
#endif

extern cpu_info_t g_cpu;
void cpu_init(void);
void cpu_cmd_info(void (*printfn)(const char *fmt, ...));

#ifdef __cplusplus
}
#endif

#endif
