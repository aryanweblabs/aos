/* SPDX-License-Identifier: MIT */
/* Deliberately touches kernel memory: the kernel must kill only this process. */
#include "libaos.h"
int main(void) {
    puts_("fault: writing to kernel memory...\n");
    *(volatile int*)0x1000 = 1;                  /* identity-mapped kernel page, supervisor only */
    puts_("fault: NOT REACHED\n");
    return 0;
}
