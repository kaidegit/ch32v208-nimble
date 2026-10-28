/*
 * npl_wch_scb.h - compile shim for npl_os_freertos.c on RISC-V.
 *
 * porting/npl/freertos/src/npl_os_freertos.c contains:
 *
 *     static inline bool in_isr(void)
 *     {
 *         return (SCB->ICSR & SCB_ICSR_VECTACTIVE_Msk) != 0;
 *     }
 *
 * which is an ARM CMSIS-ism.  On the CH32V208 (QingKe V4, RISC-V) there is
 * no SCB peripheral and no memory-mapped register that reports trap state,
 * so this shim binds SCB->ICSR to a plain RAM word that always reads 0.
 * That alone would give wrong FromISR/behaviour for calls made from
 * interrupt context, therefore the ISR-sensitive ble_npl_*() entry points
 * are re-implemented in the shadowed config/nimble/nimble_npl_os.h: they
 * detect interrupt context via the mcause CSR (bit 31 set) and call the
 * FreeRTOS *FromISR() variants directly, delegating to the stock
 * npl_freertos_*() functions only from task context.  Nothing ever reaches
 * the fake in_isr() while running in an interrupt.
 */
#ifndef H_NPL_WCH_SCB_H_
#define H_NPL_WCH_SCB_H_

#include <stdint.h>

typedef struct {
    volatile uint32_t ICSR;      /* reads as 0 -> "not in ISR" */
} SCB_WCH_TypeDef;

#define SCB_ICSR_VECTACTIVE_Msk  (0x1FFUL)   /* CMSIS: VECTACTIVE[8:0] */

/* Any writable RAM location works; the linker places it in .bss. */
extern volatile uint32_t g_npl_wch_scb_shim;
#define SCB ((SCB_WCH_TypeDef *)&g_npl_wch_scb_shim)

#endif /* H_NPL_WCH_SCB_H_ */
