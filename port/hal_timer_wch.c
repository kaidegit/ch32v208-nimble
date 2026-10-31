/*
 * hal_timer_wch.c - NimBLE hal/hal_timer.h API on CH32V208 TIM3.
 *
 * Single-instance port (OS_CPUTIME_TIMER_NUM == 0): every hal_timer_*
 * entry point addresses the one and only TIM3.  This mirrors the queue
 * architecture of porting/nimble/src/hal_timer.c (nRF): an ordered TAILQ
 * of struct hal_timer, CH1 output-compare programmed to the head expiry,
 * and the ISR popping expired entries and re-programming CH1CVR.
 *
 * Register-level timer operation; RCC_GetClocksFreq reads the clock tree:
 *   - TIM3 clock: RCC->APB1PCENR bit1 (RCC_TIM3EN).
 *   - Counter clock: PCLK1 x 2 (x2 applies because APB1 is always
 *     prescaled on this design; the x1 case is handled anyway), derived
 *     at runtime from RCC_GetClocksFreq and RCC->CFGR PPRE1[10:8]. A clock
 *     that cannot divide exactly to the requested tick rate is rejected.
 *   - PSC -> 1 MHz, ATRLR = 0xFFFF (free-running 16-bit).
 *   - UIE: update interrupt bumps g_high_word -> 32-bit software time.
 *   - CC1IE + CH1CVR: one-shot compare driving the callback queue.
 *
 * Concurrency model: the FreeRTOS WCH port implements critical sections
 * by clearing MIE/MPIE through GINTENR (vPortEnterCritical), so *any*
 * interrupt priority is safe with respect to FreeRTOS and with respect to
 * this timer's list.  Because these helpers are also called from the TIM3
 * ISR itself, saved-state locks are used instead of changing the task's
 * nesting counter. QingKe does not automatically clear MIE on ISR entry;
 * preserving both MIE/MPIE also preserves the state restored by MRET.
 *
 * NVIC: TIM3_IRQn == 45.  Priority programmed directly into PFIC->IPRIOR
 * (QingKe V4: 1 byte per IRQ; per the WCH SDK ch32v20x_misc.c convention
 * bit7 = pre-emption priority, bits[6:5] = subpriority; 0x40 = preempt 0 /
 * sub 2).  Enable via PFIC->IENR (core_riscv.h NVIC_EnableIRQ static
 * inline - a bare register store, not a library call).
 */

#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <string.h>

#include "ch32v20x.h"
#include "syscfg/syscfg.h"
#include "hal/hal_timer.h"
#include "hal_timer_wch.h"
#include "wch_irq.h"

#define WCH_TIM_PRESCALE_HZ     (1000000u)      /* target cputime clock */

/* PFIC priority byte for TIM3: preempt 0, subpriority 2 (see file comment). */
#define WCH_TIM3_PFIC_PRIO      (0x40)

struct wch_hal_timer {
    uint8_t enabled;
    uint32_t tmr_freq;
    volatile uint32_t high_word;        /* increments on each UIF */
    TAILQ_HEAD(hal_timer_qhead, hal_timer) hal_timer_q;
};

static struct wch_hal_timer g_wch_timer;

/* ------------------------------------------------------------------ lock */

static inline uint32_t
wch_irq_lock(void)
{
    return wch_irq_save();
}

static inline void
wch_irq_unlock(uint32_t mstatus)
{
    wch_irq_restore(mstatus);
}

/* ------------------------------------------------------------- hw access */

static uint32_t
wch_timer_input_freq(void)
{
    RCC_ClocksTypeDef clocks;
    uint32_t ppre1;

    RCC_GetClocksFreq(&clocks);

    ppre1 = RCC->CFGR0 & RCC_PPRE1;     /* bits [10:8] */
    if (ppre1 == RCC_PPRE1_DIV1) {
        return clocks.PCLK1_Frequency;
    }
    /* APB1 prescaled: the timer clock is PCLK1 * 2 */
    return clocks.PCLK1_Frequency * 2U;
}

/* Consume pending overflow under the same IRQ lock as the ISR. PHY start
 * waits can run with IRQs masked, including across a 16-bit wrap. Merely
 * reading high/CNT/high then relies on an ISR that cannot run and can leave
 * the caller waiting forever for a deadline in the next 65536 us epoch.
 * Polling here also supports more than one wrap during a masked busy wait,
 * provided consecutive reads are less than one hardware period apart. */
