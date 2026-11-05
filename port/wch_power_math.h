#ifndef WCH_POWER_MATH_H
#define WCH_POWER_MATH_H

#include <stdint.h>

/* Convert intervals with a carried remainder: splitting the same elapsed
 * RTC time across thousands of sleeps must not discard fractional units. */
static inline uint64_t wch_power_scale(uint32_t rtc_ticks, uint32_t target_hz, uint32_t rtc_millihz,
                                       uint32_t *remainder)
{
    uint64_t scaled_ticks = (uint64_t)rtc_ticks * target_hz * 1000u + *remainder;
    *remainder = scaled_ticks % rtc_millihz;
    return scaled_ticks / rtc_millihz;
}

/* RTOS expected_ticks is measured from the last kernel tick boundary.
 * Subtract the current tick phase before selecting an RTC deadline. */
static inline uint32_t wch_power_budget_us(uint32_t expected_ticks, uint64_t phase_cycles,
                                           uint32_t core_hz, uint32_t tick_hz)
{
    uint32_t limited_ticks = expected_ticks > 10u * tick_hz ? 10u * tick_hz : expected_ticks;
    uint64_t cycles = (uint64_t)limited_ticks * (core_hz / tick_hz);
    if (phase_cycles >= cycles) {
        return 0;
    }
    return (cycles - phase_cycles) * 1000000u / core_hz;
}

#endif
