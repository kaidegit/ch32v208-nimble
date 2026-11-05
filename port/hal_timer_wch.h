/*
 * hal_timer_wch.h - NimBLE HAL timer backed by CH32V208 TIM3.
 *
 * Why 1 MHz cputime (os_cputime_init(1000000), MYNEWT_VAL_OS_CPUTIME_FREQ):
 *   - controller/ble_ll_tmr.h has a dedicated 32768 Hz code path; at 1 MHz
 *     USECS_PER_TICK evaluates to 1 and os_cputime_usecs_to_ticks() /
 *     os_cputime_ticks_to_usecs() compile to the identity (OS_CPUTIME_FREQ_1MHZ
 *     in porting/nimble/include/os/os_cputime.h), so every LL schedule/anchor
 *     computation is an exact integer usec value - no rounding, no remainder
 *     tracking, simplest and most precise LL scheduling.
 *   - TIM3 runs from PCLK1 (x2 if APB1 is prescaled) which derives from the
 *     32 MHz HSE PLL; a PSC for exactly 1 MHz is always achievable.
 *
 * The CH32V208 BLE link (wch_rf.c BB/LLE interrupts, owned by the driver
 * agent) uses the dedicated BB_IRQn/LLE_IRQn vectors; TIM3 is reserved
 * exclusively for the NimBLE cputime/HAL timer.
 */
#ifndef H_HAL_TIMER_WCH_H_
#define H_HAL_TIMER_WCH_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Tickless hooks: the caller holds the global IRQ mask throughout.
 * next_us: zero when due/uninitialized, UINT32_MAX when the queue is empty.
 * pause: stop TIM3 and save its current 32-bit time.
 * resume: add RTC elapsed time, restart TIM3 and rearm the next callback.
 * Callbacks remain deferred to the ISR after the caller restores IRQs. */
uint32_t hal_timer_wch_next_us(void);
uint32_t hal_timer_wch_pause(void);
void hal_timer_wch_resume(uint32_t elapsed_us);

/* Called from app/ch32v20x_it.c:
 *     void TIM3_IRQHandler(void) { hal_timer_wch_irq(); }
 * Dispatches UIF (16-bit wrap -> software high word) and CC1IF (timer
 * queue expiry).  Bare-register flag clearing inside. */
void hal_timer_wch_irq(void);

#ifdef __cplusplus
}
#endif

#endif /* H_HAL_TIMER_WCH_H_ */
