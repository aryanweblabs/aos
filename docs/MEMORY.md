# AOS Virtual Memory: Demand Paging + Copy-on-Write

**Status:** Implemented (headless kernel v5+)  
**Goal:** Turn the previous eager, deep-copy address spaces into a modern, efficient virtual memory system while keeping the API stable and the code clear.

## 1. Problems with the previous design

| Old behaviour | Cost |
|---|---|
| `as_alloc` immediately allocated + zeroed every page | Large `mmap` / stack / heap wasted RAM until first touch |
| `as_clone` (fork) deep-copied every user page | Fork of a 100 MiB process cost 100 MiB + a lot of time |
| No page-fault handling for memory | Any not-present access killed the process |
| No sharing | Impossible to implement efficient fork, shared libraries later, or a global zero page |

## 2. Design principles

1. **Lazy is the default.** Physical frames are allocated only when a user process first touches a page (demand paging).
2. **Share until write.** `fork` shares pages; a write causes a private copy (classic COW).
3. **Explicit regions.** Every valid user mapping belongs to a VMA (Virtual Memory Area). A page fault outside any VMA is still a hard fault.
4. **Software PTE bits.** We use the x86 “available” bits 9–11 that the CPU ignores:
   - bit 9  → `PF_COW`     (page is copy-on-write)
   - bit 10 → `PF_DEMAND`  (not yet allocated; present bit is clear)
5. **Reference counting.** Shared pages (after COW fork or future shared mappings) are tracked with a per-frame refcount in the PMM.
6. **All-or-nothing & rollback.** Allocation failures never leave partial state.

## 3. Data structures

### 3.1 Page-table entry flags (software)

```text
bit 0   Present          (hardware)
bit 1   Writable         (hardware)
bit 2   User             (hardware)
bit 9   PF_COW           (OS) – shared, must be made private on write
bit 10  PF_DEMAND        (OS) – not present yet; allocate on first access
bit 63  NX               (hardware)
```

A demand page is stored as:

```text
PTE = 0 | PF_DEMAND | (optional protection hints in other free bits)
```

A COW page is a normal present page with the Writable bit **cleared** and `PF_COW` set.

### 3.2 VMA

```cpp
struct Vma {
    uint64_t start;     // inclusive, page-aligned
    uint64_t end;       // exclusive, page-aligned
    uint64_t prot;      // PF_WRITE | PF_NX  (read is always implied for user)
    uint32_t flags;     // VMA_ANON | … (future: VMA_FILE, VMA_SHARED, …)
};
```

Each `AddressSpace` holds a small fixed array (currently 32) of VMAs.  
This is enough for the current loader (text, rodata, data/bss, stack, heap/brk, a few mmaps).

### 3.3 Frame reference counts

PMM now keeps a `uint16_t refcount[MAX_FRAMES]` (or a compact sparse structure).  
- `pmm_alloc_frame()` → refcount = 1  
- `pmm_inc_ref(phys)` / `pmm_dec_ref(phys)`  
- When refcount reaches 0 the frame is returned to the free list.

## 4. Operations

### 4.1 `as_alloc(as, va, npages, prot)`  (now lazy)

1. Check the range is free (no overlapping VMA).
2. Install a new VMA with the requested protection.
3. Walk the page tables and install **demand** PTEs (`PF_DEMAND`, Present = 0).  
   No physical memory is consumed.
4. Return success.

### 4.2 `as_clone(src, dst)`  (COW fork)

1. Create a fresh address space for `dst` (copies kernel half of PML4).
2. Deep-copy the VMA list.
3. Walk every present user PTE in `src`:
   - Clear the Writable bit.
   - Set `PF_COW`.
   - Increment the frame’s refcount.
   - Install the same PTE (now RO+COW) into `dst`.
4. Demand PTEs are simply copied (still demand).
5. Both processes now share the physical pages.

### 4.3 Page-fault handler (`handle_user_page_fault`)

Called from the #PF path **before** the process is killed.

```
CR2 = fault address
error code = hardware PF error (present, write, user, …)

1. Locate VMA covering CR2.  If none → real fault → kill.
2. If PTE is demand (PF_DEMAND or !Present):
     - If write fault and VMA not writable → kill.
     - Allocate a zeroed frame (or the global zero page + COW).
     - Map it with the VMA’s protection (Writable only if VMA allows).
     - Return “handled”.
3. If PTE is present + PF_COW + write fault:
     - If refcount(frame) == 1:
         just set Writable, clear PF_COW.
     - Else:
         allocate new frame, copy contents, map new frame Writable,
         decrement old refcount, clear PF_COW on the new PTE.
     - Return “handled”.
4. Any other case → real fault → kill.
```

### 4.4 `as_unmap` / `as_destroy`

- Decrement refcounts (or free if last).
- Remove corresponding VMA entries.
- Free page-table pages that become empty (existing logic).

## 5. Global zero page (optimisation)

A single physical page filled with zeros is allocated at boot.  
Demand-zero faults that are **reads** can map this page COW (read-only).  
The first write then triggers a normal COW break.  
This saves a huge amount of memory for large BSS / heap regions that are never written.

## 6. Integration points

| Location | Change |
|---|---|
| `src/memory/physical/pmm.*` | refcount array + inc/dec/get |
| `src/memory/virtual/aspace.*` | VMAs, demand PTEs, COW clone, fault handler |
| `src/arch/x86_64/interrupts.cpp` | vector 14 + user mode → try `handle_user_page_fault` first |
| `src/process/process.cpp` | no major change (fork still calls `as_clone`) |
| ELF loader / `sys_mmap` / `sys_brk` | continue to call `as_alloc`; they automatically become lazy |

## 7. Testing checklist

- [ ] Boot, run `hello` (no change in behaviour)
- [ ] `forkdemo` – child and parent both run, memory usage stays low
- [ ] Large `mmap` / `brk` that is never touched → free frames almost unchanged
- [ ] Write to a COW page in child → parent’s page stays unchanged
- [ ] Write to a demand page → page appears, content zero
- [ ] Stack growth (if implemented) or large stack allocation works
- [ ] Out-of-memory on fault → process killed cleanly, kernel survives
- [ ] `as_destroy` returns all frames (refcount reaches zero)

## 8. Future extensions (not in this change)

- File-backed VMAs + page cache
- Shared mappings (`MAP_SHARED`)
- Huge pages (2 MiB) for demand regions
- Swap
- Soft page faults / NUMA
- Userfaultfd-style interfaces

---

This design keeps the kernel small, the code readable, and the behaviour predictable while delivering the two classic benefits of modern virtual memory: **cheap fork** and **lazy allocation**.