static uint32_t
wch_timer_read32(void)
{
    uint32_t sr = wch_irq_lock();
    uint32_t hi = g_wch_timer.high_word;
    uint32_t cnt = TIM3->CNT;
    if (TIM3->INTFR & TIM_UIF) {
        TIM3->INTFR = (uint16_t)~TIM_UIF;
        hi = ++g_wch_timer.high_word;
        cnt = TIM3->CNT;
    }
    wch_irq_unlock(sr);
    return (hi << 16) | (cnt & 0xffffU);
}

/* Program CH1 compare for 'expiry' and (re)arm CC1IF interrupt.
 * Must be called with the IRQ lock held. */
static void
wch_timer_set_ocmp(uint32_t expiry)
{
    TIM3->DMAINTENR &= ~TIM_CC1IE;              /* disarm while writing */
    TIM3->CH1CVR = expiry & 0xFFFFu;
    TIM3->INTFR = ~TIM_CC1IF;                   /* write-0-to-clear style: clear CC1IF only */
    TIM3->DMAINTENR |= TIM_CC1IE;

    /* Deadline already passed (or within the ISR latency)?  Force the IRQ
     * through PFIC instead of relying on a compare that can never fire. */
    if ((int32_t)(wch_timer_read32() - expiry) >= 0) {
        NVIC_SetPendingIRQ(TIM3_IRQn);
    }
}

static void
wch_timer_disable_ocmp(void)
{
    TIM3->DMAINTENR &= ~TIM_CC1IE;
}

/* Pop every expired timer from the ordered queue, then re-arm CH1 on the
 * new head (or disarm when the queue drained).  Same shape as the nRF
 * hal_timer_chk_queue(); callbacks run at interrupt context. */
static void
wch_timer_chk_queue(void)
{
    uint32_t sr;
    struct hal_timer *timer;

    sr = wch_irq_lock();
    while ((timer = TAILQ_FIRST(&g_wch_timer.hal_timer_q)) != NULL) {
        if ((int32_t)(wch_timer_read32() - timer->expiry) >= 0) {
            TAILQ_REMOVE(&g_wch_timer.hal_timer_q, timer, link);
            timer->link.tqe_prev = NULL;
            timer->cb_func(timer->cb_arg);
        } else {
            break;
        }
    }

    timer = TAILQ_FIRST(&g_wch_timer.hal_timer_q);
    if (timer) {
        wch_timer_set_ocmp(timer->expiry);
    } else {
        wch_timer_disable_ocmp();
    }
    wch_irq_unlock(sr);
}

/* ----------------------------------------------------------------- IRQ */

void
hal_timer_wch_irq(void)
{
    uint16_t intfr = TIM3->INTFR;

    if (intfr & TIM_UIF) {
        TIM3->INTFR = (uint16_t)~TIM_UIF;       /* clear flag (rc_w1) */
        g_wch_timer.high_word++;
    }

    if (intfr & TIM_CC1IF) {
        TIM3->INTFR = (uint16_t)~TIM_CC1IF;
    }
    /* A past-due deadline pends PFIC without creating a compare flag. */
    wch_timer_chk_queue();
}

/* ---------------------------------------------------------- hal_timer API */

int
hal_timer_init(int timer_num, void *cfg)
{
    (void)cfg;

    /* Single instance: any timer number aliases to the one TIM3. */
    if (timer_num < 0) {
        return EINVAL;
    }
    if (g_wch_timer.enabled) {
        return EINVAL;
    }

    TAILQ_INIT(&g_wch_timer.hal_timer_q);
    g_wch_timer.high_word = 0;
    return 0;
}

int
hal_timer_deinit(int timer_num)
{
    uint32_t sr;

    (void)timer_num;
    sr = wch_irq_lock();
    TIM3->DMAINTENR &= ~(TIM_UIE | TIM_CC1IE);
    TIM3->CTLR1 &= ~TIM_CEN;
    NVIC_DisableIRQ(TIM3_IRQn);
    g_wch_timer.enabled = 0;
    wch_irq_unlock(sr);
    return 0;
}

