# AOS Architecture

High-level map of the headless AOS kernel. For deep dives see [MEMORY.md](MEMORY.md), [VFS.md](VFS.md), and [CORE_KERNEL.md](CORE_KERNEL.md).

## Boot flow

```text
Power / QEMU
    → GRUB (Multiboot2 ISO)
    → entry.S  (32→64-bit, page tables, stack)
    → kernel_main (main.cpp)
         → PMM, heap, VMM
         → GDT/TSS, interrupts, APIC (or PIC)
         → scheduler
         → initramfs + AOSFS mount
         → drivers (storage, net, input, …)
         → shell on serial
```

## Major subsystems

```text
┌─────────────────────────────────────────────────────────┐
│                     Serial shell                         │
└──────────────────────────┬──────────────────────────────┘
                           │
┌──────────────────────────▼──────────────────────────────┐
│  Process manager  │  Syscalls  │  ELF loader  │  FDs     │
└─────────┬───────────────────┬───────────────────────────┘
          │                   │
┌─────────▼─────────┐  ┌──────▼──────────────────────────┐
│  Scheduler        │  │  VMM / Address spaces            │
│  (preemptive)     │  │  demand paging · COW · VMAs      │
└─────────┬─────────┘  └──────┬───────────────────────────┘
          │                   │
┌─────────▼───────────────────▼───────────────────────────┐
│  Arch: GDT · IDT · APIC/PIC · per-CPU · SMP trampoline  │
└─────────────────────────────────────────────────────────┘

┌──────────────────┐    ┌──────────────────┐
│  aos::fs facade  │───►│  AOSFS + page    │──► Block devices
│                  │    │  cache           │    (ATA/NVMe/…)
└──────────────────┘    └──────────────────┘

┌──────────────────┐
│  Network stack   │──► e1000 / pcnet
└──────────────────┘
```

## Address space layout (user)

| Region | Address (typical) |
|--------|-------------------|
| User code/data | `0x0000_0080_0000_0000` (`kUserBase`) |
| Heap / brk | `0x0000_0090_0000_0000` |
| mmap region | `0x0000_00A0_0000_0000` |
| Stack (grows down) | below `0x0000_00B0_0000_0000` |
| Kernel half | PML4[256..] shared across processes |

## Source map

| Path | Role |
|------|------|
| `src/kernel/main.cpp` | Boot orchestration |
| `src/arch/x86_64/` | CPU tables, interrupts, APIC |
| `src/memory/` | PMM, heap, VMM, aspace |
| `src/process/` | Processes, ELF, syscalls |
| `src/scheduler/` | Threads / preemption |
| `src/filesystem/` | AOSFS, page cache, VFS headers |
| `src/network/` | TCP/IP + drivers |
| `src/shell/` | Interactive shell |
| `user/` | Freestanding programs + `libaos.h` |

## Design principles

1. **Clarity over cleverness** — readable freestanding C++17  
2. **Safe defaults** — ring 3 userspace, demand paging, COW fork  
3. **Observable** — serial logs, `mem`, `cache`, `tasks`  
4. **Honest scope** — headless hobby kernel, not a Unix clone  

## Optional build features

| Flag | Effect |
|------|--------|
| `HH=1` | Higher-half kernel link address |
| `SMP=1` | Bring up APs |
| `APIC=0` | Force legacy PIC + PIT |
