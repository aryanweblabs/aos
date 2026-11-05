# AOS Syscall ABI

Userspace enters the kernel with the `syscall` instruction (also legacy `int 0x80` path exists in the IDT).

Calling convention (userspace → kernel):

| Register | Role |
|----------|------|
| RAX | Syscall number |
| RDI | arg0 |
| RSI | arg1 |
| RDX | arg2 |
| R10 | reserved / 0 in `libaos.h` helpers |

Return value: **RAX** (negative values are `-errno` style codes).

Header for programs: [`user/libaos.h`](../user/libaos.h).

## Numbers

| # | Name | Args | Returns |
|---|------|------|---------|
| 0 | `SYS_EXIT` | code | does not return |
| 1 | `SYS_WRITE` | fd, buf, len | bytes written |
| 2 | `SYS_READ` | fd, buf, len | bytes read |
| 3 | `SYS_YIELD` | — | 0 |
| 4 | `SYS_SLEEP_MS` | ms | 0 |
| 5 | `SYS_GETPID` | — | pid |
| 6 | `SYS_TICKS` | — | kernel tick count (~90 Hz) |
| 7 | `SYS_BRK` | addr (0 = query) | new break |
| 8 | `SYS_MMAP` | len | address or negative error |
| 9 | `SYS_SPAWN` | path, args | pid |
| 10 | `SYS_WAIT` | pid, int* code | pid |
| 11 | `SYS_KILL` | pid | 0 or error |
| 12 | `SYS_FREAD` | path, buf, max | bytes (legacy) |
| 13 | `SYS_FWRITE` | path, buf, len | 0 (legacy) |
| 14 | `SYS_OPEN` | path, flags | fd |
| 15 | `SYS_CLOSE` | fd | 0 |
| 16 | `SYS_PIPE` | int fds[2] | 0 |
| 17 | `SYS_DUP2` | oldfd, newfd | newfd |
| 18 | `SYS_LSEEK` | fd, off, whence | offset |
| 19 | `SYS_FORK` | — | 0 child / pid parent |
| 20 | `SYS_MUNMAP` | addr, len | 0 |
| 21 | `SYS_MPROTECT` | addr, len, prot | 0 |

## Error codes (subset)

| Value | Name |
|-------|------|
| -1 | E_PERM |
| -2 | E_NOENT |
| -9 | E_BADF |
| -12 | E_NOMEM |
| -14 | E_FAULT |
| -22 | E_INVAL |
| -38 | E_NOSYS |

## Memory helpers

```c
i64 mmap_(u64 len);                           /* anonymous RW, demand-paged */
i64 munmap_(void* addr, u64 len);
i64 mprotect_(void* addr, u64 len, u64 prot); /* PROT_READ|WRITE|EXEC */
```

`mmap` length is rounded up to pages. Addresses come from the process `mmap_next` region.

## Notes

- Programs are static ELF `ET_EXEC`, linked at `0x8000000000` (`user/user.ld`)
- There is no full libc — only `libaos.h` freestanding helpers
- Adding a syscall: update `syscall.hpp`, `syscall.cpp`, `libaos.h`, and this file
