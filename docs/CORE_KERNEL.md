# Core kernel work: scheduler, APIC, ring 3, address spaces, higher half, SMP

Status of every item, honestly. **Nothing here has been booted**: the environment this was written in has no
QEMU, no GRUB tools and no nasm, so it could be compiled, linked, disassembled and unit-tested on the host, but
not executed on a CPU. Section 5 is the checklist for the first boot - please run it and report what you see.

## 1. What was built

| Item | Where | Default |
|---|---|---|
| Preemptive scheduler, 4 priority levels + idle, time slices, aging, sleep, kill, nice | `src/scheduler/task.cpp`, policy in `sched_policy.hpp` | on |
| Per-CPU data, GDT per CPU, TSS (RSP0 + IST stacks for #DF/NMI/#MC) | `src/arch/x86_64/gdt.cpp`, `percpu.hpp` | on |
| MADT/ACPI parsing, LAPIC (xAPIC and x2APIC), I/O APIC routing with interrupt overrides, LAPIC timer calibrated against PIT ch.2 | `src/arch/x86_64/apic.cpp` | on, **falls back to 8259+PIT** if anything fails |
| Ring 3: SYSCALL/SYSRET MSRs, `int 0x80`, iretq return path, swapgs discipline | `interrupts.S`, `gdt.cpp`, `process/syscall.cpp` | on |
| Per-process address spaces (own PML4, shared kernel half), user copy helpers, NX, SMEP | `src/memory/virtual/aspace.cpp` | on |
| **Demand paging + Copy-on-Write** (lazy anon, COW fork, VMA, refcounted frames, global zero page) | `src/memory/virtual/aspace.cpp`, `pmm.c`, `docs/MEMORY.md` | on |
| ELF64 loader (static ET_EXEC), process table, stdin line discipline, fault -> kill process | `src/process/` | on |
| Locks: IRQ-safe spinlocks in PMM and heap, recursive mutex around every `aos::fs` call | `sync.h`, `scheduler/mutex.hpp` | on |
| Higher-half kernel (`0xFFFFFFFF80100000`, identity map kept for drivers/DMA) | `entry.S`, `linker.ld` | **off** (`make HH=1`) |
| SMP: INIT-SIPI trampoline, per-CPU idle threads, APs run `TASK_SMP_OK` threads | `src/smp/` | **off** (`make SMP=1`) |

Also: all assembly moved from NASM to GNU `as` (`.S`, Intel syntax) so it can be built and checked with just binutils;
NIC drivers now convert static DMA buffers with `aos_virt_to_phys()` (needed for HH=1).

## 2. Design notes you need when extending it

* **Address-space layout.** `PML4[0]` = 0..4 GiB identity map (supervisor only; drivers and DMA rely on phys == virt),
  `PML4[1..255]` = user space `0x8000000000..0x7FFFFFFFFFFF`, `PML4[256..511]` = kernel. User programs are therefore linked at
  `0x8000000000` (`user/user.ld`); the loader rejects anything else. Kernel mappings are changed only through the
  master PML4 (`vmm::kernel_cr3()`); new top-level kernel slots are copied into live address spaces by `sync_kernel_slot`.
* **GS invariant.** In ring 0, `GS.base` = this CPU's `PerCpu`. Entering from ring 3 does `swapgs`, leaving does too.
* **Preemption.** The timer IRQ switches threads *inside* the interrupt handler on the interrupted thread's kernel stack.
  RFLAGS is part of the saved context, so a thread preempted in an IRQ resumes with IRQs masked and one that yielded
  resumes with them enabled. `interrupts.S` saves FXSAVE state for every interrupt, so C++ code may use SSE freely.
* **What may run on any thread:** pmm, kheap, `aos::fs`, serial output. **kmain only:** network stack, USB, HDA, I2C, PS/2
  (they are polled from `poll_all()` in `main.cpp` and are not thread-safe). Use `preempt_disable()` for your own critical sections.
* **kmain behaviour.** With no other threads, the idle path is the original `cli; sti; hlt` - no context switch at all.
  Once other threads exist, kmain sleeps one tick per loop so they get the CPU.
* **Kernel stacks** are 16 KiB with an unmapped guard page below (splits the 2 MiB identity page once).
* **Syscalls** (`process/syscall.hpp`): exit, write, read (line-buffered stdin), yield, sleep_ms, getpid, ticks, brk, mmap,
  spawn, wait, kill, fread, fwrite. User-side wrappers: `user/libaos.h`. Return is always `iretq`, never `sysret`.
* **Known limits:** no TLB shootdown IPIs (kernel mappings are static after boot, so it only matters for stack guard pages);
  no munmap; no fork; one thread per process; SMP APs only run threads created with `TASK_SMP_OK`; no reschedule IPIs
  (an idle AP notices new work on its next 11 ms tick); the 8259 is masked completely once the APIC takes over.

## 3. What was verified here, and how

| Check | Result |
|---|---|
| All 5 build configs (`HH`, `SMP`, `APIC` combinations) compile and link, no undefined symbols | pass |
| `tools/check_kernel.py`: Multiboot2 header < 32 KiB, entry is a low address, HH segments satisfy `virt = phys + 0xFFFFFFFF80000000` | pass (all configs) |
| Disassembly of the 32->64 boot path: page-table addresses, `PML4[511]`, `PDPT_high[510]`, GDT, far jump, `movabs` jump to the higher half | read and checked by hand |
| Disassembly of `interrupt_common`, `syscall_entry`, `aos_enter_user`, `aos_context_switch`, AP trampoline | read and checked by hand |
| `tests/host/core_test.cpp` (119 checks, ASan+UBSan): scheduler policy (fairness, priorities, aging, preemption latency, CPU pinning), MADT/IOAPIC/RSDP parsing incl. corrupt tables, ELF loader against the real user programs and 12 corruptions, address spaces (copies across pages, isolation, read-only pages, kernel-slot propagation, no frame leaks on teardown) | pass |

Not executable here, therefore **unverified**: context switching, the real LAPIC/IOAPIC programming sequence, SYSCALL/iretq to ring 3,
TSS/IST, the AP trampoline, GRUB loading the higher-half ELF.

## 4. Safety nets already in place
* APIC init is all-or-nothing and self-tests the timer (needs >= 2 ticks in 40 ms) before the 8259 is masked; on failure the legacy path is restored.
  PS/2 input still has its polling fallback if IOAPIC routing is wrong.
* A user-mode fault kills only that process (`[proc N 'name'] killed by exception ...` on serial). Kernel faults still panic with CPU/CS/RSP.
* If something misbehaves, rebuild with `make APIC=0` (legacy interrupts) or default `HH=0 SMP=0`; each combination has its own build directory.

## 5. First-boot checklist
**Automatic:** `python3 tools/selftest.py` (or `make selftest`) does all of the below for 4 configurations (default, legacy PIC,
higher half, 4-CPU SMP) and writes `selftest_report.zip` - send that file. `--quick` = default configuration only.
The driver itself was tested against a fake serial shell (`--fake`, all failure modes), never against a real kernel.

Manual version (QEMU: `make run`, serial on your terminal):
1. Boot log should contain `GDT/TSS + Heap + preemptive Scheduler OK`, `Interrupts OK (8259 + PIT)`, then either
   `APIC: LAPIC timer + I/O APIC active` or `APIC not used` + a `[apic] ...` reason line. Send me that reason line if it falls back.
2. `apic` and `cpus` - controller status; `uptime` must keep advancing (proves the LAPIC timer).
3. `tasks` - expect `kmain` and `idle0`.
4. `schedtest 3` - three busy threads that never yield. The shell must stay responsive; the two prio-2 counts should be within a few percent; prio-3 small but non-zero.
5. `usertest` - runs `hello.elf` (heap, mmap, argv), `counter.elf` (sleep + preempted busy loop, exit code 7), `fault.elf` (must be killed, kernel survives). Then `ps`.
6. `exec hello.elf a b c`, `run counter.elf` + `ps` + `kill <pid>`; Ctrl-C kills a foreground program.
7. `make HH=1 run`: same checks. If nothing prints at all, GRUB did not accept the higher-half ELF - tell me and use HH=0.
8. `make SMP=1 run-smp` (QEMU `-smp 4`): expect `[smp] application processors started: 03`, then `cpus` lists 4.
9. Real laptop: the APIC path matters most there (keyboard/touchpad IRQ routing); capture the `apic` command output.
