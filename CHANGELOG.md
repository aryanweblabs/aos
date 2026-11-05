# Changelog

All notable changes to **AOS** (Aryan OS / AOS Enhanced) are documented here.

## [0.5.0] — 2026-11-05

### Added
- Demand paging for anonymous memory (`mmap` / stack / heap regions)
- Copy-on-write (COW) `fork`
- `munmap` and `mprotect` syscalls + VMA split/merge logic
- Page cache for AOSFS (256 KiB CLOCK cache, write-through)
- VFS contract headers (`vfs.hpp`) and design docs
- Userspace test suite: `vmtest.elf`
- Shell command: `cache` (page-cache statistics)
- Open-source packaging: LICENSE (MIT), docs, CONTRIBUTING, CI workflow

### Changed
- Headless-only build (Carbon desktop / GUI removed)
- `as_clone` shares pages under COW instead of deep-copy
- ELF loader populates demand pages safely for read-only segments

### Fixed
- COW bit not set on child PTEs (fork stack write fault)
- Loader force-write into read-only demand pages

## [0.4.x] — prior work

- Preemptive priority scheduler, APIC/LAPIC, ring 3 + SYSCALL
- Per-process address spaces, ELF loader
- AOSFS persistent filesystem (ATA/NVMe)
- Networking stack (e1000/pcnet, DHCP, TCP/UDP)
- Optional higher-half kernel and SMP
