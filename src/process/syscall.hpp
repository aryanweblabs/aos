/* SPDX-License-Identifier: MIT */
// syscall.hpp - system call numbers and dispatcher.
//
// ABI (same register convention as Linux x86-64, different numbers):
//   rax = number, rdi rsi rdx r10 r8 r9 = arguments, result in rax (negative = error).
//   Entered with the SYSCALL instruction (preferred) or int 0x80.  rcx and r11 are clobbered.
#pragma once
#include <stdint.h>
struct InterruptFrame;

namespace aos::syscall {
enum Number : uint64_t {
    SYS_EXIT = 0,      // exit(code)
    SYS_WRITE = 1,     // write(fd, buf, len) -> len   (now real FD-based)
    SYS_READ = 2,      // read(fd, buf, len)  -> bytes
    SYS_YIELD = 3,
    SYS_SLEEP_MS = 4,  // sleep_ms(ms)
    SYS_GETPID = 5,
    SYS_TICKS = 6,     // kernel ticks (90 per second)
    SYS_BRK = 7,       // brk(addr) -> new break (addr 0 = query)
    SYS_MMAP = 8,      // mmap(len) anonymous read/write memory -> address
    SYS_SPAWN = 9,     // spawn(path, args) -> pid
    SYS_WAIT = 10,     // wait(pid, int* code) -> pid
    SYS_KILL = 11,     // kill(pid)
    SYS_FREAD = 12,    // fread(path, buf, max) -> bytes  (legacy path-based)
    SYS_FWRITE = 13,   // fwrite(path, buf, len) -> 0     (legacy path-based)
    SYS_OPEN = 14,     // open(path, flags) -> fd
    SYS_CLOSE = 15,    // close(fd) -> 0
    SYS_PIPE = 16,     // pipe(int fds[2]) -> 0
    SYS_DUP2 = 17,     // dup2(oldfd, newfd) -> newfd
    SYS_LSEEK = 18,    // lseek(fd, off, whence) -> new offset
    SYS_FORK = 19,     // fork() -> child pid (0 in child)
    SYS_MUNMAP = 20,   // munmap(addr, len) -> 0
    SYS_MPROTECT = 21, // mprotect(addr, len, prot) -> 0
    SYS_COUNT
};
constexpr int64_t E_PERM = -1, E_NOENT = -2, E_BADF = -9, E_AGAIN = -11,
                  E_NOMEM = -12, E_ACCES = -13, E_FAULT = -14, E_INVAL = -22,
                  E_PIPE = -32, E_NOSYS = -38;
void dispatch(InterruptFrame* frame);
}  // namespace aos::syscall
