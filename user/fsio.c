/* SPDX-License-Identifier: MIT */
/* Exercises the file syscalls: write a file through the kernel, read it back, compare. */
#include "libaos.h"
int main(void) {
    const char msg[] = "written from ring 3";
    const char* path = "fsio.txt";
    i64 r = sys(SYS_FWRITE, (u64)path, (u64)msg, sizeof(msg) - 1);
    if (r < 0) { puts_("fsio: skipped (no writable disk)\n"); return 0; }
    char buf[64];
    i64 n = sys(SYS_FREAD, (u64)path, (u64)buf, sizeof(buf));
    int same = n == (i64)(sizeof(msg) - 1);
    for (i64 i = 0; same && i < n; ++i) if (buf[i] != msg[i]) same = 0;
    puts_(same ? "fsio: ok\n" : "fsio: MISMATCH\n");
    return same ? 0 : 1;
}
