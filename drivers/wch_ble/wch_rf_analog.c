/* RF analog initialization transcribed from wchble_linked.elf.
 * Offsets below are byte offsets verified against RV32 loads/stores, not
 * indices from the inherited RE notes.  No vendor library is linked.
 */
#include <stdint.h>
#include "os/os_cputime.h"

#define RF(off) (*(volatile uint32_t *)(uintptr_t)(0x40025000UL + (off)))
#define BB(off) (*(volatile uint32_t *)(uintptr_t)(0x40024100UL + (off)))
#define LLE(off) (*(volatile uint32_t *)(uintptr_t)(0x40024200UL + (off)))
#define CAL_TIMEOUT_US 10000UL

/* Visible diagnostics retain actual measurements and the failing stage. */
volatile uint32_t g_wch_rf_analog_stage;
volatile uint32_t g_wch_rf_analog_status;
volatile uint32_t g_wch_rf_filter_ready;
volatile uint32_t g_wch_rf_filter_window_expired;
volatile uint8_t g_wch_rf_analog_co[3]; /* 2401, 2440, 2480 MHz */
volatile uint8_t g_wch_rf_analog_ga[3];

static void
settle(void)
{
    __asm volatile ("nop; nop; nop; nop" ::: "memory");
}

/* RFEND_Reset, 0x2ad64..0x2ad96. */
static void
rf_reset(void)
{
    RF(0x0c) = 0x1101;
    for (volatile unsigned i = 0; i < 20; ++i) {
        __asm volatile ("nop");
    }
    RF(0x0c) = 0;
    for (volatile unsigned i = 0; i < 20; ++i) {
        __asm volatile ("nop");
    }
    RF(0x0c) = 0x1101;
}

/* RFEND_DevInit, 0x2ad98..0x2af0c. Preserve individual MMIO writes. */
static void
rf_dev_init(void)
{
    rf_reset();
    RF(0x28) = 0x480;
    RF(0x48) = (RF(0x48) & 0x8fffffffUL) | 0x20000000;
    RF(0x48) = (RF(0x48) & 0xf8ffffffUL) | 0x04000000;
    RF(0x48) = (RF(0x48) & ~0xfUL) | 9;
    RF(0x48) &= 0xfff8ffffUL;
    RF(0x48) |= 0x80000000UL;
    RF(0x4c) = (RF(0x4c) & ~7UL) | 3;
    RF(0x4c) = (RF(0x4c) & ~0x70UL) | 0x30;
    RF(0x4c) = (RF(0x4c) & ~0x700UL) | 0x300;
    RF(0x4c) &= ~0x01000000UL;
    RF(0x4c) |= 0x02000000UL;
    RF(0x50) = (RF(0x50) & 0xffff0fffUL) | 0x4000;
    RF(0x54) = (RF(0x54) & ~0xfUL) | 0xc;
    RF(0x54) |= 0x80;
    RF(0x54) &= ~0x1000UL;
    RF(0x3c) = (RF(0x3c) & 0xffff0fffUL) | 0x8000;
    RF(0x3c) = (RF(0x3c) & 0xf8ffffffUL) | 0x02000000;
    RF(0x3c) = (RF(0x3c) & 0x1fffffffUL) | 0x40000000;
    RF(0x2c) |= 0x00700000;
    RF(0x2c) &= 0xf8ffffffUL;
    RF(0x2c) = (RF(0x2c) & ~0x3000UL) | 0x2000;
    RF(0x2c) = (RF(0x2c) & ~0x30000UL) | 0x20000;
    RF(0x30) &= ~0xfUL;
    RF(0x30) &= ~0xf0UL;
    RF(0x30) &= ~0x700UL;
    RF(0x30) |= 0x00700000;
    RF(0x30) = (RF(0x30) & 0x8fffffffUL) | 0x50000000;
    RF(0x38) = (RF(0x38) & 0xff07ffffUL) | 0x00100000;
    RF(0x38) |= 0x80000000UL;
}

