#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include "wch_power_math.h"

/* Check split intervals against an independent, wide cumulative reference. */
int main(void)
{
    const uint32_t hz[] = {1000000, 72000000, 96000000, 144000000};
    const uint32_t rates[] = {12500000, 15993561, 16000000, 16384000, 22500000, 32768000};
    for (unsigned r = 0; r < sizeof(rates) / sizeof(rates[0]); ++r) {
        for (unsigned k = 0; k < sizeof(hz) / sizeof(hz[0]); ++k) {
            uint32_t remainder = 0, seed = 17;
            uint64_t elapsed = 0, actual = 0;
            for (unsigned i = 0; i < 100000; ++i) {
                seed = seed * 1664525u + 1013904223u;
                uint32_t interval = seed % 32768 + 1;
                elapsed += interval;
                actual += wch_power_scale(interval, hz[k], rates[r], &remainder);
                assert(actual == (__uint128_t)elapsed * hz[k] * 1000 / rates[r]);
                assert(remainder == (__uint128_t)elapsed * hz[k] * 1000 % rates[r]);
            }
        }
    }
    /* The sleep budget starts at the last tick boundary, not hook entry. */
    for (unsigned k = 1; k < sizeof(hz) / sizeof(hz[0]); ++k) {
        uint32_t core = hz[k];
        assert(wch_power_budget_us(15, 0, core, 1000) == 15000);
        assert(wch_power_budget_us(15, core / 2000, core, 1000) == 14500);
        assert(wch_power_budget_us(15, core / 1000 - 1, core, 1000) == 14000);
        assert(wch_power_budget_us(100000, 0, core, 1000) == 10000000);
        assert(wch_power_budget_us(0, 0, core, 1000) == 0);
        assert(wch_power_budget_us(1, core / 1000, core, 1000) == 0);
    }
    assert(wch_power_scale(160000, 144000000, 16000000, &(uint32_t){0}) == 1440000000ULL);
    /* RTC counter rollover and 1 MHz cputime rollover are independent. */
    uint32_t before = UINT32_MAX - 100, after = 50, remainder = 0;
    assert(after - before == 151);
    uint32_t dt = wch_power_scale(after - before, 1000000, 32768000, &remainder);
    uint32_t timer = UINT32_MAX - 40;
    assert(dt == 4608);
    assert((uint32_t)(timer + dt) == 4567);
    assert(wch_power_scale(32768, 1000000, 32768000, &(uint32_t){0}) == 1000000);
    puts("power math: fractional conversion / 72,96,144 MHz / counter wraps PASS");
}
