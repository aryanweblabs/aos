# AOS Virtual File System & Page Cache

## Overview

This document describes the filesystem architecture after the VFS / page-cache work.

```
  User programs / Shell
            │
            ▼
     aos::fs  (facade, RecursiveMutex)
            │
     ┌──────┴──────┐
     ▼             ▼
  aos::aosfs    initramfs
  (disk)       (boot RAM)
     │
     ▼
  page_cache   ◄── CLOCK, 512 slots × 512 B = 256 KiB
     │
     ▼
  BlockDevice (ATA / NVMe / VirtIO / AHCI)
```

## Page cache (`src/filesystem/page_cache.*`)

| Property | Value |
|----------|--------|
| Granularity | 512-byte cluster (matches AOSFS) |
| Capacity | 512 slots (256 KiB) |
| Eviction | CLOCK (second-chance) |
| Key | `(dev_id, cluster)` |
| Coherence | Write-through + invalidate on unmount/format |

### API

```cpp
bool get(dev_id, cluster, out);     // hit → copy to out
bool put(dev_id, cluster, data);    // insert / update
void invalidate(dev_id, cluster);
void invalidate_device(dev_id);
void stats(Stats*);
```

### Integration

- `aosfs::read_cluster` — try cache first; on miss read device and `put`.
- `aosfs::write_cluster` — write device, then `put` (write-through).
- `aosfs::unmount` / `format` — `invalidate_device`.

### Shell

```
aos:/> cache
Page cache
  capacity : 512 slots (256 KiB)
  used     : …
  hits     : …
  misses   : …
  hit rate : …%
```

## VFS layer (`src/filesystem/vfs.hpp`)

Formal contract for future multi-mount support:

- `Inode` — lightweight inode view (`ino`, `size`, `mode`, `fsid`)
- `FileSystemOps` — function table (`lookup`, `read`, `write`, `create`, `unlink`, …)
- `Mount` — mount-point record

Today a single AOSFS instance is mounted through `aos::fs::init()`.  
New filesystems should implement `FileSystemOps` and register a `Mount`.

## Design choices

1. **Cluster-sized cache entries** — AOSFS already uses 512 B clusters; no read-modify-write amplification.
2. **Write-through** — simple coherence, no dirty-page writeback path required yet.
3. **CLOCK instead of full LRU** — O(1) eviction, good enough for 512 slots.
4. **Stable `aos::fs` API** — shell and syscalls unchanged; cache is transparent.
5. **Explicit stats** — `cache` command makes hits/misses observable during testing.

## Testing checklist

- [ ] Boot with AOSFS mounted
- [ ] `write test.txt hello` then `cat test.txt` twice → second `cat` increases hits
- [ ] `cache` shows non-zero hits after repeated reads
- [ ] `format CONFIRM` / remount → cache invalidated (no stale data)
- [ ] `vmtest` / `forkdemo` still pass (cache must not break existing paths)

## Future work

- File-backed `mmap` (VMA + page cache pages)
- 4 KiB multi-cluster pages for larger sequential reads
- Write-back mode with explicit `sync` flushing dirty cache pages
- Multiple simultaneous mounts (`/mnt/...`)
- dentry cache for path lookup
