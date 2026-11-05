#include "process.hpp"
#include "elf.hpp"
#include "../arch/x86_64/frame.hpp"
#include "../arch/x86_64/percpu.hpp"
#include "../arch/x86_64/sync.h"
#include "../arch/x86_64/interrupts.hpp"
#include "../memory/physical/pmm.h"
#include "../scheduler/task.hpp"
#include "../filesystem/initramfs.hpp"
#include "../filesystem/ramfs.hpp"
#include "../memory/heap/kheap.hpp"

extern "C" [[noreturn]] void aos_fork_return(InterruptFrame* f);

extern "C" void aos_serial_write(const char* text);
extern "C" void aos_serial_write_hex64(uint64_t value);

namespace aos::process {
namespace {
Process        table[kMaxProcesses];
aos_spinlock_t lock = AOS_SPINLOCK_INIT;
uint32_t       next_pid = 1;
volatile uint32_t fg_pid = 0;

// ---- console input shared by all user programs
aos_spinlock_t in_lock = AOS_SPINLOCK_INIT;
char     edit[256];
unsigned edit_len = 0;
char     ready_buf[1024];
unsigned ready_head = 0, ready_tail = 0;
volatile bool eof_flag = false;
volatile unsigned readers = 0;

void echo(const char* s) { aos_serial_write(s); }
void put_dec(uint64_t v) {
    char b[21]; int n = 20; b[n] = 0;
    if (!v) b[--n] = '0';
    while (v) { b[--n] = static_cast<char>('0' + v % 10); v /= 10; }
    aos_serial_write(&b[n]);
}
void copy_name(char* dst, const char* src, unsigned max) {
    unsigned i = 0;
    for (; src && src[i] && i + 1 < max; ++i) dst[i] = src[i];
    dst[i] = 0;
}

[[noreturn]] void process_main(void* arg) {
    Process* p = static_cast<Process*>(arg);
    if (p->forked_child) {
        // Resume at the syscall return site with rax = 0 (child).
        // Build an iretq frame on this kernel stack from the saved user frame.
        InterruptFrame fr = p->fork_frame;
        fr.rax = 0;
        p->forked_child = false;
        // Switch to the child's address space (already in CR3 via scheduler) and return to user.
        // We reuse aos_enter_user only for fresh processes; forked children need full register restore.
        // Minimal path: set rip/rsp from saved frame and zero most regs except the ones iretq needs.
        // Full fidelity: jump through a small assembly trampoline.  For correctness of fork() return
        // value and control flow, restoring rip/rsp/rflags and rax is enough for C code after syscall.
        aos_fork_return(&fr);
    }
    aos_enter_user(p->entry, p->user_rsp, p->argc, p->argv);
}

void reap(void* owner) {                    // scheduler hook: the thread is gone, its stack is free
    Process* p = static_cast<Process*>(owner);
    fd_close_all(&p->fds);
    aos::vmm::as_destroy(&p->as);
    if (p->state != State::Zombie) { p->exit_code = -1; p->state = State::Zombie; }
}

Process* by_pid(uint32_t pid) {
    for (auto& p : table) if (p.state != State::Free && p.pid == pid) return &p;
    return nullptr;
}

bool build_stack(Process* p, const char* name, const char* args) {
    char strs[256]; unsigned offs[9]; unsigned n = 0, used = 0;
    auto add = [&](const char* s, unsigned len) {
        if (n >= 8 || used + len + 1 > sizeof(strs)) return;
        offs[n++] = used;
        for (unsigned i = 0; i < len; ++i) strs[used++] = s[i];
        strs[used++] = 0;
    };
    unsigned nl = 0; while (name[nl]) ++nl;
    add(name, nl);
    for (const char* a = args; a && *a;) {
        while (*a == ' ') ++a;
        const char* b = a;
        while (*a && *a != ' ') ++a;
        if (a > b) add(b, static_cast<unsigned>(a - b));
    }
    const uint64_t str_base = kStackTop - ((used + 15) & ~15u);
    const uint64_t arr_base = (str_base - (n + 1) * 8) & ~15ULL;
    uint64_t argv[9];
    for (unsigned i = 0; i < n; ++i) argv[i] = str_base + offs[i];
    argv[n] = 0;
    if (!aos::vmm::as_copy_to(&p->as, str_base, strs, used, true)) return false;
    if (!aos::vmm::as_copy_to(&p->as, arr_base, argv, (n + 1) * 8, true)) return false;
    p->user_rsp = arr_base - 64;
    p->argc = n;
    p->argv = arr_base;
    return true;
}
}  // namespace

void init() { aos::scheduler::set_reap_hook(reap); }
Process* current() { return static_cast<Process*>(aos::scheduler::current_owner()); }
void set_foreground(uint32_t pid) { fg_pid = pid; }
uint32_t foreground() { return fg_pid; }

int spawn(const uint8_t* image, size_t length, const char* name, const char* args, const char** error, const SpawnStdio* io) {
    const char* dummy;
    if (!error) error = &dummy;
    aos::elf::Image img;
    const aos::elf::Err e = aos::elf::parse(image, length, aos::vmm::kUserBase, aos::vmm::kUserEnd, &img);
    if (e != aos::elf::Err::Ok) { *error = aos::elf::error_text(e); return -1; }

    uint64_t f = aos_spin_lock_irqsave(&lock);
    Process* p = nullptr;
    for (auto& q : table) if (q.state == State::Free) { p = &q; break; }
    if (!p) for (auto& q : table) if (q.state == State::Zombie && !q.as.pml4) { p = &q; break; }   // recycle a finished one
    if (!p) { aos_spin_unlock_irqrestore(&lock, f); *error = "process table full"; return -1; }
    __builtin_memset(static_cast<void*>(p), 0, sizeof(*p));
    p->pid = next_pid++;
    p->state = State::Loading;
    aos_spin_unlock_irqrestore(&lock, f);
    copy_name(p->name, name, sizeof(p->name));

    auto fail = [&](const char* why) {
        fd_close_all(&p->fds);
        aos::vmm::as_destroy(&p->as);
        p->state = State::Free;
        *error = why;
        return -1;
    };
    if (!aos::vmm::as_create(&p->as)) return fail("out of memory (page tables)");
    for (unsigned i = 0; i < img.segment_count; ++i) {
        const aos::elf::Segment& s = img.segments[i];
        const uint64_t first = s.vaddr & ~4095ULL;
        const uint64_t pages = ((s.vaddr + s.memsz + 4095) & ~4095ULL) - first;
        const uint64_t flags = ((s.flags & aos::elf::PF_W) ? aos::vmm::PF_WRITE : 0) | ((s.flags & aos::elf::PF_X) ? 0 : aos::vmm::PF_NX);
        if (!aos::vmm::as_alloc(&p->as, first, pages / 4096, flags)) return fail("out of memory (segment)");
        if (s.filesz && !aos::vmm::as_copy_to(&p->as, s.vaddr, image + s.offset, s.filesz, true)) return fail("segment copy failed");
    }
    if (!aos::vmm::as_alloc(&p->as, kStackTop - kStackPages * 4096, kStackPages, aos::vmm::PF_WRITE | aos::vmm::PF_NX))
        return fail("out of memory (stack)");
    if (!build_stack(p, p->name, args)) return fail("argument setup failed");
    p->entry = img.entry;
    p->brk = kHeapBase;
    p->mmap_next = kMmapBase;
    p->parent_task = aos::scheduler::current_id();
    // Inherit FDs from the parent process when one exists; otherwise fresh stdio.
    if (Process* parent = current()) fd_clone(&parent->fds, &p->fds);
    else fd_init_std(&p->fds);
    // Optional pipeline redirection (kernel shell).
    if (io) {
        if (io->stdin_from)  pipe_attach_read(&p->fds, 0, io->stdin_from);
        if (io->stdout_to)   pipe_attach_write(&p->fds, 1, io->stdout_to);
    }
    p->forked_child = false;
    p->state = State::Running;

    aos::scheduler::SpawnParams sp;
    sp.name = p->name;
    sp.entry = reinterpret_cast<aos::scheduler::Entry>(process_main);
    sp.arg = p;
    sp.prio = aos::scheduler::PRIO_NORMAL;
    sp.cr3 = p->as.pml4;
    sp.owner = p;
    const int tid = aos::scheduler::spawn(sp);
    if (tid < 0) { p->state = State::Loading; return fail("no free kernel thread"); }
    p->task_id = static_cast<uint32_t>(tid);
    return static_cast<int>(p->pid);
}

int spawn_path(const char* path, const char* args, const char** error, const SpawnStdio* io) {
    const char* dummy;
    if (!error) error = &dummy;
    aos::filesystem::FileView v;
    if (aos::filesystem::find_file(path, &v)) return spawn(v.data, v.size, path, args, error, io);
    void* buf = aos::heap::allocate(aos::fs::CONTENT_MAX);
    if (!buf) { *error = "out of memory"; return -1; }
    size_t n = 0;
    if (aos::fs::read_into(path, buf, aos::fs::CONTENT_MAX, &n) != aos::fs::Result::Ok) {
        aos::heap::release(buf);
        *error = "not found (initramfs and AOSFS)";
        return -1;
    }
    const int pid = spawn(static_cast<const uint8_t*>(buf), n, path, args, error, io);
    aos::heap::release(buf);
    return pid;
}


int fork_from(InterruptFrame* frame) {
    Process* parent = current();
    if (!parent || !frame) return -1;

    uint64_t f = aos_spin_lock_irqsave(&lock);
    Process* child = nullptr;
    for (auto& q : table) if (q.state == State::Free) { child = &q; break; }
    if (!child) for (auto& q : table) if (q.state == State::Zombie && !q.as.pml4) { child = &q; break; }
    if (!child) { aos_spin_unlock_irqrestore(&lock, f); return -1; }
    __builtin_memset(static_cast<void*>(child), 0, sizeof(*child));
    child->pid = next_pid++;
    child->state = State::Loading;
    aos_spin_unlock_irqrestore(&lock, f);

    // Copy name with "-child" suffix if room.
    unsigned i = 0;
    for (; parent->name[i] && i + 1 < sizeof(child->name); ++i) child->name[i] = parent->name[i];
    child->name[i] = 0;

    if (!aos::vmm::as_clone(&parent->as, &child->as)) {
        child->state = State::Free;
        return -1;
    }
    fd_clone(&parent->fds, &child->fds);
    child->entry = parent->entry;
    child->user_rsp = parent->user_rsp;
    child->argc = parent->argc;
    child->argv = parent->argv;
    child->brk = parent->brk;
    child->mmap_next = parent->mmap_next;
    child->parent_task = aos::scheduler::current_id();
    child->fork_frame = *frame;
    child->forked_child = true;
    child->state = State::Running;

    aos::scheduler::SpawnParams sp;
    sp.name = child->name;
    sp.entry = reinterpret_cast<aos::scheduler::Entry>(process_main);
    sp.arg = child;
    sp.prio = aos::scheduler::PRIO_NORMAL;
    sp.cr3 = child->as.pml4;
    sp.owner = child;
    const int tid = aos::scheduler::spawn(sp);
    if (tid < 0) {
        fd_close_all(&child->fds);
        aos::vmm::as_destroy(&child->as);
        child->state = State::Free;
        return -1;
    }
    child->task_id = static_cast<uint32_t>(tid);
    return static_cast<int>(child->pid);
}

bool wait_for(uint32_t pid, int* code, bool blocking) {
    for (;;) {
        Process* p = by_pid(pid);
        if (!p) return false;
        if (p->state == State::Zombie && !p->as.pml4) {
            if (code) *code = p->exit_code;
            p->state = State::Free;
            if (fg_pid == pid) fg_pid = 0;
            return true;
        }
        if (!blocking) return false;
        aos::scheduler::sleep_current(1);
    }
}

bool kill(uint32_t pid) {
    Process* p = by_pid(pid);
    if (!p || p->state != State::Running) return false;
    p->killed = true;
    return true;
}

[[noreturn]] void exit_current(int code) {
    Process* p = current();
    if (p) {
        fd_close_all(&p->fds);
        p->exit_code = code;
        p->state = State::Zombie;
    }
    aos::scheduler::exit_current(code);
}

void check_kill() {
    Process* p = current();
    if (p && p->killed) exit_current(-9);
}

[[noreturn]] void user_fault(InterruptFrame* f) {
    Process* p = current();
    aos_serial_write("\r\n[proc ");
    put_dec(p ? p->pid : 0);
    aos_serial_write(" '");
    aos_serial_write(p ? p->name : "?");
    aos_serial_write("'] killed by exception ");
    put_dec(f->vector);
    aos_serial_write(" rip="); aos_serial_write_hex64(f->rip);
    aos_serial_write(" err="); aos_serial_write_hex64(f->error);
    if (f->vector == 14) {
        uint64_t cr2; asm volatile("mov %%cr2, %0" : "=r"(cr2));
        aos_serial_write(" addr="); aos_serial_write_hex64(cr2);
    }
    aos_serial_write("\r\n");
    int code = 128 + static_cast<int>(f->vector);
    if (f->vector == 14 || f->vector == 13) code = 139;      // like SIGSEGV
    else if (f->vector == 0) code = 136;                     // SIGFPE
    else if (f->vector == 6) code = 132;                     // SIGILL
    exit_current(code);
}

int list(ProcInfo* out, int max) {
    int n = 0;
    for (auto& p : table) {
        if (p.state == State::Free || n >= max) continue;
        ProcInfo& o = out[n++];
        o.pid = p.pid; copy_name(o.name, p.name, sizeof(o.name));
        o.state = static_cast<uint8_t>(p.state); o.exit_code = p.exit_code; o.pages = p.as.pages; o.task_id = p.task_id;
    }
    return n;
}

// ------------------------------------------------------------------ stdin (line discipline)
void stdin_push(char c) {
    const uint64_t f = aos_spin_lock_irqsave(&in_lock);
    if (c == '\r') c = '\n';
    if (c == 4) {
        if (!edit_len) eof_flag = true;
    } else if (c == 8 || c == 127) {
        if (edit_len) { --edit_len; echo("\b \b"); }
    } else if (c == '\n') {
        echo("\r\n");
        for (unsigned i = 0; i < edit_len; ++i) { const unsigned nx = (ready_head + 1) % sizeof(ready_buf); if (nx != ready_tail) { ready_buf[ready_head] = edit[i]; ready_head = nx; } }
        const unsigned nx = (ready_head + 1) % sizeof(ready_buf);
        if (nx != ready_tail) { ready_buf[ready_head] = '\n'; ready_head = nx; }
        edit_len = 0;
    } else if (c >= 32 && c < 127 && edit_len < sizeof(edit)) {
        edit[edit_len++] = c;
        char s[2] = {c, 0}; echo(s);
    }
    aos_spin_unlock_irqrestore(&in_lock, f);
}

bool stdin_wanted() { return readers != 0; }

long stdin_read(char* dst, size_t max) {
    Process* me = current();
    __atomic_add_fetch(&readers, 1, __ATOMIC_SEQ_CST);
    long got = 0;
    for (;;) {
        const uint64_t f = aos_spin_lock_irqsave(&in_lock);
        if (ready_head != ready_tail) {
            while (ready_head != ready_tail && static_cast<size_t>(got) < max) {
                const char c = ready_buf[ready_tail];
                ready_tail = (ready_tail + 1) % sizeof(ready_buf);
                dst[got++] = c;
                if (c == '\n') break;
            }
            aos_spin_unlock_irqrestore(&in_lock, f);
            break;
        }
        if (eof_flag) { eof_flag = false; aos_spin_unlock_irqrestore(&in_lock, f); break; }
        aos_spin_unlock_irqrestore(&in_lock, f);
        if (me && me->killed) { got = -1; break; }
        aos::scheduler::sleep_current(1);
    }
    __atomic_sub_fetch(&readers, 1, __ATOMIC_SEQ_CST);
    return got;
}
}  // namespace aos::process
