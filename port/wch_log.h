/*
 * wch_log.h - project log backend interface (EasyLogger + USART1 DMA TX).
 *
 * One sink serves every runtime log path in the image:
 *   - EasyLogger records      (log_i/log_d/... from app code, elog.c formats)
 *   - the console facade      (console_printf/console_write, NimBLE host use)
 *   - the NimBLE NPL sink     (config/nimble/nimble_npl_os_log.h)
 *   - newlib stdout/_write    (port/syscalls.c)
 *
 * Implemented in port/console_uart.c: PA9 TX / PA10 RX, USART1, 921600 8N1,
 * DMA1 channel 4 (RM v2.5 fig. 11-1), ring-buffered async TX that never
 * waits for the UART, plus a polled emergency path for assert/fault code
 * that may run with interrupts disabled.
 */
#ifndef H_WCH_LOG_H_
#define H_WCH_LOG_H_

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Bring up USART1 + DMA1_CH4 + EasyLogger. Call once from main() before the
 * first log record (works with the scheduler not yet started). */
void wch_log_init(void);

/* Async ring-buffered output. Copies the caller's bytes (never formats,
 * never waits for UART/DMA); drops whole and counts when the ring is full.
 * Safe from task context and before the scheduler starts. Interrupt context
 * is NOT supported for runtime logs - fault/assert code must use
 * wch_log_panic() instead. */
void wch_log_write(const void *data, size_t len);

/* Polled, bounded emergency output for assert/HardFault/NMI/hooks/_exit.
 * Safe with interrupts disabled and with the DMA TC interrupt unreachable:
 * first parks DMA (bounded poll of the hardware flag, no IRQ needed), then
 * writes bytes by polling TXE with a spin cap. A dead UART degrades to
 * counted loss, never a hang. */
void wch_log_panic(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* DMA1_CH4 interrupt body; dispatched from DMA1_Channel4_IRQHandler in
 * app/ch32v20x_it.c. Never logs, never blocks. */
void wch_log_dma_irq(void);

/* Diagnostics (also readable via gdb - all fields are plain globals). */
struct wch_log_stats {
    uint32_t drop_bytes;   /* payload bytes dropped (ring full)          */
    uint32_t drop_msgs;    /* enqueues dropped whole                     */
    uint32_t dma_errors;   /* DMA1_CH4 transfer-error interrupts         */
    uint32_t dma_dead;     /* 1 after repeated TE parked the channel     */
    uint32_t panic_mode;   /* 1 once polled emergency output took over   */
    uint32_t panic_lost;   /* bytes lost in panic mode (TXE spin expiry) */
};
/* Caller holds global IRQ mask; never waits for DMA or USART. */
int wch_log_can_sleep(void);
void wch_log_get_stats(struct wch_log_stats *out);

/* One object guarantees that the canaries bracket the DMA buffer even
 * when the compiler/linker reorder independent data sections. */
#define WCH_LOG_RING_SIZE 1024u
struct wch_log_storage {
    uint32_t canary_lo[4];
    char ring[WCH_LOG_RING_SIZE];
    uint32_t canary_hi[4];
};
extern struct wch_log_storage g_wch_log_storage;
#define g_wch_log_canary_lo (g_wch_log_storage.canary_lo)
#define g_wch_log_canary_hi (g_wch_log_storage.canary_hi)

#ifdef __cplusplus
}
#endif

#endif /* H_WCH_LOG_H_ */
