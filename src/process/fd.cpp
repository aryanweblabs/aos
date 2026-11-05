// fd.cpp - file descriptor table, AOSFS-backed files, and anonymous pipes.
#include "fd.hpp"
#include "process.hpp"
#include "../filesystem/ramfs.hpp"
#include "../memory/heap/kheap.hpp"
#include "../scheduler/task.hpp"
#include "../arch/x86_64/sync.h"

extern "C" void aos_serial_write(const char* text);

namespace aos::process {
namespace {

constexpr int64_t E_BADF  = -9;
constexpr int64_t E_INVAL = -22;
constexpr int64_t E_NOMEM = -12;
constexpr int64_t E_NOENT = -2;
constexpr int64_t E_ACCES = -13;
constexpr int64_t E_PIPE  = -32;   // EPIPE
constexpr int64_t E_AGAIN = -11;

aos_spinlock_t pipe_lock = AOS_SPINLOCK_INIT;
Pipe pipe_pool[32];

Pipe* pipe_alloc() {
    const uint64_t f = aos_spin_lock_irqsave(&pipe_lock);
    Pipe* p = nullptr;
    for (auto& slot : pipe_pool) {
        if (!slot.in_use) {
            slot = {};
            slot.in_use = true;
            p = &slot;
            break;
        }
    }
    aos_spin_unlock_irqrestore(&pipe_lock, f);
    return p;
}

void pipe_release(Pipe* p) {
    if (!p) return;
    const uint64_t f = aos_spin_lock_irqsave(&pipe_lock);
    if (p->readers == 0 && p->writers == 0) {
        p->in_use = false;
        p->len = p->head = p->tail = 0;
    }
    aos_spin_unlock_irqrestore(&pipe_lock, f);
}

int alloc_slot(FdTable* t) {
    for (unsigned i = 0; i < kMaxFds; ++i)
        if (t->fds[i].kind == FdKind::Free) return static_cast<int>(i);
    return -1;
}

void clear_slot(FileDesc& d) {
    d = {};
    d.kind = FdKind::Free;
}

// Copy path, reject empty / too long.
bool copy_path(char* dst, const char* src, size_t max) {
    if (!src || !src[0]) return false;
    size_t i = 0;
    while (src[i] && i + 1 < max) { dst[i] = src[i]; ++i; }
    if (src[i]) return false;
    dst[i] = 0;
    return true;
}

}  // namespace

void fd_init_std(FdTable* t) {
    for (unsigned i = 0; i < kMaxFds; ++i) clear_slot(t->fds[i]);
    t->fds[0].kind = FdKind::Stdin;
    t->fds[1].kind = FdKind::Stdout;
    t->fds[2].kind = FdKind::Stderr;
}

void fd_close_all(FdTable* t) {
    for (unsigned i = 0; i < kMaxFds; ++i) {
        if (t->fds[i].kind == FdKind::Free) continue;
        fd_close(t, static_cast<int>(i));
    }
}

void fd_clone(const FdTable* src, FdTable* dst) {
    fd_init_std(dst);
    for (unsigned i = 0; i < kMaxFds; ++i) {
        const FileDesc& s = src->fds[i];
        if (s.kind == FdKind::Free) continue;
        FileDesc& d = dst->fds[i];
        d = s;
        if (s.kind == FdKind::PipeRead || s.kind == FdKind::PipeWrite) {
            if (s.pipe) {
                const uint64_t f = aos_spin_lock_irqsave(&pipe_lock);
                if (s.kind == FdKind::PipeRead)  ++s.pipe->readers;
                if (s.kind == FdKind::PipeWrite) ++s.pipe->writers;
                aos_spin_unlock_irqrestore(&pipe_lock, f);
            }
        }
    }
}

int64_t fd_open(FdTable* t, const char* path, uint32_t flags) {
    if (!path || !path[0]) return E_INVAL;
    const int slot = alloc_slot(t);
    if (slot < 0) return E_NOMEM;

    const bool want_read  = (flags & 3) != O_WRONLY;
    const bool want_write = (flags & 3) != O_RDONLY;
    const bool creat      = (flags & O_CREAT) != 0;
    const bool trunc      = (flags & O_TRUNC) != 0;

    // Probe existence. Use a 1-byte scratch so read_into never sees a null buffer.
    uint8_t scratch = 0;
    size_t existing = 0;
    const auto rr = aos::fs::read_into(path, &scratch, 1, &existing);
    const bool exists = (rr == aos::fs::Result::Ok);

    if (!exists) {
        if (!creat || !want_write) return E_NOENT;
        // create empty file
        if (aos::fs::write(path, "", 0) != aos::fs::Result::Ok)
            return E_ACCES;
    } else if (trunc && want_write) {
        if (aos::fs::write(path, "", 0) != aos::fs::Result::Ok) return E_ACCES;
    }

    FileDesc& d = t->fds[slot];
    clear_slot(d);
    d.kind = FdKind::File;
    d.flags = flags;
    d.offset = (flags & O_APPEND) ? 0xFFFFFFFFu : 0;  // append: seek to end on each write
    if (!copy_path(d.path, path, sizeof(d.path))) {
        clear_slot(d);
        return E_INVAL;
    }
    (void)want_read;
    return slot;
}

int64_t fd_close(FdTable* t, int fd) {
    if (fd < 0 || static_cast<unsigned>(fd) >= kMaxFds) return E_BADF;
    FileDesc& d = t->fds[fd];
    if (d.kind == FdKind::Free) return E_BADF;

    if (d.kind == FdKind::PipeRead || d.kind == FdKind::PipeWrite) {
        Pipe* p = d.pipe;
        if (p) {
            const uint64_t f = aos_spin_lock_irqsave(&pipe_lock);
            if (d.kind == FdKind::PipeRead  && p->readers)  --p->readers;
            if (d.kind == FdKind::PipeWrite && p->writers)  --p->writers;
            const bool dead = (p->readers == 0 && p->writers == 0);
            aos_spin_unlock_irqrestore(&pipe_lock, f);
            if (dead) pipe_release(p);
        }
    }
    clear_slot(d);
    return 0;
}

int64_t fd_read(FdTable* t, int fd, void* buf, size_t len) {
    if (fd < 0 || static_cast<unsigned>(fd) >= kMaxFds || !buf) return E_BADF;
    if (len == 0) return 0;
    FileDesc& d = t->fds[fd];

    switch (d.kind) {
    case FdKind::Stdin: {
        // Line-buffered console input already implemented for user programs.
        char tmp[1024];
        if (len > sizeof(tmp)) len = sizeof(tmp);
        const long n = stdin_read(tmp, len);
        if (n < 0) return E_ACCES;
        if (n == 0) return 0;
        for (long i = 0; i < n; ++i) static_cast<char*>(buf)[i] = tmp[i];
        return n;
    }
    case FdKind::File: {
        if ((d.flags & 3) == O_WRONLY) return E_ACCES;
        // Kernel heap only supports <= ~4 KiB objects. Read the file in a
        // modest scratch buffer and copy out the requested window.
        constexpr size_t kScratch = 2048;
        uint8_t scratch[kScratch];
        size_t total = 0;
        const auto r = aos::fs::read_into(d.path, scratch, kScratch, &total);
        if (r != aos::fs::Result::Ok) return E_NOENT;
        // read_into reports true size in total even when truncated to kScratch;
        // for the bytes we actually have in scratch, usable = min(total, kScratch).
        const size_t have = total < kScratch ? total : kScratch;
        if (d.offset >= have) {
            // Offset past what we loaded: if file is larger than scratch we cannot
            // stream further without a better FS API — treat as EOF for now when
            // offset is beyond the on-disk size reported.
            if (d.offset >= total) return 0;
            return E_NOMEM;  // need multi-window read (not yet)
        }
        size_t avail = have - d.offset;
        if (avail > len) avail = len;
        for (size_t i = 0; i < avail; ++i)
            static_cast<uint8_t*>(buf)[i] = scratch[d.offset + i];
        d.offset += static_cast<uint32_t>(avail);
        return static_cast<int64_t>(avail);
    }
    case FdKind::PipeRead: {
        Pipe* p = d.pipe;
        if (!p) return E_BADF;
        // Spin/sleep until data arrives or all writers are gone.
        for (;;) {
            const uint64_t f = aos_spin_lock_irqsave(&pipe_lock);
            if (p->len > 0) {
                size_t n = p->len < len ? p->len : len;
                uint8_t* out = static_cast<uint8_t*>(buf);
                for (size_t i = 0; i < n; ++i) {
                    out[i] = p->data[p->head];
                    p->head = (p->head + 1) % kPipeCapacity;
                }
                p->len -= static_cast<uint32_t>(n);
                aos_spin_unlock_irqrestore(&pipe_lock, f);
                return static_cast<int64_t>(n);
            }
            if (p->writers == 0) {
                aos_spin_unlock_irqrestore(&pipe_lock, f);
                return 0;   // EOF
            }
            aos_spin_unlock_irqrestore(&pipe_lock, f);
            aos::scheduler::sleep_current(1);   // ~11 ms
            if (current() && current()->killed) return E_ACCES;
        }
    }
    default:
        return E_BADF;
    }
}

int64_t fd_write(FdTable* t, int fd, const void* buf, size_t len) {
    if (fd < 0 || static_cast<unsigned>(fd) >= kMaxFds || (!buf && len)) return E_BADF;
    if (len == 0) return 0;
    FileDesc& d = t->fds[fd];

    switch (d.kind) {
    case FdKind::Stdout:
    case FdKind::Stderr: {
        // Chunked serial write (same as old sys_write).
        const char* s = static_cast<const char*>(buf);
        char chunk[129];
        size_t off = 0;
        while (off < len) {
            size_t n = len - off > 128 ? 128 : len - off;
            for (size_t i = 0; i < n; ++i) chunk[i] = s[off + i];
            chunk[n] = 0;
            aos_serial_write(chunk);
            off += n;
        }
        return static_cast<int64_t>(len);
    }
    case FdKind::File: {
        if ((d.flags & 3) == O_RDONLY) return E_ACCES;
        if (len > aos::fs::CONTENT_MAX) return E_INVAL;

        if (d.flags & O_APPEND) {
            // Append path.
            if (aos::fs::append(d.path, buf, len) != aos::fs::Result::Ok) return E_ACCES;
            return static_cast<int64_t>(len);
        }

        // Full rewrite from offset 0 (typical open+write pattern).
        if (d.offset == 0 && !(d.flags & O_APPEND)) {
            if (aos::fs::write(d.path, buf, len) != aos::fs::Result::Ok) return E_ACCES;
            d.offset = static_cast<uint32_t>(len);
            return static_cast<int64_t>(len);
        }
        // Mid-file random write would need a large RMW buffer; not supported yet.
        return E_INVAL;
    }
    case FdKind::PipeWrite: {
        Pipe* p = d.pipe;
        if (!p) return E_BADF;
        const uint8_t* src = static_cast<const uint8_t*>(buf);
        size_t written = 0;
        while (written < len) {
            const uint64_t f = aos_spin_lock_irqsave(&pipe_lock);
            if (p->readers == 0) {
                aos_spin_unlock_irqrestore(&pipe_lock, f);
                return written ? static_cast<int64_t>(written) : E_PIPE;
            }
            if (p->len < kPipeCapacity) {
                size_t space = kPipeCapacity - p->len;
                size_t n = (len - written) < space ? (len - written) : space;
                for (size_t i = 0; i < n; ++i) {
                    p->data[p->tail] = src[written + i];
                    p->tail = (p->tail + 1) % kPipeCapacity;
                }
                p->len += static_cast<uint32_t>(n);
                written += n;
                aos_spin_unlock_irqrestore(&pipe_lock, f);
                continue;
            }
            aos_spin_unlock_irqrestore(&pipe_lock, f);
            aos::scheduler::sleep_current(1);
            if (current() && current()->killed) return E_ACCES;
        }
        return static_cast<int64_t>(written);
    }
    default:
        return E_BADF;
    }
}

int64_t fd_pipe(FdTable* t, int out_fds[2]) {
    if (!out_fds) return E_INVAL;
    Pipe* p = pipe_alloc();
    if (!p) return E_NOMEM;
    p->readers = 1;
    p->writers = 1;

    const int r = alloc_slot(t);
    if (r < 0) { p->readers = p->writers = 0; pipe_release(p); return E_NOMEM; }
    // Claim the read slot immediately so the next alloc_slot cannot reuse it.
    t->fds[r].kind = FdKind::PipeRead;
    t->fds[r].pipe = p;
    t->fds[r].flags = O_RDONLY;

    const int w = alloc_slot(t);
    if (w < 0) {
        clear_slot(t->fds[r]);
        p->readers = p->writers = 0;
        pipe_release(p);
        return E_NOMEM;
    }
    t->fds[w].kind = FdKind::PipeWrite;
    t->fds[w].pipe = p;
    t->fds[w].flags = O_WRONLY;

    out_fds[0] = r;
    out_fds[1] = w;
    return 0;
}

int64_t fd_dup2(FdTable* t, int oldfd, int newfd) {
    if (oldfd < 0 || static_cast<unsigned>(oldfd) >= kMaxFds) return E_BADF;
    if (newfd < 0 || static_cast<unsigned>(newfd) >= kMaxFds) return E_BADF;
    if (oldfd == newfd) return newfd;
    FileDesc& src = t->fds[oldfd];
    if (src.kind == FdKind::Free) return E_BADF;

    if (t->fds[newfd].kind != FdKind::Free)
        fd_close(t, newfd);

    t->fds[newfd] = src;
    if (src.kind == FdKind::PipeRead || src.kind == FdKind::PipeWrite) {
        if (src.pipe) {
            const uint64_t f = aos_spin_lock_irqsave(&pipe_lock);
            if (src.kind == FdKind::PipeRead)  ++src.pipe->readers;
            if (src.kind == FdKind::PipeWrite) ++src.pipe->writers;
            aos_spin_unlock_irqrestore(&pipe_lock, f);
        }
    }
    return newfd;
}

int64_t fd_seek(FdTable* t, int fd, int64_t offset, int whence) {
    if (fd < 0 || static_cast<unsigned>(fd) >= kMaxFds) return E_BADF;
    FileDesc& d = t->fds[fd];
    if (d.kind != FdKind::File) return E_BADF;

    size_t total = 0;
    aos::fs::read_into(d.path, nullptr, 0, &total);  // size only

    int64_t base = 0;
    if (whence == 1) base = d.offset;           // SEEK_CUR
    else if (whence == 2) base = static_cast<int64_t>(total); // SEEK_END
    else if (whence != 0) return E_INVAL;       // SEEK_SET

    int64_t neu = base + offset;
    if (neu < 0) return E_INVAL;
    d.offset = static_cast<uint32_t>(neu);
    return neu;
}


Pipe* pipe_create() {
    Pipe* p = pipe_alloc();
    if (!p) return nullptr;
    p->readers = 0;
    p->writers = 0;
    return p;
}

void pipe_attach_read(FdTable* t, int fd, Pipe* p) {
    if (!t || !p || fd < 0 || static_cast<unsigned>(fd) >= kMaxFds) return;
    if (t->fds[fd].kind != FdKind::Free && t->fds[fd].kind != FdKind::Stdin)
        fd_close(t, fd);
    const uint64_t f = aos_spin_lock_irqsave(&pipe_lock);
    ++p->readers;
    aos_spin_unlock_irqrestore(&pipe_lock, f);
    t->fds[fd].kind = FdKind::PipeRead;
    t->fds[fd].pipe = p;
    t->fds[fd].flags = O_RDONLY;
}

void pipe_attach_write(FdTable* t, int fd, Pipe* p) {
    if (!t || !p || fd < 0 || static_cast<unsigned>(fd) >= kMaxFds) return;
    if (t->fds[fd].kind != FdKind::Free && t->fds[fd].kind != FdKind::Stdout)
        fd_close(t, fd);
    const uint64_t f = aos_spin_lock_irqsave(&pipe_lock);
    ++p->writers;
    aos_spin_unlock_irqrestore(&pipe_lock, f);
    t->fds[fd].kind = FdKind::PipeWrite;
    t->fds[fd].pipe = p;
    t->fds[fd].flags = O_WRONLY;
}

void pipe_release_one(Pipe* p, bool was_reader) {
    if (!p) return;
    const uint64_t f = aos_spin_lock_irqsave(&pipe_lock);
    if (was_reader) { if (p->readers) --p->readers; }
    else            { if (p->writers) --p->writers; }
    const bool dead = (p->readers == 0 && p->writers == 0);
    aos_spin_unlock_irqrestore(&pipe_lock, f);
    if (dead) pipe_release(p);
}

}  // namespace aos::process
