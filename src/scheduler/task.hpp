// task.hpp - preemptive priority scheduler (kernel threads; user processes ride on top of them).
//
// Rules for kernel code that may run on a thread other than kmain:
//   * pmm, kheap, aos::fs and the serial console are safe to call from any thread;
//   * everything else (network stack, USB/HDA/I2C polling) belongs to the kmain thread.
//   * hold preempt_disable()/preempt_enable() or an IRQ-safe lock around any non-reentrant section.
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace aos::scheduler {

using Entry = void (*)(void*);

constexpr uint32_t kMaxTasks = 64;
constexpr uint8_t PRIO_URGENT = 0, PRIO_HIGH = 1, PRIO_NORMAL = 2, PRIO_LOW = 3;
constexpr uint32_t TASK_SMP_OK = 1u << 0;     // may be scheduled on application processors

struct SpawnParams {
    const char* name = "task";
    Entry       entry = nullptr;
    void*       arg = nullptr;
    uint8_t     prio = PRIO_NORMAL;
    uint32_t    flags = 0;
    uint64_t    cr3 = 0;                      // address space to load while this thread runs (0 = kernel's)
    void*       owner = nullptr;              // opaque pointer handed to the reap hook (process)
    uint64_t    delay_ticks = 0;
};

struct TaskInfo {
    uint32_t id;
    char     name[16];
    uint8_t  state;          // policy::ST_*
    uint8_t  prio;
    int32_t  cpu;            // CPU it is running on, -1 if not running
    uint64_t run_ticks;
    bool     user;
};

bool initialize();                                   // adopts the boot context as thread 0 ("kmain") + BSP idle thread
bool add_task(Entry entry, void* argument, uint64_t delay_ticks = 0);       // legacy convenience wrapper
int  spawn(const SpawnParams& p);                    // returns thread id or -1
void yield();
void sleep_current(uint64_t ticks);
[[noreturn]] void exit_current(int code);
bool kill(uint32_t id);                              // marks a kernel thread for termination (see should_stop)
bool should_stop();                                  // cooperative cancellation point for kernel threads
bool set_priority(uint32_t id, uint8_t prio);
uint32_t current_id();
void*    current_owner();                            // SpawnParams::owner of the running thread
uint32_t task_count();                               // live threads excluding idle threads
int  snapshot(TaskInfo* out, int max);
const char* state_name(uint8_t state);

// Called from the timer interrupt on every CPU after EOI.
void on_tick();
void preempt_disable();
void preempt_enable();

// kmain helper: sleep until the next tick when other threads need the CPU, otherwise plain hlt.
void idle_wait(uint64_t seen_tick);
void wake(uint32_t id);

void set_reap_hook(void (*hook)(void* owner));       // called when a thread with an owner is torn down

// SMP
void adopt_ap(uint32_t cpu_id);                      // turn the calling AP's boot context into its idle thread
[[noreturn]] void ap_idle_loop();
}  // namespace aos::scheduler
