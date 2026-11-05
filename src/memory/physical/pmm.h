/* SPDX-License-Identifier: MIT */
#ifndef AOS_PMM_H
#define AOS_PMM_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
int pmm_init(const void* multiboot_info, uint64_t kernel_start, uint64_t kernel_end);
uint64_t pmm_alloc_frame(void);
uint64_t pmm_alloc_contiguous(uint64_t count);   /* physically contiguous run, 0 on failure */
void pmm_free_frame(uint64_t address);
uint64_t pmm_free_frames(void);
uint64_t pmm_total_frames(void);   /* free frames at first call (call early in boot) */

/* Reference counting for COW and shared pages.
 * pmm_alloc_frame() starts the new frame at refcount 1.
 * pmm_inc_ref / pmm_dec_ref adjust it.  When the count reaches zero the
 * frame is returned to the free list (same as pmm_free_frame).
 * pmm_get_ref returns the current count (0 if the address is invalid).
 */
void     pmm_inc_ref(uint64_t physical_address);
void     pmm_dec_ref(uint64_t physical_address);   /* frees when count hits 0 */
uint16_t pmm_get_ref(uint64_t physical_address);

#ifdef __cplusplus
}
#endif
#endif