int
hal_timer_config(int timer_num, uint32_t freq_hz)
{
    uint32_t sr;
    uint32_t tmrclk;
    uint32_t div;

    (void)timer_num;
    if (g_wch_timer.enabled || freq_hz == 0) {
        return EINVAL;
    }

    tmrclk = wch_timer_input_freq();
    if (tmrclk == 0 || tmrclk % freq_hz != 0) {
        return EINVAL; /* LL microseconds require an exact time base */
    }
    div = (tmrclk + freq_hz - 1) / freq_hz;     /* ceil */
    if (div == 0) {
        div = 1;
    }
    if (div > 65536) {
        return EINVAL;                          /* 16-bit prescaler */
    }

    sr = wch_irq_lock();

    RCC->APB1PCENR |= RCC_TIM3EN;               /* TIM3 on APB1 */

    TIM3->CTLR1 = 0;                            /* stopped, edge-aligned, no ARPE */
    TIM3->PSC = (uint16_t)(div - 1);
    TIM3->ATRLR = 0xFFFF;
    TIM3->CNT = 0;

    g_wch_timer.high_word = 0;
    g_wch_timer.tmr_freq = tmrclk / div;
    g_wch_timer.enabled = 1;

    TAILQ_INIT(&g_wch_timer.hal_timer_q);

    /* One compare interrupt per 65.536 ms worst case (UIF) - cheap enough
     * and keeps the 32-bit software counter alive. */
    TIM3->INTFR = (uint16_t)~(TIM_UIF | TIM_CC1IF);
    TIM3->DMAINTENR |= TIM_UIE;                 /* update irq only; CC1 on demand */
    TIM3->SWEVGR = TIM_UG;                      /* load PSC immediately */

    NVIC_SetPriority(TIM3_IRQn, WCH_TIM3_PFIC_PRIO);
    NVIC_EnableIRQ(TIM3_IRQn);

    TIM3->CTLR1 |= TIM_CEN;                     /* run */

    wch_irq_unlock(sr);
    return 0;
}

uint32_t
hal_timer_get_resolution(int timer_num)
{
    (void)timer_num;
    if (!g_wch_timer.enabled || g_wch_timer.tmr_freq == 0) {
        return 0;
    }
    return 1000000000u / g_wch_timer.tmr_freq;
}

uint32_t
hal_timer_read(int timer_num)
{
    (void)timer_num;
    return wch_timer_read32();
}

int
hal_timer_delay(int timer_num, uint32_t ticks)
{
    uint32_t until;

    (void)timer_num;
    until = wch_timer_read32() + ticks;
    while ((int32_t)(wch_timer_read32() - until) < 0) {
        /* busy wait */
    }
    return 0;
}

int
hal_timer_set_cb(int timer_num, struct hal_timer *tmr, hal_timer_cb cb_func,
                 void *arg)
{
    (void)timer_num;
    if (tmr == NULL || cb_func == NULL) {
        return EINVAL;
    }

    tmr->cb_func = cb_func;
    tmr->cb_arg = arg;
    tmr->link.tqe_prev = NULL;
    tmr->bsp_timer = &g_wch_timer;
    return 0;
}

int
hal_timer_start(struct hal_timer *tmr, uint32_t ticks)
{
    return hal_timer_start_at(tmr, wch_timer_read32() + ticks);
}

int
hal_timer_start_at(struct hal_timer *tmr, uint32_t tick)
{
    uint32_t sr;
    struct hal_timer *entry;

    if ((tmr == NULL) || (tmr->link.tqe_prev != NULL) ||
        (tmr->cb_func == NULL)) {
        return EINVAL;
    }
    tmr->expiry = tick;

    sr = wch_irq_lock();

    if (TAILQ_EMPTY(&g_wch_timer.hal_timer_q)) {
        TAILQ_INSERT_HEAD(&g_wch_timer.hal_timer_q, tmr, link);
    } else {
        TAILQ_FOREACH(entry, &g_wch_timer.hal_timer_q, link) {
            if ((int32_t)(tmr->expiry - entry->expiry) < 0) {
                TAILQ_INSERT_BEFORE(entry, tmr, link);
                break;
            }
        }
        if (entry == NULL) {
            TAILQ_INSERT_TAIL(&g_wch_timer.hal_timer_q, tmr, link);
        }
    }

    /* Head changed -> reprogram the compare. */
    if (tmr == TAILQ_FIRST(&g_wch_timer.hal_timer_q)) {
        wch_timer_set_ocmp(tmr->expiry);
    }

    wch_irq_unlock(sr);
    return 0;
}

int
hal_timer_stop(struct hal_timer *tmr)
{
    uint32_t sr;
    int reset_ocmp;
    struct hal_timer *entry;

    if (tmr == NULL) {
        return EINVAL;
    }

    sr = wch_irq_lock();

    if (tmr->link.tqe_prev != NULL) {
        reset_ocmp = 0;
        if (tmr == TAILQ_FIRST(&g_wch_timer.hal_timer_q)) {
            entry = TAILQ_NEXT(tmr, link);
            reset_ocmp = 1;
        }
        TAILQ_REMOVE(&g_wch_timer.hal_timer_q, tmr, link);
        tmr->link.tqe_prev = NULL;
        if (reset_ocmp) {
            if (entry) {
                wch_timer_set_ocmp(entry->expiry);
            } else {
                wch_timer_disable_ocmp();
            }
        }
    }

    wch_irq_unlock(sr);
    return 0;
}
