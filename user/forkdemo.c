/* SPDX-License-Identifier: MIT */
#include "libaos.h"

int main(int argc, char** argv) {
    (void)argc; (void)argv;
    puts_("=== fork demo ===\n");
    i64 pid = fork_();
    if (pid < 0) {
        puts_("fork failed\n");
        return 1;
    }
    if (pid == 0) {
        puts_("child: hello, my pid=");
        putu_(getpid_());
        puts_("\n");
        return 42;
    }
    puts_("parent: child pid=");
    putu_((u64)pid);
    puts_(", waiting...\n");
    int code = 0;
    /* wait(pid, &code) */
    i64 w = sys(SYS_WAIT, (u64)pid, (u64)&code, 0);
    puts_("parent: wait returned ");
    putu_((u64)w);
    puts_(" exit=");
    putu_((u64)(unsigned)code);
    puts_("\n");
    puts_("fork demo OK\n");
    return 0;
}
