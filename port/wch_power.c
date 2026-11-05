/* CH32V208 FreeRTOS tickless: RTC event -> WFE with IRQs masked.
 * No ISR or context switch can run on the HSI wake clock. See docs/low-power.md.
 */
#include "FreeRTOS.h"
#include "task.h"
#include "ch32v20x_pwr.h"
#include "ch32v20x_rtc.h"
#include "ch32v20x_exti.h"
#include "wch_irq.h"
#include "wch_log.h"
#include "wch_power.h"
#include "wch_power_math.h"
#define LOG_TAG "pm"
#include "elog.h"

volatile struct wch_power_stats g_wch_power;

#if CH32_LOW_POWER
/* Boot image has no controller. Weak references keep that image useful as
 * a FreeRTOS-only STOP test while sharing exactly the same tickless code. */
extern uint32_t hal_timer_wch_next_us(void) __attribute__((weak));
extern uint32_t hal_timer_wch_pause(void) __attribute__((weak));
extern void hal_timer_wch_resume(uint32_t) __attribute__((weak));
extern int ble_phy_state_get(void) __attribute__((weak));
extern uint32_t wch_rf_sleep_prepare(void) __attribute__((weak));
extern void wch_rf_sleep_restore(uint32_t) __attribute__((weak));
/* Remainders carry sub-microsecond / sub-cycle RTC fractions across sleeps. */
static uint32_t sleep_ready;
static uint32_t rtc_us_remainder;
static uint32_t rtc_cycle_remainder;
#define RTC_HZ_MILLI (g_wch_power.rtc_millihz)
#define WAKE_GUARD_US CH32_POWER_WAKE_GUARD_US
#define MIN_STOP_US 10000u
#define BOOT_WINDOW_SECONDS 10u
#define LSI_MEASUREMENT_INTERVAL_MS 10000u
#define LSI_MEASUREMENT_TIMEOUT_MS 50u
#define LSI_CAPTURE_CYCLES 128u
#define LSI_COUNTER_MODULUS 16384u

/* PFIC sleep policy bits; these do not disable any peripheral wake source. */
enum {
    PFIC_SLEEP_ON_EXIT = 1u << 1,
    PFIC_SLEEP_DEEP = 1u << 2,
    PFIC_EVENT_ON_PENDING = 1u << 4,
};

static int pending_irq(void)
{
    /* Only enabled pending sources matter. Event-only RTC has no PFIC IRQ. */
    for (unsigned i = 0; i < 4; ++i) {
        if (NVIC->IPR[i] & NVIC->ISR[i]) {
            return 1;
        }
    }
    return 0;
}

/* Short idle waits keep HSE/PLL, SysTick and TIM3 running. New pending
 * interrupts become events while MIE/MPIE remain masked. Consume an old
 * event BEFORE checking pending status, then use exactly one real WFE:
 * an interrupt arriving between the check and WFE leaves a latched event. */
static void short_idle_wfe(void)
{
    uint32_t rf_enabled = wch_rf_sleep_prepare ? wch_rf_sleep_prepare() : 0;
    uint32_t saved = NVIC->SCTLR;
    NVIC->SCTLR = (saved & ~(PFIC_SLEEP_ON_EXIT | PFIC_SLEEP_DEEP)) | PFIC_EVENT_ON_PENDING;
    _SEV();
    _WFE();
    if (!pending_irq()) {
        ++g_wch_power.short_sleeps;
        _WFE();
    }
    NVIC->SCTLR = saved;

    if (wch_rf_sleep_restore) {
        wch_rf_sleep_restore(rf_enabled);
    }
}

#if !CH32_POWER_LSE
static TickType_t lsi_measure_due, lsi_measure_started;
static unsigned lsi_measuring, lsi_discard_first;

