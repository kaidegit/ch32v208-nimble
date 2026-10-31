/* QingKe V4: GINTENR maps mstatus.MIE and MPIE (processor manual 8.3).
 * Mask both bits: MRET restores MIE from MPIE. Preserve the caller's state
 * without writing unrelated mstatus privilege/FP fields. */
#ifndef WCH_IRQ_H
#define WCH_IRQ_H
#include <stdint.h>

static inline uint32_t wch_irq_save(void)
{
    uint32_t prior;
    __asm volatile("csrrc %0, 0x800, %1; fence.i"
                   : "=r"(prior) : "r"(0x88U) : "memory");
    return prior & 0x88U;
}

static inline void wch_irq_restore(uint32_t prior)
{
    /* Use bit operations on the alias: a whole-register write also changes
     * other mstatus fields on this core, including MPP. */
    __asm volatile("csrc 0x800, %0; csrs 0x800, %1; fence.i"
                   :: "r"(0x88U), "r"(prior & 0x88U) : "memory");
}
#endif
