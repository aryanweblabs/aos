/* SPDX-License-Identifier: MIT */
#include "libaos.h"

int main(int argc, char** argv) {
    (void)argc; (void)argv;
    puts_("=== FD + pipe demo ===\n");

    /* --- file open/write/read/close --- */
    int fd = (int)open_("demo.txt", O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0) { puts_("open write failed\n"); return 1; }
    const char* msg = "hello via open/write\n";
    if (write_(fd, msg, slen(msg)) < 0) { puts_("write failed\n"); return 1; }
    close_(fd);

    fd = (int)open_("demo.txt", O_RDONLY);
    if (fd < 0) { puts_("open read failed\n"); return 1; }
    char buf[64];
    i64 n = read_(fd, buf, sizeof(buf) - 1);
    if (n < 0) { puts_("read failed\n"); return 1; }
    buf[n] = 0;
    puts_("file content: "); puts_(buf);
    close_(fd);

    /* --- pipe --- */
    int pfd[2];
    if (pipe_(pfd) < 0) { puts_("pipe failed\n"); return 1; }
    puts_("pipe fds: r="); putu_((u64)pfd[0]); puts_(" w="); putu_((u64)pfd[1]); puts_("\n");

    const char* ping = "ping through pipe";
    if (write_(pfd[1], ping, slen(ping)) < 0) { puts_("pipe write failed\n"); return 1; }
    close_(pfd[1]);   /* signal EOF to reader */

    char rbuf[64];
    n = read_(pfd[0], rbuf, sizeof(rbuf) - 1);
    if (n < 0) { puts_("pipe read failed\n"); return 1; }
    rbuf[n] = 0;
    puts_("pipe read: "); puts_(rbuf); puts_("\n");
    close_(pfd[0]);

    puts_("demo OK\n");
    return 0;
}
