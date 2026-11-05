# AOS — Aryan OS (Enhanced Headless Kernel)

**x86_64 hobby operating system kernel** with demand paging, copy-on-write fork, a persistent disk filesystem, page cache, and a small networking stack.

Open source · **MIT License** · Maintained by [**aryanweblabs**](https://github.com/aryanweblabs)

```text
========================================
  AOS Enhanced 64-bit (headless kernel)
========================================
[boot] Multiboot2 OK
[boot] AOSFS mounted from disk
[boot] APIC: LAPIC timer + I/O APIC active
Type 'help' for commands.
aos:/>
```

---

## Features

| Area | What you get |
|------|----------------|
| **CPU / mode** | x86_64, ring 0 + ring 3, SYSCALL, optional higher-half (`HH=1`) |
| **Scheduling** | Preemptive priority scheduler, optional SMP (`SMP=1`) |
| **Memory** | Per-process address spaces, demand paging, COW `fork`, `mmap` / `munmap` / `mprotect` |
| **Filesystem** | Persistent **AOSFS** on ATA/NVMe, page cache (256 KiB CLOCK) |
| **Networking** | Ethernet, IPv4, TCP/UDP, DHCP, DNS (e1000 / pcnet) |
| **Userspace** | ELF loader, shell on serial, sample programs + `vmtest` |
| **Build** | Multiboot2 ISO via GRUB, QEMU-friendly |

**Not included (by design):** graphical desktop, multi-user security model, full POSIX.

---

## Quick start

### Dependencies (Debian / Ubuntu)

```bash
sudo apt update
sudo apt install build-essential grub-pc-bin grub-common xorriso mtools qemu-system-x86
```

### Build & run

```bash
git clone https://github.com/aryanweblabs/aos.git
cd aos
make                 # builds kernel + bootable ISO
make run-serial      # QEMU, serial shell on this terminal
```

Inside the shell:

```text
help
mem
cache
exec hello.elf
exec forkdemo.elf
exec vmtest.elf
write hello.txt Hello from AOS
cat hello.txt
```

Full walkthrough: **[docs/USAGE.md](docs/USAGE.md)**

---

## Documentation

| Document | Contents |
|----------|----------|
| [docs/USAGE.md](docs/USAGE.md) | **Detailed user guide** — install, build, QEMU, shell, FS, network, troubleshooting |
| [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) | Kernel layout, subsystems, boot flow |
| [docs/MEMORY.md](docs/MEMORY.md) | Demand paging, COW, VMAs, page faults |
| [docs/VFS.md](docs/VFS.md) | Filesystem facade, page cache, VFS contract |
| [docs/SYSCALLS.md](docs/SYSCALLS.md) | Syscall ABI for userspace programs |
| [docs/CORE_KERNEL.md](docs/CORE_KERNEL.md) | Scheduler, APIC, SMP, ring 3 notes |
| [CONTRIBUTING.md](CONTRIBUTING.md) | How to contribute |
| [CHANGELOG.md](CHANGELOG.md) | Version history |
| [REAL_HARDWARE.txt](REAL_HARDWARE.txt) | Notes for bare-metal experiments |

---

## Build options

```bash
make              # default: identity-mapped kernel, APIC on, SMP off
make HH=1         # higher-half kernel at 0xFFFFFFFF80100000
make SMP=1        # start application processors
make APIC=0       # legacy 8259 + PIT only
make check        # static checks + host tests
make clean
```

Each option combination uses its own `build/hh*_smp*_apic*/` directory.

---

## Project layout

```text
aos/
├── LICENSE / README.md / CONTRIBUTING.md / CHANGELOG.md
├── Makefile / linker.ld / grub.cfg
├── docs/                 # design + usage documentation
├── src/
│   ├── kernel/           # entry (main.cpp)
│   ├── arch/x86_64/      # GDT, IDT, APIC, interrupts
│   ├── memory/           # PMM, VMM, address spaces, heap
│   ├── process/          # ELF, tasks, syscalls, FDs
│   ├── filesystem/       # AOSFS, page cache, VFS headers
│   ├── scheduler/        # preemptive scheduler
│   ├── network/          # TCP/IP stack + NIC drivers
│   ├── drivers/          # storage, USB, input, audio, …
│   ├── shell/            # serial command interpreter
│   └── …
├── user/                 # freestanding user programs + libaos.h
├── tests/host/           # host-side unit tests
├── tools/                # check_kernel.py, selftest helpers
└── resources/initramfs/  # tiny boot archive
```

---

## Status & limitations

- Hobby / educational kernel — not a production OS
- AOSFS is simple (cluster chain; no full journaling yet)
- SMP and higher-half are optional and less exercised than the default build
- Networking is best-effort in QEMU user-net
- No GUI (headless serial)

Tested primarily under **QEMU** (x86_64). Real hardware notes: `REAL_HARDWARE.txt`.

---

## License

This project is released under the **MIT License** — see [LICENSE](LICENSE).

Copyright © 2025–2026 [aryanweblabs](https://github.com/aryanweblabs)

---

## Acknowledgements

Inspired by the long tradition of small open-source kernels and OSDev community resources. Built for learning, experimentation, and clean systems code.