static void lsi_measure_begin(void)
{
    uint32_t mask = wch_irq_save();
    OSC->LSI32K_CAL_CTRL &= ~(RB_OSC_CAL_EN | RB_OSC_CAL_INT_EN);
    OSC->LSI32K_CAL_CFG = Level_128; /* 128 CK32K periods at HCLK */
    OSC->LSI32K_CAL_STATR |= RB_OSC_CAL_IF_END | RB_OSC_CAL_CNT_OV;
    OSC->LSI32K_CAL_CTRL |= RB_OSC_CAL_EN;
    lsi_measuring = 1;
    lsi_discard_first = 1;
    lsi_measure_started = xTaskGetTickCount();
    wch_irq_restore(mask);
}

/* Return false while HSE is still needed for the asynchronous capture.
 * A refresh failure disables STOP rather than trusting a stale RC rate. */
static int lsi_measure_poll(void)
{
    TickType_t now = xTaskGetTickCount();
    if (!lsi_measuring) {
        if ((int32_t)(now - lsi_measure_due) < 0) {
            return 1;
        }
        lsi_measure_begin();
    }
    uint32_t mask = wch_irq_save();
    if ((OSC->LSI32K_CAL_CTRL & RB_OSC_CAL_HALT) && (OSC->LSI32K_CAL_STATR & RB_OSC_CAL_IF_END)) {
        if (lsi_discard_first) {
            lsi_discard_first = 0;
            OSC->LSI32K_CAL_STATR |= RB_OSC_CAL_IF_END | RB_OSC_CAL_CNT_OV;
        } else {
            OSC->LSI32K_CAL_CTRL &= ~RB_OSC_CAL_EN;
            uint32_t count = (OSC->LSI32K_CAL_STATR & RB_OSC_CAL_CNT) +
                             (uint32_t)OSC->LSI32K_CAL_OV_CNT * LSI_COUNTER_MODULUS;
            uint32_t lsi_millihz =
                count ? (uint64_t)LSI_CAPTURE_CYCLES * SystemCoreClock * 1000u / count : 0;
            if (lsi_millihz >= 25000000u && lsi_millihz <= 45000000u) {
                /* RTC prescaler=1 divides the raw LSI clock by two. */
                uint32_t rtc_millihz = lsi_millihz / 2;
                uint32_t previous_millihz = RTC_HZ_MILLI;
                if (previous_millihz) {
                    rtc_us_remainder = (uint64_t)rtc_us_remainder * rtc_millihz / previous_millihz;
                    rtc_cycle_remainder =
                        (uint64_t)rtc_cycle_remainder * rtc_millihz / previous_millihz;
                }
                g_wch_power.rtc_millihz = rtc_millihz;
                ++g_wch_power.calibrations;
            } else {
                ++g_wch_power.calibration_errors;
                sleep_ready = 0;
                g_wch_power.rtc_ready = 0;
            }
            lsi_measuring = 0;
            lsi_measure_due = now + pdMS_TO_TICKS(LSI_MEASUREMENT_INTERVAL_MS);
        }
    }
    if (lsi_measuring && now - lsi_measure_started >= pdMS_TO_TICKS(LSI_MEASUREMENT_TIMEOUT_MS)) {
        OSC->LSI32K_CAL_CTRL &= ~RB_OSC_CAL_EN;
        lsi_measuring = 0;
        lsi_measure_due = now + pdMS_TO_TICKS(LSI_MEASUREMENT_INTERVAL_MS);
        ++g_wch_power.calibration_errors;
        g_wch_power.rtc_ready = 0;
        sleep_ready = 0; /* a failed refresh must not silently use an old RC rate */
    }
    wch_irq_restore(mask);
    return !lsi_measuring;
}
#endif

static void park_unused_io(void)
{
    /* Board-specific bring-up wiring: UART1 TX, HSE, LSE are retained.
     * SWJ disable is delayed until after the mandatory reflash window. */
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOA | RCC_APB2Periph_GPIOB | RCC_APB2Periph_GPIOC |
                               RCC_APB2Periph_GPIOD | RCC_APB2Periph_AFIO,
                           ENABLE);
