// mutex.hpp - recursive, yielding mutex for long critical sections (file system) that must stay preemptible.
#pragma once
#include <stdint.h>
#include "task.hpp"
#include "../arch/x86_64/sync.h"

namespace aos::sync {
class RecursiveMutex {
    aos_spinlock_t l_ = AOS_SPINLOCK_INIT;
    uint32_t owner_ = 0, depth_ = 0;
public:
    void lock() {
        const uint32_t me = aos::scheduler::current_id();
        for (;;) {
            const uint64_t f = aos_spin_lock_irqsave(&l_);
            if (depth_ == 0 || owner_ == me) {
                owner_ = me; ++depth_;
                aos_spin_unlock_irqrestore(&l_, f);
                return;
            }
            aos_spin_unlock_irqrestore(&l_, f);
            aos::scheduler::yield();
        }
    }
    void unlock() {
        const uint64_t f = aos_spin_lock_irqsave(&l_);
        if (depth_) {
            if (--depth_ == 0) owner_ = 0;
        }
        aos_spin_unlock_irqrestore(&l_, f);
    }
};
class Guard {
    RecursiveMutex& m_;
public:
    explicit Guard(RecursiveMutex& m) : m_(m) { m_.lock(); }
    ~Guard() { m_.unlock(); }
    Guard(const Guard&) = delete;
    Guard& operator=(const Guard&) = delete;
};
}  // namespace aos::sync
