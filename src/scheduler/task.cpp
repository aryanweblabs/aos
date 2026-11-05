#include "task.hpp"
#include <stddef.h>
#include "sched_policy.hpp"
#include "../arch/x86_64/interrupts.hpp"
#include "../arch/x86_64/percpu.hpp"
#include "../arch/x86_64/sync.h"
#include "../memory/physical/pmm.h"
#include "../memory/virtual/vmm.hpp"

extern "C" void aos_serial_write(const char* text);

namespace aos::scheduler {
using namespace policy;

struct Context {
    uint64_t rsp;
    uint64_t pad;
    alignas(16) uint8_t fx_state[512];
};

static_assert(offsetof(Context, fx_state) == 16, "context.S addresses the FXSAVE area at offset 16");

struct Task {
    Context  ctx;                 // first: aos_context_switch addresses it directly
    uint32_t id;
    char     name[16];
    uint8_t  state;
    uint8_t  prio;
    uint8_t  eff_prio;
    uint8_t  killed;
    uint32_t flags;
    int32_t  on_cpu;
    int32_t  pinned_cpu;
    uint32_t slice_left;
    uint32_t wait_ticks;
    uint64_t wake_tick;
    uint64_t run_ticks;
    uint64_t cr3;
    uint64_t kstack_base;         // 0 for adopted boot contexts (their stack is not ours to free)
    uint64_t kstack_top;
    Entry    entry;
    void*    arg;
    void*    owner;
    int      exit_code;
};

extern "C" void aos_context_switch(Context* old_ctx, Context* new_ctx);
extern "C" [[noreturn]] void aos_task_bootstrap();

namespace {
constexpr unsigned kStackPages = 8;                // 32 KiB + 1 unmapped guard page (AOSFS uses large stack buffers)

Task            tasks[kMaxTasks];
aos_spinlock_t  sched_lock = AOS_SPINLOCK_INIT;
aos_spinlock_t  stack_lock = AOS_SPINLOCK_INIT;
unsigned        cursor[kLevels];
bool            ready = false;
uint32_t        next_id = 1;
void          (*reap_hook)(void*) = nullptr;
alignas(16) uint8_t initial_fx[512];

inline Task* cur_task() { return static_cast<Task*>(aos::arch::percpu()->current); }

void copy_name(char* dst, const char* src) {
    unsigned i = 0;
    for (; src && src[i] && i < 15; ++i) dst[i] = src[i];
    dst[i] = 0;
}

bool stack_alloc(uint64_t* base, uint64_t* top) {
    const uint64_t p = pmm_alloc_contiguous(kStackPages + 1);
    if (!p) return false;
    const uint64_t f = aos_spin_lock_irqsave(&stack_lock);
    aos::vmm::unmap_page(p);                       // guard page: overflow faults instead of corrupting memory
    aos_spin_unlock_irqrestore(&stack_lock, f);
    *base = p;
    *top = p + (kStackPages + 1) * 4096ULL - 8;    // rsp == 8 (mod 16) at function entry, like after a call
    return true;
}

void stack_free(uint64_t base) {
    const uint64_t f = aos_spin_lock_irqsave(&stack_lock);
    aos::vmm::map_page(base, base, 3);
    aos_spin_unlock_irqrestore(&stack_lock, f);
    for (unsigned i = 0; i <= kStackPages; ++i) pmm_free_frame(base + i * 4096ULL);
}

void init_context(Task& t) {
    for (unsigned i = 0; i < 512; ++i) t.ctx.fx_state[i] = initial_fx[i];
    auto* sp = reinterpret_cast<uint64_t*>(t.kstack_top - 8 * sizeof(uint64_t));
    for (int i = 0; i < 6; ++i) sp[i] = 0;                        // r15 r14 r13 r12 rbp rbx
    sp[6] = 0x2;                                                  // rflags: IF clear until finish_switch() ran
    sp[7] = reinterpret_cast<uint64_t>(&aos_task_bootstrap);
    t.ctx.rsp = reinterpret_cast<uint64_t>(sp);
}

Task* slot_locked() {
    for (uint32_t i = 1; i < kMaxTasks; ++i) if (tasks[i].state == ST_UNUSED) return &tasks[i];
    return nullptr;
}

void idle_main(void*) {
    for (;;) asm volatile("sti; hlt");
}

void finish_switch() {
    aos::arch::PerCpu* c = aos::arch::percpu();
    Task* prev = static_cast<Task*>(c->switch_prev);
    c->switch_prev = nullptr;
    aos_spin_unlock(&sched_lock);
    if (prev && prev->state == ST_ZOMBIE) {            // we are on another stack now: tear the dead thread down
        if (prev->owner && reap_hook) reap_hook(prev->owner);
        if (prev->kstack_base) stack_free(prev->kstack_base);
        prev->kstack_base = 0;
        prev->owner = nullptr;
        prev->state = ST_UNUSED;
    }
}

// Preconditions: IRQs off, sched_lock held.  Postcondition: lock released, IRQs still off.
void switch_locked(aos::arch::PerCpu* c) {
    Task* prev = static_cast<Task*>(c->current);
    const uint64_t now = timer_ticks();
    if (prev->state == ST_RUNNING) prev->state = ST_READY;   // may be picked again below
    prev->on_cpu = -1;
    int idx = pick_next(tasks, kMaxTasks, c->cpu_id, now, cursor);
    if (idx < 0) idx = static_cast<int>(static_cast<Task*>(c->idle) - tasks);
    Task* next = &tasks[idx];
    if (next == prev) {
        prev->state = ST_RUNNING;
        prev->on_cpu = static_cast<int32_t>(c->cpu_id);
        on_dispatch(*prev);
        prev->slice_left = slice_for(prev->prio);
        aos_spin_unlock(&sched_lock);
        return;
    }
    next->state = ST_RUNNING;
    next->on_cpu = static_cast<int32_t>(c->cpu_id);
    on_dispatch(*next);
    next->slice_left = slice_for(next->prio);
    c->current = next;
    c->switch_prev = prev;
    aos::arch::set_kernel_stack(next->kstack_top);
    const uint64_t want = next->cr3 ? next->cr3 : aos::vmm::kernel_cr3();
    uint64_t have;
    asm volatile("mov %%cr3, %0" : "=r"(have));
    if (have != want) asm volatile("mov %0, %%cr3" : : "r"(want) : "memory");
    aos_context_switch(&prev->ctx, &next->ctx);
    finish_switch();                                    // resumed (possibly on another CPU)
}
}  // namespace

extern "C" [[noreturn]] void aos_task_bootstrap() {
    finish_switch();
    asm volatile("sti");
    Task* t = cur_task();
    t->entry(t->arg);
    exit_current(0);
}

bool initialize() {
    for (auto& t : tasks) t = {};
    asm volatile("fninit");
    asm volatile("fxsave64 %0" : "=m"(initial_fx));
    aos::arch::PerCpu* c = aos::arch::percpu();
    Task& k = tasks[0];
    k.id = next_id++;                                   // id 1 = kmain (index 0)
    copy_name(k.name, "kmain");
    k.state = ST_RUNNING;
    k.prio = k.eff_prio = PRIO_HIGH;
    k.on_cpu = 0;
    k.pinned_cpu = 0;
    k.flags = TASK_KERNEL;
    k.slice_left = slice_for(k.prio);
    c->current = &k;

    Task& idle = tasks[1];
    if (!stack_alloc(&idle.kstack_base, &idle.kstack_top)) return false;
    idle.id = next_id++;
    copy_name(idle.name, "idle0");
    idle.state = ST_READY;
    idle.prio = idle.eff_prio = kIdleLevel;
    idle.on_cpu = -1;
    idle.pinned_cpu = 0;
    idle.flags = TASK_IDLE | TASK_KERNEL;
    idle.entry = idle_main;
    init_context(idle);
    c->idle = &idle;
    ready = true;
    return true;
}

int spawn(const SpawnParams& p) {
    if (!ready || !p.entry) return -1;
    uint64_t base, top;
    if (!stack_alloc(&base, &top)) return -1;
    const uint64_t f = aos_spin_lock_irqsave(&sched_lock);
    Task* t = slot_locked();
    if (!t) { aos_spin_unlock_irqrestore(&sched_lock, f); stack_free(base); return -1; }
    *t = {};
    t->id = next_id++;
    copy_name(t->name, p.name);
    t->prio = t->eff_prio = p.prio > PRIO_LOW ? PRIO_LOW : p.prio;
    t->flags = p.flags;
    t->on_cpu = -1;
    t->pinned_cpu = -1;
    t->wake_tick = timer_ticks() + p.delay_ticks;
    t->cr3 = p.cr3;
    t->kstack_base = base;
    t->kstack_top = top;
    t->entry = p.entry;
    t->arg = p.arg;
    t->owner = p.owner;
    init_context(*t);
    t->state = ST_READY;                                // published last
    const int id = static_cast<int>(t->id);
    aos_spin_unlock_irqrestore(&sched_lock, f);
    return id;
}

bool add_task(Entry entry, void* argument, uint64_t delay_ticks) {
    SpawnParams p;
    p.entry = entry; p.arg = argument; p.delay_ticks = delay_ticks;
    return spawn(p) > 0;
}

void yield() {
    if (!ready) return;
    const uint64_t f = aos_irq_save();
    aos_spin_lock(&sched_lock);
    switch_locked(aos::arch::percpu());
    aos_irq_restore(f);
}

void sleep_current(uint64_t ticks) {
    if (!ready) return;
    cur_task()->wake_tick = timer_ticks() + ticks;
    yield();
}

[[noreturn]] void exit_current(int code) {
    asm volatile("cli");
    aos_spin_lock(&sched_lock);
    Task* t = cur_task();
    t->state = ST_ZOMBIE;
    t->exit_code = code;
    switch_locked(aos::arch::percpu());
    for (;;) asm volatile("cli; hlt");                  // a zombie is never scheduled again
}

static Task* find_locked(uint32_t id) {
    for (auto& t : tasks) if (t.id == id && t.state != ST_UNUSED) return &t;
    return nullptr;
}

bool kill(uint32_t id) {
    const uint64_t f = aos_spin_lock_irqsave(&sched_lock);
    Task* t = find_locked(id);
    const bool ok = t && !(t->flags & (TASK_IDLE)) && t != &tasks[0];
    if (ok) t->killed = 1;
    aos_spin_unlock_irqrestore(&sched_lock, f);
    return ok;
}

bool should_stop() { return cur_task()->killed != 0; }

bool set_priority(uint32_t id, uint8_t prio) {
    if (prio > PRIO_LOW) return false;
    const uint64_t f = aos_spin_lock_irqsave(&sched_lock);
    Task* t = find_locked(id);
    const bool ok = t && !(t->flags & TASK_IDLE);
    if (ok) { t->prio = t->eff_prio = prio; }
    aos_spin_unlock_irqrestore(&sched_lock, f);
    return ok;
}

uint32_t current_id() { return ready ? cur_task()->id : 1; }
void* current_owner() { return ready ? cur_task()->owner : nullptr; }

uint32_t task_count() {
    uint32_t n = 0;
    for (auto& t : tasks) if (t.state != ST_UNUSED && !(t.flags & TASK_IDLE)) ++n;
    return n;
}

int snapshot(TaskInfo* out, int max) {
    int n = 0;
    const uint64_t f = aos_spin_lock_irqsave(&sched_lock);
    for (auto& t : tasks) {
        if (t.state == ST_UNUSED || n >= max) continue;
        TaskInfo& o = out[n++];
        o.id = t.id;
        for (int i = 0; i < 16; ++i) o.name[i] = t.name[i];
        o.state = t.state; o.prio = t.prio; o.cpu = t.on_cpu; o.run_ticks = t.run_ticks;
        o.user = t.cr3 != 0;
    }
    aos_spin_unlock_irqrestore(&sched_lock, f);
    return n;
}

const char* state_name(uint8_t s) {
    switch (s) { case ST_READY: return "ready"; case ST_RUNNING: return "running"; case ST_ZOMBIE: return "zombie"; default: return "-"; }
}

void on_tick() {
    if (!ready) return;
    aos::arch::PerCpu* c = aos::arch::percpu();
    Task* t = static_cast<Task*>(c->current);
    if (!t) return;
    aos_spin_lock(&sched_lock);
    const uint64_t now = timer_ticks();
    ++t->run_ticks;
    if (c->cpu_id == 0) age(tasks, kMaxTasks, now);
    bool need = false;
    if (t->flags & TASK_IDLE) {
        need = any_ready(tasks, kMaxTasks, c->cpu_id, now, kIdleLevel);
    } else {
        if (t->slice_left) --t->slice_left;
        need = t->slice_left == 0 || better_ready(tasks, kMaxTasks, c->cpu_id, now, t->eff_prio);
    }
    if (need && c->preempt_count == 0) {
        switch_locked(c);                               // releases the lock; returns when we are resumed
        return;
    }
    if (need) c->need_resched = 1;
    aos_spin_unlock(&sched_lock);
}

void preempt_disable() {
    const uint64_t f = aos_irq_save();
    ++aos::arch::percpu()->preempt_count;
    aos_irq_restore(f);
}

void preempt_enable() {
    const uint64_t f = aos_irq_save();
    aos::arch::PerCpu* c = aos::arch::percpu();
    if (c->preempt_count) --c->preempt_count;
    if (!c->preempt_count && c->need_resched && ready) {
        c->need_resched = 0;
        aos_spin_lock(&sched_lock);
        switch_locked(c);
    }
    aos_irq_restore(f);
}

void wake(uint32_t id) {
    const uint64_t f = aos_spin_lock_irqsave(&sched_lock);
    if (Task* t = find_locked(id)) t->wake_tick = 0;
    aos_spin_unlock_irqrestore(&sched_lock, f);
}

void idle_wait(uint64_t seen_tick) {
    // Only kmain and the idle thread exist: the proven plain-hlt path, no context switch at all.
    if (!ready || task_count() <= 1) {
        asm volatile("cli");
        if (timer_ticks() == seen_tick) asm volatile("sti; hlt");
        else asm volatile("sti");
        return;
    }
    sleep_current(1);                                   // let the other threads have the CPU for a tick
}

void set_reap_hook(void (*hook)(void*)) { reap_hook = hook; }

void adopt_ap(uint32_t cpu_id) {
    aos::arch::PerCpu* c = aos::arch::percpu();
    const uint64_t f = aos_spin_lock_irqsave(&sched_lock);
    Task* t = slot_locked();
    *t = {};
    t->id = next_id++;
    copy_name(t->name, "idle");
    t->name[4] = static_cast<char>('0' + (cpu_id % 10));
    t->name[5] = 0;
    t->state = ST_RUNNING;
    t->prio = t->eff_prio = kIdleLevel;
    t->on_cpu = static_cast<int32_t>(cpu_id);
    t->pinned_cpu = static_cast<int32_t>(cpu_id);
    t->flags = TASK_IDLE | TASK_KERNEL;
    t->kstack_top = c->kernel_rsp;
    c->current = t;
    c->idle = t;
    aos_spin_unlock_irqrestore(&sched_lock, f);
}

[[noreturn]] void ap_idle_loop() {
    for (;;) {
        yield();
        asm volatile("sti; hlt");
    }
}
}  // namespace aos::scheduler