#if !WCH_POWER_DEBUG
    GPIO_PinRemapConfig(GPIO_Remap_SWJ_Disable, ENABLE);
#endif
    GPIO_InitTypeDef pin = {0};
#if CH32_POWER_GPIO_ANALOG
    pin.GPIO_Mode = GPIO_Mode_AIN;
#else
    pin.GPIO_Mode = GPIO_Mode_IPD;
#endif
    pin.GPIO_Speed = GPIO_Speed_2MHz;
    /* Board-specific masks: keep UART TX, both crystals, and optionally SWJ.
     * A product must also exclude its active peripheral and wakeup pins. */
    pin.GPIO_Pin = GPIO_Pin_All & ~GPIO_Pin_9;
#if WCH_POWER_DEBUG
    pin.GPIO_Pin &= ~(GPIO_Pin_13 | GPIO_Pin_14);
#endif
    GPIO_Init(GPIOA, &pin);
    pin.GPIO_Pin = GPIO_Pin_All;
    GPIO_Init(GPIOB, &pin);
    pin.GPIO_Pin = GPIO_Pin_All & ~(GPIO_Pin_14 | GPIO_Pin_15);
    GPIO_Init(GPIOC, &pin);
    pin.GPIO_Pin = GPIO_Pin_All & ~(GPIO_Pin_0 | GPIO_Pin_1);
    GPIO_Init(GPIOD, &pin);
}