/* RFEND_WaitTune, 0x34052..0x3408a, checks BOTH ready bits in RF+0x90.
 * The vendor silently returns when its 3000 us LLE window expires. Here
 * expiry is an error; it must not produce a table from unfinished readings.
 */
static int
wait_tune(void)
{
    uint32_t started = os_cputime_get32();
    LLE(0x64) = 6000;
    for (;;) {
        uint32_t st = RF(0x90);
        g_wch_rf_analog_status = st;
        if ((st & 0x06000000UL) == 0x06000000UL) {
            return 0;
        }
        if (LLE(0x64) == 0 ||
            (uint32_t)(os_cputime_get32() - started) >= CAL_TIMEOUT_US) {
            return -1;
        }
    }
}

static void
nibble_write(unsigned base, unsigned index, int value)
{
    unsigned off = base + (index / 8U) * 4U;
    unsigned shift = (index % 8U) * 4U;
    uint32_t mask = 0xfUL << shift;
    RF(off) = (RF(off) & ~mask) | (((uint32_t)value & 0xfUL) << shift);
}

/* RISC-V DIV returns -1 for a zero divisor; preserve that vendor behavior
 * explicitly, avoiding undefined C division. Equal CO samples can be valid.
 */
static int
tune_div(int numerator, int denominator)
{
    return denominator == 0 ? -1 : numerator / denominator;
}

/* RFEND_TXCtune, 0x2b196..0x2bec4. Calibration table stores signed offsets
 * from the 2440 MHz baseline, with 1 MHz spacing. The original unrolled
 * stores cover 82 CO nibbles (RF+A0..C8), then 21 GA nibbles (C8..D0).
 */
static int
rf_txctune(void)
{
    static const uint8_t bands[] = { 0xbf, 0xe7, 0xd3 };
    static const uint8_t sample_index[] = { 0, 2, 1 };
    RF(0x04) &= ~0x100UL;
    RF(0x28) &= ~0x1000UL;
    RF(0x2c) &= ~0x10UL;
    RF(0x08) |= 0x20000;
    RF(0x04) |= 0x10;
    for (unsigned i = 0; i < 3; ++i) {
        unsigned sample = sample_index[i];
        g_wch_rf_analog_stage = 2 + i;
        RF(0x04) &= ~1UL;
        RF(0x38) = (RF(0x38) & 0xfffe00ffUL) |
                   ((uint32_t)bands[i] << 8);
        RF(0x04) |= 1;
        if (wait_tune() != 0) {
            RF(0x04) &= ~0x11UL;
            RF(0x28) |= 0x1000;
            RF(0x2c) |= 0x10;
            return -1;
        }
        g_wch_rf_analog_co[sample] = RF(0x90) & 0x3f;
        g_wch_rf_analog_ga[sample] = (RF(0x94) >> 10) & 0x7f;
    }
    int co_low = (int)g_wch_rf_analog_co[0] - g_wch_rf_analog_co[1];
    int co_high = (int)g_wch_rf_analog_co[1] - g_wch_rf_analog_co[2];
    int ga_low = (int)g_wch_rf_analog_ga[0] - g_wch_rf_analog_ga[1];
    int ga_high = (int)g_wch_rf_analog_ga[1] - g_wch_rf_analog_ga[2];
    for (unsigned i = 0; i < 40; ++i) {
        nibble_write(0xa0, i, co_low * (39 - (int)i) / 39);
    }
    for (unsigned i = 1; i <= 42; ++i) {
        nibble_write(0xa0, 39 + i, co_high * (int)i / 40);
    }
    for (unsigned i = 0; i < 10; ++i) {
        nibble_write(0xc8, 2 + i,
                     tune_div(ga_high * (10 - (int)i), co_high) | 8);
    }
    nibble_write(0xc8, 12, 0);
    for (unsigned i = 1; i <= 10; ++i) {
        nibble_write(0xc8, 12 + i, tune_div(ga_low * (int)i, co_low));
    }
    RF(0x04) &= ~0x10UL;
    RF(0x04) &= ~1UL;
    RF(0x28) |= 0x1000;
    RF(0x2c) |= 0x10;
    RF(0x38) = (RF(0x38) & ~0x3fUL) | g_wch_rf_analog_co[1];
    RF(0x38) = (RF(0x38) & 0x80ffffffUL) |
               ((uint32_t)g_wch_rf_analog_ga[1] << 24);
    return 0;
}

