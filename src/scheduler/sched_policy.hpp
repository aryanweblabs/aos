// sched_policy.hpp - the scheduling *policy*, free of hardware so it can be unit-tested on the host.
//
// Multilevel priority round robin with time slices and aging:
//   * levels 0 (most urgent) .. 3 for normal work, level 4 is reserved for idle threads;
//   * the highest level with a runnable thread always wins, equal levels share the CPU round-robin;
//   * a runnable thread that has waited kAgeTicks without running is promoted one level (never above 0),
//     and drops back to its base level as soon as it gets the CPU - so nothing can starve;
//   * the slice is longer for more important levels.
#pragma once
#include <stdint.h>

namespace aos::scheduler::policy {

constexpr unsigned kLevels = 5;
constexpr unsigned kIdleLevel = 4;
constexpr uint32_t kAgeTicks = 45;            // ~0.5 s at 90 Hz

enum : uint8_t { ST_UNUSED = 0, ST_READY = 1, ST_RUNNING = 2, ST_ZOMBIE = 3 };
enum : uint32_t { TASK_SMP_OK = 1u << 0, TASK_IDLE = 1u << 1, TASK_KERNEL = 1u << 2 };

inline uint32_t slice_for(uint8_t prio) {
    static const uint8_t slices[kLevels] = {9, 7, 5, 3, 1};
    return slices[prio < kLevels ? prio : kLevels - 1];
}

// T needs: state, prio, eff_prio, flags, wait_ticks, wake_tick, pinned_cpu (int32_t), on_cpu (int32_t).
template <class T>
inline bool eligible(const T& t, unsigned cpu, uint64_t now) {
    if (t.state != ST_READY || t.on_cpu >= 0 || now < t.wake_tick) return false;
    if (t.pinned_cpu >= 0) return static_cast<unsigned>(t.pinned_cpu) == cpu;
    return cpu == 0 || (t.flags & TASK_SMP_OK) != 0;
}

// Index of the thread to run next on `cpu`, or -1.  `cursor` remembers the last pick per level.
template <class T>
inline int pick_next(T* tasks, unsigned n, unsigned cpu, uint64_t now, unsigned* cursor) {
    for (unsigned level = 0; level < kLevels; ++level) {
        for (unsigned step = 1; step <= n; ++step) {
            const unsigned i = (cursor[level] + step) % n;
            if (tasks[i].eff_prio == level && eligible(tasks[i], cpu, now)) {
                cursor[level] = i;
                return static_cast<int>(i);
            }
        }
    }
    return -1;
}

// True when some eligible thread is more urgent than `level` (used to preempt a running thread early).
template <class T>
inline bool better_ready(const T* tasks, unsigned n, unsigned cpu, uint64_t now, unsigned level) {
    for (unsigned i = 0; i < n; ++i)
        if (tasks[i].eff_prio < level && eligible(tasks[i], cpu, now)) return true;
    return false;
}

template <class T>
inline bool any_ready(const T* tasks, unsigned n, unsigned cpu, uint64_t now, unsigned min_level_exclusive) {
    for (unsigned i = 0; i < n; ++i)
        if (tasks[i].eff_prio < min_level_exclusive && eligible(tasks[i], cpu, now)) return true;
    return false;
}

// One aging step per timer tick.
template <class T>
inline void age(T* tasks, unsigned n, uint64_t now) {
    for (unsigned i = 0; i < n; ++i) {
        T& t = tasks[i];
        if (t.state != ST_READY || now < t.wake_tick) { t.wait_ticks = 0; continue; }
        if (t.eff_prio == 0 || (t.flags & TASK_IDLE)) continue;
        if (++t.wait_ticks >= kAgeTicks) { --t.eff_prio; t.wait_ticks = 0; }
    }
}

template <class T>
inline void on_dispatch(T& t) {
    t.eff_prio = t.prio;
    t.wait_ticks = 0;
}
}  // namespace aos::scheduler::policy
