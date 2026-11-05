/* SPDX-License-Identifier: MIT */
#pragma once
#include <stdint.h>

void interrupt_init();
void interrupt_load_idt();   // lidt on the calling CPU (application processors)
uint64_t timer_ticks();
char keyboard_read_char();
void ps2_poll();        // drain the i8042 by polling (fallback when IRQ1/12 never arrive)
bool input_pending();   // true if a key or mouse event is waiting (used to skip hlt)
