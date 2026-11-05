/* SPDX-License-Identifier: MIT */
/* sync.h - IRQ-safe spinlocks usable from both C and C++.
 *
 * aos_spin_lock_irqsave() disables interrupts on the local CPU first, so a lock holder can never be
 * preempted by the timer tick (and therefore never deadlocks against itself on one CPU) and spins on
 * other CPUs when SMP is enabled.  Hold these locks only for a few instructions.
 */
#ifndef AOS_SYNC_H
#define AOS_SYNC_H
#include <stdint.h>

typedef struct { volatile uint32_t locked; } aos_spinlock_t;
#define AOS_SPINLOCK_INIT { 0 }

#ifdef AOS_HOST_TEST   /* unit tests run as an ordinary Linux process: no cli/sti */
static inline uint64_t aos_irq_save(void) { return 0; }
static inline void aos_irq_restore(uint64_t f) { (void)f; }
#else
static inline uint64_t aos_irq_save(void) {
    uint64_t f;
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(f) : : "memory");
    return f;
}
static inline void aos_irq_restore(uint64_t f) {
    if (f & 0x200) __asm__ volatile("sti" : : : "memory");
}
#endif
static inline void aos_spin_lock(aos_spinlock_t* l) {
    while (__atomic_exchange_n(&l->locked, 1u, __ATOMIC_ACQUIRE)) {
        while (__atomic_load_n(&l->locked, __ATOMIC_RELAXED)) __asm__ volatile("pause");
    }
}
static inline int aos_spin_trylock(aos_spinlock_t* l) {
    return __atomic_exchange_n(&l->locked, 1u, __ATOMIC_ACQUIRE) == 0;
}
static inline void aos_spin_unlock(aos_spinlock_t* l) { __atomic_store_n(&l->locked, 0u, __ATOMIC_RELEASE); }
static inline uint64_t aos_spin_lock_irqsave(aos_spinlock_t* l) {
    const uint64_t f = aos_irq_save();
    aos_spin_lock(l);
    return f;
}
static inline void aos_spin_unlock_irqrestore(aos_spinlock_t* l, uint64_t f) {
    aos_spin_unlock(l);
    aos_irq_restore(f);
}

/* Kernel static data lives in the higher half when built with HH=1; DMA engines need physical addresses. */
#define AOS_KERNEL_VMA_BASE 0xFFFFFFFF80000000ULL
static inline uint64_t aos_virt_to_phys(const void* p) {
    const uint64_t a = (uint64_t)(uintptr_t)p;
    return a >= AOS_KERNEL_VMA_BASE ? a - AOS_KERNEL_VMA_BASE : a;
}
#endif
