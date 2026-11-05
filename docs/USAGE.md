# AOS — Detailed Usage Guide

This guide explains how to **build**, **run**, and **use** the AOS kernel from a fresh machine to everyday shell work and userspace programs.

Repository: `https://github.com/aryanweblabs/aos`  
Maintainer: **aryanweblabs** · License: **MIT**

---

## Table of contents

1. [Requirements](#1-requirements)
2. [Install dependencies](#2-install-dependencies)
3. [Get the source](#3-get-the-source)
4. [Build the kernel](#4-build-the-kernel)
5. [Run in QEMU](#5-run-in-qemu)
6. [First boot checklist](#6-first-boot-checklist)
7. [Shell reference](#7-shell-reference)
8. [Filesystem (AOSFS)](#8-filesystem-aosfs)
9. [Userspace programs](#9-userspace-programs)
10. [Virtual memory features](#10-virtual-memory-features)
11. [Page cache](#11-page-cache)
12. [Networking](#12-networking)
13. [Build variants](#13-build-variants)
14. [Writing your own user program](#14-writing-your-own-user-program)
15. [Troubleshooting](#15-troubleshooting)

---

## 1. Requirements

| Item | Recommendation |
|------|----------------|
| Host OS | Linux (Ubuntu 22.04+ / Debian 12+ recommended) |
| CPU | x86_64 |
| RAM | 4 GiB+ host free |
| Disk | ~500 MB for tools + build |
| Emulator | QEMU system x86 |

Windows/macOS: use WSL2 or a Linux VM for the simplest path.

---

## 2. Install dependencies

```bash
sudo apt update
sudo apt install -y \
  build-essential \
  grub-pc-bin grub-common \
  xorriso mtools \
  qemu-system-x86 \
  python3
```

| Package | Role |
|---------|------|
| `build-essential` | `g++`, `gcc`, `ld`, `make` |
| `grub-pc-bin` / `grub-common` | `grub-mkrescue` for bootable ISO |
| `xorriso` / `mtools` | ISO / FAT helpers used by GRUB |
| `qemu-system-x86` | Emulator |

Optional: `git` to clone the repository.

---

## 3. Get the source

```bash
git clone https://github.com/aryanweblabs/aos.git
cd aos
```

Or unpack a release source zip and `cd` into the project root (directory that contains `Makefile`).

---

## 4. Build the kernel

```bash
make
```

This produces:

- `build/hh0_smp0_apic1/kernel.elf` — kernel binary  
- `build/hh0_smp0_apic1/aos.iso` — Multiboot2 ISO  
- `build/aos.iso` — copy used by run targets  
- `build/user/*.elf` — userspace programs packed into the initramfs  

Useful targets:

```bash
make user          # only user programs
make check         # static ELF checks + host tests
make clean         # remove build outputs
make V=1           # verbose compiler commands
```

---

## 5. Run in QEMU

### Serial-only (recommended)

```bash
make run-serial
```

- No graphical window  
- Shell on the **same terminal**  
- Exit QEMU: `Ctrl-A` then `X` (or close the terminal)

### With display window + serial

```bash
make run
```

### Other run modes

```bash
make run-smp       # needs: make SMP=1 first; 4 vCPUs
make run-usb       # USB mouse/tablet devices attached
```

A 16 MiB raw disk image `build/aos-data.img` is created automatically for AOSFS.

---

## 6. First boot checklist

You should see something like:

```text
========================================
  AOS Enhanced 64-bit (headless kernel)
========================================
[boot] Multiboot2 OK
[boot] Initramfs mounted
[boot] PMM free frames: ...
[boot] AOSFS mounted from disk   # or a message to format
[boot] Network OK (NIC found)    # under QEMU user-net
Type 'help' for commands.
aos:/>
```

Try:

```text
help
mem
cpu
uptime
exec hello.elf
exec vmtest.elf
```

If AOSFS is not mounted:

```text
disks
mkaosfs 0 0 CONFIRM
```

Then reboot (`reboot`) so the filesystem is used cleanly.

---

## 7. Shell reference

| Command | Description |
|---------|-------------|
| `help` | List commands |
| `mem` | Free frames / heap pages |
| `cache` | Page-cache hit/miss stats |
| `cpu` | CPUID vendor / features |
| `uptime` / `date` | Time since boot / CMOS RTC |
| `tasks` | Kernel threads |
| `ps` | User processes |
| `exec <file> [args]` | Run ELF and wait |
| `run <file> [args]` | Run ELF in background |
| `kill <pid>` | Kill process |
| `ls` / `cd` / `pwd` | Directories |
| `cat` / `write` / `touch` / `mkdir` / `rm` | Files |
| `sync` / `format CONFIRM` | Flush / wipe AOSFS |
| `disks` / `disk` / `mkaosfs …` | Storage setup |
| `ifconfig` / `ping` / `dhcp` / … | Network (if NIC present) |
| `reboot` / `poweroff` | Restart / shut down |

---

## 8. Filesystem (AOSFS)

- Persistent on the QEMU/ATA (or NVMe) disk image  
- Survives reboot when the same `aos-data.img` is used  
- File names: letters, digits, `_ . -` (max length limited)  

Example session:

```text
mkdir notes
cd notes
write readme.txt This is stored on disk
cat readme.txt
cd /
ls
sync
```

Page cache speeds repeated reads; see `cache` after several `cat`s.

---

## 9. Userspace programs

Shipped in the initramfs (run with `exec <name>.elf`):

| Program | Purpose |
|---------|---------|
| `hello.elf` | Ring-3 smoke test, mmap/heap demo |
| `forkdemo.elf` | `fork` + `wait` |
| `vmtest.elf` | Full VM suite (demand page, COW, munmap, mprotect) |
| `counter.elf` / `fault.elf` / `fsio.elf` / `pipedemo.elf` | Extra demos |

Expected:

```text
aos:/> exec vmtest.elf
...
RESULT: ALL TESTS PASSED
```

---

## 10. Virtual memory features

Documented in depth in [MEMORY.md](MEMORY.md).

- **Demand paging** — `mmap` regions cost almost no RAM until touched  
- **COW fork** — child shares pages; write triggers a private copy  
- **munmap / mprotect** — syscalls 20 and 21  

---

## 11. Page cache

Documented in [VFS.md](VFS.md).

```text
aos:/> cache
Page cache
  capacity : 512 slots (256 KiB)
  hits     : ...
  misses   : ...
  hit rate : ...%
```

---

## 12. Networking

Under default `make run-serial`, QEMU user networking + e1000 is enabled.

```text
ifconfig
dhcp
ping 10.0.2.2
```

Behaviour depends on QEMU and host network; treat this as best-effort.

---

## 13. Build variants

```bash
make HH=1          # higher-half kernel
make SMP=1         # SMP + then: make run-smp
make APIC=0        # PIC + PIT only
```

Combinations build into separate directories under `build/`.

---

## 14. Writing your own user program

1. Create `user/myprog.c` using `libaos.h`  
2. Add `myprog` to `USER_PROGS` in the `Makefile`  
3. `make user` && rebuild ISO (`make`)  
4. `exec myprog.elf`  

Syscall list: [SYSCALLS.md](SYSCALLS.md).

Minimal example:

```c
#include "libaos.h"
int main(void) {
    puts_("hello from myprog\n");
    return 0;
}
```

---

## 15. Troubleshooting

| Problem | What to try |
|---------|-------------|
| `grub-mkrescue: command not found` | Install `grub-pc-bin` `grub-common` `xorriso` |
| ISO build fails | `make clean && make`; ensure disk space |
| QEMU not found | `sudo apt install qemu-system-x86` |
| No AOSFS | `disks` then `mkaosfs … CONFIRM`, reboot |
| `exec` → not found | Use full name: `hello.elf` |
| Black screen with `make run` | Prefer `make run-serial` |
| Want to wipe disk | Delete `build/aos-data.img` and run again |

Still stuck? Open an issue at  
https://github.com/aryanweblabs/aos/issues  
and attach the serial boot log.

---

## License

MIT © [aryanweblabs](https://github.com/aryanweblabs) — see [LICENSE](../LICENSE).
