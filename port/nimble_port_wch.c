/*
 * nimble_port_wch.c - NimBLE platform init for CH32V208 (FreeRTOS NPL).
 *
 * Replaces porting/nimble/src/nimble_port.c (excluded from nimble_core in
 * CMakeLists.txt) because that file hardcodes os_cputime_init(32768); here
 * the cputime runs at 1 MHz on TIM3 (see port/hal_timer_wch.c).
 *
 * Initialization order (nimble_port_init) and why:
 *   1. ble_npl_eventq_init(&g_eventq_dflt) - host event queue must exist
 *      before anything can post to it.
 *   2. os_mempool_module_init() + os_msys_init() - the global mbuf pools;
 *      transport/LL/host allocation depends on them.
 *   3. hal_timer_init(0, NULL) + os_cputime_init(1000000) - TIM3 @ 1 MHz.
 *      Runs before the hardware bring-up: wch_rf_init() calibration polls
 *      and the RNG seed already consume os_cputime_get32() as their time
 *      base, and the LL scheduler registers its first cputime callbacks
 *      during the ble_transport_ll_init() -> ble_ll_init-complete path.
 *   4. wch_ble_hw_init() - WCH radio bring-up: BB/LLE/RFEND registers,
 *      ble_phy_init() and the AES engine (driver agent, drivers/wch_ble/).
 *      Must precede ble_ll_init(), whose ble_phy_x/ble_hw_x callbacks may
 *      be touched from the LL task once it exists.
 *   5. ble_ll_init() - controller state, LL task event queue, RNG pool.
 *   6. ble_transport_init() - HCI buffer pools (cmd/evt; ACL/ISO pools are
 *      compiled out for the native/native transport).
 *   7. ble_transport_hs_init() - registers host-side HCI dispatch.
 *   8. ble_transport_ll_init() - binds the controller to the transport;
 *      with both sides "native" this is a direct function hookup.
 *
 * This file also hosts the ISR-aware NPL helpers declared in the shadowed
 * config/nimble/nimble_npl_os.h (npl_wch_*): the stock
 * porting/npl/freertos/src/npl_os_freertos.c probes ISR context through an
 * ARM CMSIS SCB read that cannot work on RISC-V, so ISR-sensitive NPL calls
 * are intercepted before they reach it.
 */

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "syscfg/syscfg.h"
#include "os/os.h"
#include "hal/hal_timer.h"
#include "console/console.h"
#include "host/ble_hs.h"
#include "nimble/nimble_port.h"
#include "nimble/transport.h"
#include "nimble/hci_common.h"
#if NIMBLE_CFG_CONTROLLER
#include "controller/ble_ll.h"
#include "os/os_cputime.h"
#endif

/* Provided by the driver agent (drivers/wch_ble/ble_hw_wch.c):
 * RF/BB/LLE/AES bring-up + ble_phy_init(). */
extern void wch_ble_hw_init(void);

/* Bring-up forensics: step-localizing tripwire checker (app/main.c). */
extern void app_dbg_chk(const char *tag);

static struct ble_npl_eventq g_eventq_dflt;

extern void os_msys_init(void);
extern void os_mempool_module_init(void);
#if NIMBLE_CFG_CONTROLLER
extern void ble_ll_init(void);
#endif

/* ------------------------------------------------- NPL ISR-safe helpers */

bool
npl_wch_in_isr(void)
{
    /* mcause describes the most recent trap and remains set after mret.
     * PFIC_GISR.GACTSTA (QingKe V4 manual, bit 8) reports a live ISR. */
    return (*(volatile const uint32_t *)0xE000E04CUL & (1UL << 8)) != 0;
}

struct ble_npl_event *
npl_wch_eventq_get(struct ble_npl_eventq *evq, ble_npl_time_t tmo)
{
    struct ble_npl_event *ev = NULL;
    BaseType_t woken = pdFALSE;
    BaseType_t ret;

    if (npl_wch_in_isr()) {
        configASSERT(tmo == 0);
        ret = xQueueReceiveFromISR(evq->q, &ev, &woken);
        portYIELD_FROM_ISR(woken);
        assert(ret == pdPASS || ret == errQUEUE_EMPTY);
        (void)ret;
    } else {
        return npl_freertos_eventq_get(evq, tmo);
    }
    if (ev) {
        ev->queued = false;
    }
    return ev;
}

void
npl_wch_eventq_put(struct ble_npl_eventq *evq, struct ble_npl_event *ev)
{
    BaseType_t woken = pdFALSE;

    if (ev->queued) {
        return;
    }

    if (npl_wch_in_isr()) {
        ev->queued = true;
        (void)xQueueSendToBackFromISR(evq->q, &ev, &woken);
        portYIELD_FROM_ISR(woken);
    } else {
        npl_freertos_eventq_put(evq, ev);
    }
}