void wch_power_init(void)
{
    for (unsigned i = BOOT_WINDOW_SECONDS; i; --i) {
        log_i("reflash window %us; STOP and SWJ disable deferred", i);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    RCC_APB1PeriphClockCmd(RCC_APB1Periph_PWR | RCC_APB1Periph_BKP, ENABLE);
    PWR_BackupAccessCmd(ENABLE);
    /* This test firmware owns RTC and backup domain; no wall clock stored. */
    RCC_BackupResetCmd(ENABLE);
    RCC_BackupResetCmd(DISABLE);
#if CH32_POWER_LSE
    PWR_BackupAccessCmd(ENABLE);
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOC, ENABLE);
    GPIO_InitTypeDef crystal = {0};
    crystal.GPIO_Pin = GPIO_Pin_14 | GPIO_Pin_15;
    crystal.GPIO_Mode = GPIO_Mode_AIN;
    GPIO_Init(GPIOC, &crystal);
    RCC_LSEConfig(RCC_LSE_ON);
    for (unsigned i = 0; !(RCC->BDCTLR & RCC_LSERDY); ++i) {
        if (i == 500) {
            log_e("LSE failed: STOP disabled");
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    RCC_RTCCLKConfig(RCC_RTCCLKSource_LSE);
    RCC_LSICmd(DISABLE);
    g_wch_power.rtc_millihz = 16384000u;
#else
    RCC_LSEConfig(RCC_LSE_OFF);
    RCC_LSICmd(ENABLE);
    for (unsigned i = 0; !(RCC->RSTSCKR & RCC_LSIRDY); ++i) {
        if (i == 100) {
            log_e("LSI failed: STOP disabled");
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    RCC_RTCCLKConfig(RCC_RTCCLKSource_LSI);
#endif
    RCC_RTCCLKCmd(ENABLE);
    RTC_WaitForSynchro();
    RTC_WaitForLastTask();
    RTC_SetPrescaler(1); /* WCH BLE HAL divide-by-two configuration */
    RTC_WaitForLastTask();
    RTC_SetCounter(0);
    RTC_WaitForLastTask();
    RTC_ITConfig(RTC_IT_ALR, ENABLE);
    RTC_WaitForLastTask();
    NVIC_DisableIRQ(RTCAlarm_IRQn);
    EXTI->INTENR &= ~EXTI_Line17;
    EXTI->EVENR |= EXTI_Line17;
    EXTI->RTENR |= EXTI_Line17;
    EXTI->FTENR &= ~EXTI_Line17;
#if !CH32_POWER_LSE
    /* Frequency measurement uses the same hardware as WCH Calibration_LSI,
     * without changing trim or pausing BLE for the vendor's polling loops. */
    lsi_measure_begin();
    while (!lsi_measure_poll()) {
        taskYIELD();
    }
    if (!g_wch_power.rtc_millihz) {
        log_e("LSI reference capture failed: STOP disabled");
        return;
    }
#endif
    g_wch_power.rtc_ready = 1;
    park_unused_io();
    log_i("RTC %s %u mHz; event STOP armed after BLE init", CH32_POWER_LSE ? "LSE" : "LSI/HSE",
          (unsigned)RTC_HZ_MILLI);
}
void wch_power_start(void)
{
    sleep_ready = g_wch_power.rtc_ready;
}

void wch_power_idle(uint32_t expected_ticks)
{
    if (!sleep_ready || expected_ticks < 2) {
        return;
    }

#if !CH32_POWER_LSE
    if (!lsi_measure_poll()) {
        return; /* HSE must run until capture completes */
    }
#endif
    uint32_t mask = wch_irq_save();
    if (eTaskConfirmSleepModeStatus() == eAbortSleep) {
        goto abort;
    }
    if (pending_irq()) {
        goto abort;
    }
    if (!wch_log_can_sleep()) {
        goto abort;
    }
    if (ble_phy_state_get && ble_phy_state_get() != 0) {
        goto abort;
    }

    /* 1. Select the earlier kernel/radio deadline, accounting for the
     * current SysTick phase. Short windows keep the normal clock running. */
    uint32_t budget_us =
        wch_power_budget_us(expected_ticks, SysTick->CNT, SystemCoreClock, configTICK_RATE_HZ);
    if (hal_timer_wch_next_us) {
        uint32_t radio_budget = hal_timer_wch_next_us();
        if (radio_budget < budget_us) {
            budget_us = radio_budget;
        }
    }
    if (budget_us < MIN_STOP_US + WAKE_GUARD_US) {
        short_idle_wfe();
        wch_irq_restore(mask);
        return;
    }

    /* 2. Arm the RTC event while IRQ responses remain masked. After this
     * point, abort if a tick/IRQ arrived or too little alarm time remains. */
    uint32_t alarm = RTC_GetCounter() +
                     (uint32_t)((uint64_t)(budget_us - WAKE_GUARD_US) * RTC_HZ_MILLI / 1000000000u);
    RTC_ClearITPendingBit(RTC_IT_ALR);
    RTC_WaitForLastTask();
    EXTI_ClearITPendingBit(EXTI_Line17);
    RTC_SetAlarm(alarm);
    RTC_WaitForLastTask();
    /* Align both ends to RTC edges, avoiding a discarded partial RTC tick
     * on EVERY sleep. Keep fractional microseconds and CPU cycles as well. */
    uint32_t before = RTC_GetCounter(), edge;
    uint32_t spins = SystemCoreClock / 1000;
    do {
        edge = RTC_GetCounter();
        if (!--spins) {
            sleep_ready = 0;
            goto abort;
        }
    } while (edge == before);
    if (pending_irq() || SysTick->SR ||
        (int32_t)(alarm - edge) < (int32_t)(RTC_HZ_MILLI / 1000000)) {
        goto abort;
    }

    if (hal_timer_wch_pause) {
        (void)hal_timer_wch_pause();
    }
    uint32_t tick_ctl = SysTick->CTLR;
    SysTick->CTLR = 0;
    uint64_t phase = SysTick->CNT;
    uint64_t tick_cmp = SysTick->CMP;
    /* A tick can assert between the precheck and disabling its counter.
     * Resume without changing the timebase; leave the pending tick alone. */
    if (SysTick->SR || pending_irq()) {
        if (hal_timer_wch_resume) {
            hal_timer_wch_resume(0);
        }
        SysTick->CTLR = tick_ctl;
        goto abort;
    }
    uint32_t sctlr = NVIC->SCTLR;
    NVIC->SCTLR &= ~(PFIC_SLEEP_ON_EXIT | PFIC_EVENT_ON_PENDING);
    uint32_t rf_enabled = wch_rf_sleep_prepare ? wch_rf_sleep_prepare() : 0;

    /* 3. Sleep with TIM3/SysTick stopped. RTC EXTI17 is an event source,
     * so WFE can return without an ISR running on the HSI wake clock. */
    PWR_EnterSTOPMode_RAM_LV(PWR_Regulator_LowPower, PWR_STOPEntry_WFE);
    /* IRQ mask remains held from admission through timebase restoration. */

    /* 4. Restore HSE/PLL first, then both timebases, and only then IRQs. */
    SystemInit();

    before = RTC_GetCounter();
    uint32_t wake;
    spins = SystemCoreClock / 1000;
    do {
        wake = RTC_GetCounter();
        if (!--spins) {
            sleep_ready = 0;
            break;
        }
    } while (wake == before);
    /* Count restoration bookkeeping at HCLK, without generating any tick
     * ISR. RTC register synchronisation below can consume several LSE ticks. */
    SysTick->CNT = 0;
    SysTick->CMP = ~(uint64_t)0;
    SysTick->CTLR = 5; /* Counter enabled, HCLK source, interrupt disabled. */

    int32_t recovery_ticks = (int32_t)(wake - alarm);
    if (recovery_ticks > 0) {
        uint32_t recovery_us = (uint64_t)(uint32_t)recovery_ticks * 1000000000u / RTC_HZ_MILLI;
        if (recovery_us > g_wch_power.recovery_max_us) {
            g_wch_power.recovery_max_us = recovery_us;
        }
    }

    uint32_t rtc_ticks = wake - edge; /* unsigned wrap is intentional */
    uint32_t elapsed_us = wch_power_scale(rtc_ticks, 1000000u, RTC_HZ_MILLI, &rtc_us_remainder);
    if (hal_timer_wch_resume) {
        hal_timer_wch_resume(elapsed_us + (uint32_t)(SysTick->CNT / (SystemCoreClock / 1000000u)));
    }

    uint64_t elapsed_cycles =
        wch_power_scale(rtc_ticks, SystemCoreClock, RTC_HZ_MILLI, &rtc_cycle_remainder);
    uint32_t cycles_per_tick = SystemCoreClock / configTICK_RATE_HZ;

    RTC_ClearITPendingBit(RTC_IT_ALR);
    RTC_WaitForLastTask();

    EXTI_ClearITPendingBit(EXTI_Line17);
    NVIC->SCTLR = sctlr;
    /* Include RTC synchronization/bookkeeping time in the restored tick. */
    uint64_t total = phase + elapsed_cycles + SysTick->CNT;
    uint32_t ticks = total / cycles_per_tick;
    uint32_t step = ticks < expected_ticks ? ticks : expected_ticks - 1;

    vTaskStepTick(step);
    /* If clock restoration overran a deadline, defer the extra ticks to
     * xTaskResumeAll instead of stepping past the kernel's unblock list. */
    if (ticks > step) {
        ++g_wch_power.overruns;
        for (uint32_t i = step; i < ticks; ++i) {
            (void)xTaskIncrementTick();
        }
    }
    SysTick->CTLR = 0;
    SysTick->CMP = tick_cmp;
    SysTick->CNT = total % cycles_per_tick;
    SysTick->SR = 0;
    SysTick->CTLR = tick_ctl;
    if (wch_rf_sleep_restore) {
        wch_rf_sleep_restore(rf_enabled);
    }

    ++g_wch_power.sleeps;
    g_wch_power.slept_us += elapsed_us;
    wch_irq_restore(mask);
    return;
abort:
    ++g_wch_power.aborts;
    wch_irq_restore(mask);
}
#else
void wch_power_init(void)
{
}
void wch_power_start(void)
{
}
void wch_power_idle(uint32_t expected_ticks)
{
    (void)expected_ticks;
}
#endif
