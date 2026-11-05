/* SPDX-License-Identifier: MIT */
#include "libaos.h"
int main(int argc, char** argv) {
    puts_("Hello from ring 3! pid=");
    putu_(getpid_());
    puts_(", argc=");
    putu_((u64)argc);
    for (int i = 0; i < argc; ++i) { puts_(i ? " " : " ["); puts_(argv[i]); }
    puts_("]\n");
    /* heap: grow the break and use it */
    char* heap = (char*)sys(SYS_BRK, 0, 0, 0);
    sys(SYS_BRK, (u64)heap + 8192, 0, 0);
    for (int i = 0; i < 8192; ++i) heap[i] = (char)i;
    u64 sum = 0;
    for (int i = 0; i < 8192; ++i) sum += (unsigned char)heap[i];
    puts_("heap checksum = "); putu_(sum); puts_(sum == 1044480 ? " (ok)\n" : " (WRONG)\n");
    /* anonymous mmap */
    char* m = (char*)sys(SYS_MMAP, 100000, 0, 0);
    m[0] = 1; m[99999] = 2;
    puts_("mmap ok, kernel tick = "); putu_(ticks_()); puts_("\n");
    return 0;
}
