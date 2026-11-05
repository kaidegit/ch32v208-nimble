#ifndef WCH_POWER_H
#define WCH_POWER_H

#include <stdint.h>

/* Called from the app task before BLE starts. Includes the mandatory
 * 10-second reflash window and initializes the owned RTC/backup domain. */
void wch_power_init(void);

/* Allow STOP only after the BLE startup sequence has completed. */
void wch_power_start(void);

/* FreeRTOS tickless hook: scheduler suspended on entry. The implementation
 * masks IRQ responses until clocks, TIM3 and kernel time are restored. */
void wch_power_idle(uint32_t expected_ticks);

struct wch_power_stats {
    uint32_t sleeps;
    uint32_t slept_us; /* Cumulative microseconds, wraps at 32 bits. */
    uint32_t aborts;
    uint32_t overruns; /* Recovery crossed the kernel unblock deadline. */
    uint32_t rtc_ready;
    uint32_t rtc_millihz; /* RTC frequency after its divide-by-two prescaler. */
    uint32_t calibrations;
    uint32_t calibration_errors;
    uint32_t recovery_max_us;
    uint32_t short_sleeps;
};

extern volatile struct wch_power_stats g_wch_power;

#endif
