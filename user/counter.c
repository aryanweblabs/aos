/* SPDX-License-Identifier: MIT */
/* Counts for a while, sleeping between lines: shows that user processes are preempted and that sleep works. */
#include "libaos.h"
int main(void) {
    for (int i = 1; i <= 5; ++i) {
        puts_("counter: "); putu_((u64)i); puts_(" (tick "); putu_(ticks_()); puts_(")\n");
        sleep_ms_(200);
    }
    volatile u64 spin = 0;                       /* busy loop: the timer must take the CPU away from us */
    u64 end = ticks_() + 45;
    while (ticks_() < end) ++spin;
    puts_("counter: done after a preempted busy loop\n");
    return 7;
}
