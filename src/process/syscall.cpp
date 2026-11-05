#include "syscall.hpp"
#include "process.hpp"
#include "fd.hpp"
#include "../arch/x86_64/frame.hpp"
#include "../arch/x86_64/interrupts.hpp"
#include "../filesystem/initramfs.hpp"
#include "../filesystem/ramfs.hpp"
#include "../memory/heap/kheap.hpp"
#include "../scheduler/task.hpp"

extern "C" void aos_serial_write(const char* text);

namespace aos::syscall {
namespace {
using aos::process::Process;
using aos::vmm::kPageSize;

// Generic read/write go through the process FD table.
int64_t sys_write(Process* p, uint64_t fd, uint64_t buf, uint64_t len) {
    if (len > (1u << 20)) len = 1u << 20;
    if (len == 0) return 0;
    // Copy user buffer in chunks to avoid a huge kernel allocation.
    char chunk[512];
    int64_t total = 0;
    while (len) {
        const uint64_t n = len > sizeof(chunk) ? sizeof(chunk) : len;
        if (!aos::vmm::as_copy_from(&p->as, chunk, buf, n))
            return total ? total : E_FAULT;
        const int64_t w = aos::process::fd_write(&p->fds, static_cast<int>(fd), chunk, n);
        if (w < 0) return total ? total : w;
        total += w;
        if (static_cast<uint64_t>(w) < n) break;   // short write
        buf += n;
        len -= n;
    }
    return total;
}

int64_t sys_read(Process* p, uint64_t fd, uint64_t buf, uint64_t len) {
    if (len > (1u << 20)) len = 1u << 20;
    if (len == 0) return 0;
    char chunk[512];
    int64_t total = 0;
    while (len) {
        const uint64_t n = len > sizeof(chunk) ? sizeof(chunk) : len;
        const int64_t r = aos::process::fd_read(&p->fds, static_cast<int>(fd), chunk, n);
        if (r < 0) return total ? total : r;
        if (r == 0) break;   // EOF
        if (!aos::vmm::as_copy_to(&p->as, buf, chunk, static_cast<size_t>(r)))
            return total ? total : E_FAULT;
        total += r;
        buf += static_cast<uint64_t>(r);
        len -= static_cast<uint64_t>(r);
        if (static_cast<uint64_t>(r) < n) break;   // short read
        // For stdin (line buffered) stop after first chunk.
        if (fd == 0) break;
    }
    return total;
}

int64_t sys_brk(Process* p, uint64_t addr) {
    if (addr == 0) return static_cast<int64_t>(p->brk);
    if (addr < aos::process::kHeapBase || addr - aos::process::kHeapBase > aos::process::kMaxHeapBytes)
        return static_cast<int64_t>(p->brk);
    const uint64_t have = (p->brk + kPageSize - 1) & ~(kPageSize - 1);
    const uint64_t want = (addr + kPageSize - 1) & ~(kPageSize - 1);
    if (want > have && !aos::vmm::as_alloc(&p->as, have, (want - have) / kPageSize,
                                           aos::vmm::PF_WRITE | aos::vmm::PF_NX))
        return static_cast<int64_t>(p->brk);
    if (addr > p->brk) p->brk = addr;
    return static_cast<int64_t>(p->brk);
}

int64_t sys_mmap(Process* p, uint64_t len) {
    if (!len || len > aos::process::kMaxHeapBytes) return E_INVAL;
    const uint64_t pages = (len + kPageSize - 1) / kPageSize;
    const uint64_t at = p->mmap_next;
    if (at + pages * kPageSize >= aos::process::kStackTop - aos::process::kStackPages * kPageSize)
        return E_NOMEM;
    if (!aos::vmm::as_alloc(&p->as, at, pages, aos::vmm::PF_WRITE | aos::vmm::PF_NX)) return E_NOMEM;
    p->mmap_next = at + pages * kPageSize;
    return static_cast<int64_t>(at);
}

int64_t sys_munmap(Process* p, uint64_t addr, uint64_t len) {
    if (!len) return 0;
    // Round len up to page boundary; addr must already be aligned.
    const uint64_t pages = (len + kPageSize - 1) / kPageSize;
    len = pages * kPageSize;
    if ((addr & (kPageSize - 1)) || !aos::vmm::user_range_ok(addr, len)) return E_INVAL;
    if (!aos::vmm::as_munmap(&p->as, addr, len)) return E_INVAL;
    return 0;
}

// prot bits from userspace: bit0=READ (ignored, always on), bit1=WRITE, bit2=EXEC
int64_t sys_mprotect(Process* p, uint64_t addr, uint64_t len, uint64_t prot) {
    if (!len) return 0;
    const uint64_t pages = (len + kPageSize - 1) / kPageSize;
    len = pages * kPageSize;
    if ((addr & (kPageSize - 1)) || !aos::vmm::user_range_ok(addr, len)) return E_INVAL;
    uint64_t flags = aos::vmm::PF_NX;          // default: not executable
    if (prot & 2) flags |= aos::vmm::PF_WRITE; // PROT_WRITE
    if (prot & 4) flags &= ~aos::vmm::PF_NX;   // PROT_EXEC
    if (!aos::vmm::as_mprotect(&p->as, addr, len, flags)) return E_INVAL;
    return 0;
}

int64_t sys_spawn(Process* p, uint64_t path_va, uint64_t args_va) {
    char path[64], args[128] = {0};
    if (!aos::vmm::as_copy_string(&p->as, path, path_va, sizeof(path))) return E_FAULT;
    if (args_va && !aos::vmm::as_copy_string(&p->as, args, args_va, sizeof(args))) return E_FAULT;
    const char* err = nullptr;
    const int pid = aos::process::spawn_path(path, args, &err);
    if (pid < 0) return err && err[0] == 'n' && err[1] == 'o' && err[2] == 't' ? E_NOENT : E_INVAL;
    return pid;
}

int64_t sys_fread(Process* p, uint64_t path_va, uint64_t buf, uint64_t max) {
    char path[64];
    if (!aos::vmm::as_copy_string(&p->as, path, path_va, sizeof(path))) return E_FAULT;
    if (max > aos::fs::CONTENT_MAX) max = aos::fs::CONTENT_MAX;
    void* tmp = aos::heap::allocate(aos::fs::CONTENT_MAX);
    if (!tmp) return E_NOMEM;
    size_t n = 0;
    int64_t r = 0;
    if (aos::fs::read_into(path, tmp, max, &n) != aos::fs::Result::Ok) r = E_NOENT;
    else if (!aos::vmm::as_copy_to(&p->as, buf, tmp, n)) r = E_FAULT;
    else r = static_cast<int64_t>(n);
    aos::heap::release(tmp);
    return r;
}

int64_t sys_fwrite(Process* p, uint64_t path_va, uint64_t buf, uint64_t len) {
    char path[64];
    if (!aos::vmm::as_copy_string(&p->as, path, path_va, sizeof(path))) return E_FAULT;
    if (len > aos::fs::CONTENT_MAX) return E_INVAL;
    void* tmp = aos::heap::allocate(len ? len : 1);
    if (!tmp) return E_NOMEM;
    int64_t r = 0;
    if (!aos::vmm::as_copy_from(&p->as, tmp, buf, len)) r = E_FAULT;
    else if (aos::fs::write(path, tmp, len) != aos::fs::Result::Ok) r = E_PERM;
    aos::heap::release(tmp);
    return r;
}

int64_t sys_open(Process* p, uint64_t path_va, uint64_t flags) {
    char path[64];
    if (!aos::vmm::as_copy_string(&p->as, path, path_va, sizeof(path))) return E_FAULT;
    return aos::process::fd_open(&p->fds, path, static_cast<uint32_t>(flags));
}

int64_t sys_close(Process* p, uint64_t fd) {
    return aos::process::fd_close(&p->fds, static_cast<int>(fd));
}

int64_t sys_pipe(Process* p, uint64_t fds_va) {
    int pair[2] = {-1, -1};
    const int64_t r = aos::process::fd_pipe(&p->fds, pair);
    if (r < 0) return r;
    if (!aos::vmm::as_copy_to(&p->as, fds_va, pair, sizeof(pair))) {
        aos::process::fd_close(&p->fds, pair[0]);
        aos::process::fd_close(&p->fds, pair[1]);
        return E_FAULT;
    }
    return 0;
}

int64_t sys_dup2(Process* p, uint64_t oldfd, uint64_t newfd) {
    return aos::process::fd_dup2(&p->fds, static_cast<int>(oldfd), static_cast<int>(newfd));
}

int64_t sys_lseek(Process* p, uint64_t fd, uint64_t offset, uint64_t whence) {
    return aos::process::fd_seek(&p->fds, static_cast<int>(fd),
                                 static_cast<int64_t>(offset), static_cast<int>(whence));
}
}  // namespace

void dispatch(InterruptFrame* f) {
    Process* p = aos::process::current();
    if (!p) { f->rax = static_cast<uint64_t>(E_NOSYS); return; }
    const uint64_t a0 = f->rdi, a1 = f->rsi, a2 = f->rdx;
    int64_t r = E_NOSYS;
    switch (f->rax) {
        case SYS_EXIT:     aos::process::exit_current(static_cast<int>(a0));
        case SYS_WRITE:    r = sys_write(p, a0, a1, a2); break;
        case SYS_READ:     r = sys_read(p, a0, a1, a2); break;
        case SYS_YIELD:    aos::scheduler::yield(); r = 0; break;
        case SYS_SLEEP_MS: {
            uint64_t t = (a0 * 90 + 999) / 1000;
            while (t && !p->killed) {
                aos::scheduler::sleep_current(t > 9 ? 9 : t);
                t = t > 9 ? t - 9 : 0;
            }
            r = 0; break;
        }
        case SYS_GETPID:   r = p->pid; break;
        case SYS_TICKS:    r = static_cast<int64_t>(timer_ticks()); break;
        case SYS_BRK:      r = sys_brk(p, a0); break;
        case SYS_MMAP:     r = sys_mmap(p, a0); break;
        case SYS_SPAWN:    r = sys_spawn(p, a0, a1); break;
        case SYS_WAIT: {
            int code = 0;
            if (!aos::process::wait_for(static_cast<uint32_t>(a0), &code, true)) { r = E_NOENT; break; }
            if (a1 && !aos::vmm::as_copy_to(&p->as, a1, &code, sizeof(code))) { r = E_FAULT; break; }
            r = static_cast<int64_t>(a0);
            break;
        }
        case SYS_KILL:     r = aos::process::kill(static_cast<uint32_t>(a0)) ? 0 : E_NOENT; break;
        case SYS_FREAD:    r = sys_fread(p, a0, a1, a2); break;
        case SYS_FWRITE:   r = sys_fwrite(p, a0, a1, a2); break;
        case SYS_OPEN:     r = sys_open(p, a0, a1); break;
        case SYS_CLOSE:    r = sys_close(p, a0); break;
        case SYS_PIPE:     r = sys_pipe(p, a0); break;
        case SYS_DUP2:     r = sys_dup2(p, a0, a1); break;
        case SYS_LSEEK:    r = sys_lseek(p, a0, a1, a2); break;
        case SYS_FORK:     r = aos::process::fork_from(f); break;
        case SYS_MUNMAP:   r = sys_munmap(p, a0, a1); break;
        case SYS_MPROTECT: r = sys_mprotect(p, a0, a1, a2); break;
        default: break;
    }
    f->rax = static_cast<uint64_t>(r);
    aos::process::check_kill();
}
}  // namespace aos::syscall
