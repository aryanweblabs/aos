/* SPDX-License-Identifier: MIT */
/* libaos.h - freestanding syscall wrappers for AOS user programs. */
#ifndef LIBAOS_H
#define LIBAOS_H
typedef unsigned long u64;
typedef long          i64;

enum {
    SYS_EXIT = 0, SYS_WRITE = 1, SYS_READ = 2, SYS_YIELD = 3, SYS_SLEEP_MS = 4,
    SYS_GETPID = 5, SYS_TICKS = 6, SYS_BRK = 7, SYS_MMAP = 8,
    SYS_SPAWN = 9, SYS_WAIT = 10, SYS_KILL = 11, SYS_FREAD = 12, SYS_FWRITE = 13,
    SYS_OPEN = 14, SYS_CLOSE = 15, SYS_PIPE = 16, SYS_DUP2 = 17, SYS_LSEEK = 18, SYS_FORK = 19,
    SYS_MUNMAP = 20, SYS_MPROTECT = 21
};

enum {
    O_RDONLY = 0, O_WRONLY = 1, O_RDWR = 2,
    O_CREAT = 0x40, O_TRUNC = 0x200, O_APPEND = 0x400
};

enum { PROT_READ = 1, PROT_WRITE = 2, PROT_EXEC = 4 };

static inline i64 sys(u64 n, u64 a, u64 b, u64 c) {
    i64 r;
    register u64 r10 __asm__("r10") = 0;
    __asm__ volatile("syscall" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c), "r"(r10) : "rcx", "r11", "memory");
    return r;
}
static inline u64 slen(const char* s) { u64 n = 0; while (s[n]) ++n; return n; }
static inline void puts_(const char* s) { sys(SYS_WRITE, 1, (u64)s, slen(s)); }
static inline void putu_(u64 v) {
    char b[21]; int n = 20; b[n] = 0;
    if (!v) b[--n] = '0';
    while (v) { b[--n] = (char)('0' + v % 10); v /= 10; }
    puts_(&b[n]);
}
static inline void exit_(int code) { sys(SYS_EXIT, (u64)code, 0, 0); for (;;) {} }
static inline u64 ticks_(void) { return (u64)sys(SYS_TICKS, 0, 0, 0); }
static inline void sleep_ms_(u64 ms) { sys(SYS_SLEEP_MS, ms, 0, 0); }
static inline u64 getpid_(void) { return (u64)sys(SYS_GETPID, 0, 0, 0); }

/* FD-based I/O */
static inline i64 open_(const char* path, u64 flags) { return sys(SYS_OPEN, (u64)path, flags, 0); }
static inline i64 close_(int fd) { return sys(SYS_CLOSE, (u64)fd, 0, 0); }
static inline i64 read_(int fd, void* buf, u64 len) { return sys(SYS_READ, (u64)fd, (u64)buf, len); }
static inline i64 write_(int fd, const void* buf, u64 len) { return sys(SYS_WRITE, (u64)fd, (u64)buf, len); }
static inline i64 pipe_(int fds[2]) { return sys(SYS_PIPE, (u64)fds, 0, 0); }
static inline i64 dup2_(int oldfd, int newfd) { return sys(SYS_DUP2, (u64)oldfd, (u64)newfd, 0); }
static inline i64 lseek_(int fd, i64 off, int whence) { return sys(SYS_LSEEK, (u64)fd, (u64)off, (u64)whence); }
static inline i64 fork_(void) { return sys(SYS_FORK, 0, 0, 0); }
static inline i64 mmap_(u64 len) { return sys(SYS_MMAP, len, 0, 0); }
static inline i64 munmap_(void* addr, u64 len) { return sys(SYS_MUNMAP, (u64)addr, len, 0); }
static inline i64 mprotect_(void* addr, u64 len, u64 prot) { return sys(SYS_MPROTECT, (u64)addr, len, prot); }
#endif