void
npl_wch_eventq_remove(struct ble_npl_eventq *evq, struct ble_npl_event *ev)
{
    struct ble_npl_event *tmp_ev;
    BaseType_t woken = pdFALSE;
    BaseType_t woken2 = pdFALSE;
    BaseType_t ret;
    int i;
    int count;

    if (!ev->queued) {
        return;
    }

    if (npl_wch_in_isr()) {
        /* Same drain-and-refill workaround as the stock implementation. */
        count = uxQueueMessagesWaitingFromISR(evq->q);
        for (i = 0; i < count; i++) {
            ret = xQueueReceiveFromISR(evq->q, &tmp_ev, &woken2);
            assert(ret == pdPASS);
            (void)ret;
            woken |= woken2;

            if (tmp_ev == ev) {
                continue;
            }

            ret = xQueueSendToBackFromISR(evq->q, &tmp_ev, &woken2);
            assert(ret == pdPASS);
            (void)ret;
            woken |= woken2;
        }
        portYIELD_FROM_ISR(woken);
    } else {
        npl_freertos_eventq_remove(evq, ev);
        return;
    }

    ev->queued = 0;
}

ble_npl_error_t
npl_wch_sem_pend(struct ble_npl_sem *sem, ble_npl_time_t timeout)
{
    BaseType_t woken = pdFALSE;
    BaseType_t ret;

    if (npl_wch_in_isr()) {
        configASSERT(timeout == 0);
        ret = xSemaphoreTakeFromISR(sem->handle, &woken);
        portYIELD_FROM_ISR(woken);
        return ret == pdPASS ? BLE_NPL_OK : BLE_NPL_TIMEOUT;
    }

    return npl_freertos_sem_pend(sem, timeout);
}

ble_npl_error_t
npl_wch_sem_release(struct ble_npl_sem *sem)
{
    BaseType_t woken = pdFALSE;

    if (npl_wch_in_isr()) {
        (void)xSemaphoreGiveFromISR(sem->handle, &woken);
        portYIELD_FROM_ISR(woken);
        return BLE_NPL_OK;
    }

    return npl_freertos_sem_release(sem);
}

ble_npl_error_t
npl_wch_callout_reset(struct ble_npl_callout *co, ble_npl_time_t ticks)
{
    BaseType_t woken1 = pdFALSE, woken2 = pdFALSE, woken3 = pdFALSE;

    if (ticks == 0) {
        ticks = 1;
    }

    if (npl_wch_in_isr()) {
        xTimerStopFromISR(co->handle, &woken1);
        xTimerChangePeriodFromISR(co->handle, ticks, &woken2);
        xTimerResetFromISR(co->handle, &woken3);
        portYIELD_FROM_ISR(woken1 || woken2 || woken3);
        return BLE_NPL_OK;
    }

    return npl_freertos_callout_reset(co, ticks);
}





extern void os_msys_init(void);
extern void os_mempool_module_init(void);
#if NIMBLE_CFG_CONTROLLER
extern void ble_ll_init(void);
#endif

/* ----------------------------------------------------- nimble_port API */

void
nimble_port_init(void)
{
    /* Breadcrumbs (np:1..8): the bring-up saw the app-task stack pattern
     * destroyed somewhere inside this function; these localize the step. */
    console_printf("np:1 eventq\n");

    /* 1. default (host) event queue */
    ble_npl_eventq_init(&g_eventq_dflt);
    app_dbg_chk("np:1");

    console_printf("np:2 msys\n");
    /* 2. global mbuf pools */
    os_mempool_module_init();
    os_msys_init();
    app_dbg_chk("np:2");

#if NIMBLE_CFG_CONTROLLER
    console_printf("np:3 cputime\n");
    /* 3. cputime on TIM3 @ 1 MHz (replaces the stock 32768 Hz init).
     * Must run before wch_ble_hw_init(): the RF driver's calibration polls
     * (poll_reg) and the RNG seed use os_cputime_get32() as their time
     * base - with TIM3 unconfigured it reads a constant 0 and no poll
     * timeout can ever fire. */
    int timer_rc = hal_timer_init(MYNEWT_VAL(OS_CPUTIME_TIMER_NUM), NULL);
    configASSERT(timer_rc == 0);
    timer_rc = os_cputime_init(MYNEWT_VAL(OS_CPUTIME_FREQ));
    configASSERT(timer_rc == 0);
    app_dbg_chk("np:3");

    console_printf("np:4 hw\n");
    /* 4. radio + PHY bring-up (BB/LLE/RFEND/AES registers) */
    wch_ble_hw_init();

    console_printf("np:5 ll\n");
    /* 5. controller */
    ble_ll_init();
    app_dbg_chk("np:5");
#endif

    console_printf("np:6 transport\n");
    /* 6. HCI buffer pools */
    ble_transport_init();
    app_dbg_chk("np:6");

    console_printf("np:7 hs\n");
    /* 7. host side of the transport */
    ble_transport_hs_init();

#if NIMBLE_CFG_CONTROLLER
    console_printf("np:8 ll-tr\n");
    /* 8. controller side: send the initial command-credit NOP. */
    ble_transport_ll_init();
#endif

    console_printf("np: done\n");
}

void
nimble_port_run(void)
{
    struct ble_npl_event *ev;

    while (1) {
        ev = ble_npl_eventq_get(&g_eventq_dflt, BLE_NPL_TIME_FOREVER);
        ble_npl_event_run(ev);
    }
}

struct ble_npl_eventq *
nimble_port_get_dflt_eventq(void)
{
    return &g_eventq_dflt;
}

#if NIMBLE_CFG_CONTROLLER
void
nimble_port_ll_task_func(void *arg)
{
    extern void ble_ll_task(void *);

    ble_ll_task(arg);
}
#endif