/* RFEND_RXFilter, 0x2afda..0x2b06c. */
static int
rf_rxfilter(void)
{
    g_wch_rf_filter_ready = 0;
    g_wch_rf_filter_window_expired = 0;
    RF(0x50) &= ~0x10000UL;
    RF(0x08) |= 0x200000;
    RF(0x0c) |= 0x10;
    RF(0x0c) &= ~0x10UL;
    settle();
    RF(0x0c) |= 0x10;
    RF(0x04) |= 0x1000;
    uint32_t started = os_cputime_get32();
    LLE(0x64) = 80;
    for (;;) {
        uint32_t st = RF(0x9c);
        g_wch_rf_analog_status = st;
        if (st & 0x100) {
            g_wch_rf_filter_ready = 1;
            break;
        }
        /* Vendor 0x2b030..2b03e exits on countdown OR ready and always
         * applies the low-five-bit result. Preserve that bounded fallback
         * while recording it; sequence completion is not proof of ready. */
        if (LLE(0x64) == 0) {
            g_wch_rf_filter_window_expired = 1;
            break;
        }
        if ((uint32_t)(os_cputime_get32() - started) >= CAL_TIMEOUT_US) {
            RF(0x08) &= ~0x200000UL;
            return -1;
        }
    }
    settle();
    uint32_t result = RF(0x9c);
    RF(0x50) |= 0x10000;
    RF(0x50) = (RF(0x50) & ~0x1fUL) | (result & 0x1f);
    RF(0x08) &= ~0x200000UL;
    return 0;
}

/* RFEND_RXAdc, 0x2b06e..0x2b0a6. It has no completion poll in vendor. */
static void
rf_rxadc(void)
{
    RF(0x58) &= ~0x10000UL;
    RF(0x08) |= 0x10000;
    RF(0x0c) &= ~0x100UL;
    RF(0x04) &= ~0x10000UL;
    RF(0x0c) |= 0x100;
    RF(0x04) |= 0x10000;
}

/* Call after BLE AHB clock, cputime, LLE_DevInit and BB_DevInit are ready.
 * Calibration power/PHY envelope: BLE_RegInit 0x13e24..0x13eae.
 * Return -1/-2/-3 for low/high/mid VCO readiness, -4 for filter readiness.
 */
int
wch_rf_analog_init(void)
{
    uint32_t saved_mask = LLE(0x0c);
    int rc;
    LLE(0x0c) = 0;
    __asm volatile ("fence.i" ::: "memory");
    g_wch_rf_analog_stage = 1;
    rf_dev_init();
    BB(0) = (BB(0) & ~0x3000UL) | 0x1000;
    BB(0) &= ~0x180UL;
    BB(0) = (BB(0) & ~0x180UL) | 0x100;
    RF(8) |= 0x330000;
    LLE(0x50) = 0x5d;
    rc = rf_txctune();
    if (rc != 0) {
        rc = -(int)(g_wch_rf_analog_stage - 1);
        goto out;
    }
    g_wch_rf_analog_stage = 5;
    RF(4) |= 0x100; /* RFEND_TXFtune, 0x2bec6..0x2bed8. */
    if (rf_rxfilter() != 0) {
        rc = -4;
        goto out;
    }
    g_wch_rf_analog_stage = 6;
    rf_rxadc();
    g_wch_rf_analog_stage = 7;
out:
    BB(0) = (BB(0) & ~0x180UL) | 0x80;
    RF(8) &= 0xffcdffffUL;
    LLE(0x50) = 0;
    LLE(0x64) = 0;
    __asm volatile ("fence.i" ::: "memory");
    LLE(0x0c) = saved_mask;
    return rc;
}
