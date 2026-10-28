/*
 * wch_rf.c - WCH CH32V208 BLE baseband / LLE / RF-AFE register level driver.
 *
 * Every sequence is a faithful transcription of the reverse-engineered
 * libwchble.a programming model documented in
 * WCH_BLE_LIB_RE_FINDINGS.md (referenced as "RE §x.y" below).
 *
 * Time source: os_cputime_get32() only (1 MHz, provided by the platform
 * hal_timer on TIM3).  This driver never touches TIM3 itself.
 */

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include "syscfg/syscfg.h"
#include "os/os_cputime.h"

#include "ch32v20x.h"       /* OSC->HSE_CAL_CTRL, BB_IRQn / LLE_IRQn */
#include "core_riscv.h"     /* NVIC_SetPriority / NVIC_EnableIRQ     */

#include "wch_rf.h"

/* ------------------------------------------------------------------------- */
/* Module state                                                              */
/* ------------------------------------------------------------------------- */

struct wch_rf_timing g_wch_rf_timing = {
    .tx_guard_us = 100,                 /* busy-wait margin - TODO(board): RE §12.10 */
    .tx_late_guard_us = 50,             /* TODO(board): RE §12.10 */
    .wfr_half_us_1m = 0x196,            /* 203 us - RE §4.8/§12.1 ACK/等待窗 */
    .tifs_turnaround_half_us = 0xB4,    /* 90 us nominal - TODO(board): RE §5.3 table is 93..107 */
    .ack_window_half_us_1m = 0x196,     /* RE §4.8 rf_tx_auto_txProcess */
    .ack_window_half_us_2m = 0x196,     /* RE §12.1: 1M/2M both 0x196 */
    .rx_window_widen_half_us = 73,      /* C_PHY @1M - RE §12.1 */
    .tune_timeout_us = 10000,           /* VCO tune / filter measure budget */
    .lo_inject_khz_1m = 1000,           /* low-side LO injection - RE §4.2 (*) */
    .lo_inject_khz_2m = 2000,           /* RE §4.2 (*) */
};

static wch_rf_cb_t s_cb;                /* event callback (ISR context!)      */
static uint32_t s_guard_save;           /* [0x4002420C] saved by guard_enter  */
static uint32_t s_lle_mask = 0xF00F;    /* LLE[0x0C] init value - RE §3.2     */

static uint8_t  s_channel = 0xFF;       /* currently programmed BLE channel   */
static uint32_t s_aa;                   /* access address                     */
static uint32_t s_crcinit;              /* CRC init (24-bit)                  */
static uint32_t s_synth_khz;            /* last programmed synth frequency    */

static uint8_t  s_phy_tx = 1;           /* 1 = 1M, 2 = 2M (coded: v1 no)      */
static uint8_t  s_phy_rx = 1;
static bool     s_whiten_off;           /* NimBLE always whitens -> false     */
static bool     s_encrypt;              /* CCM enabled -> LLE[0x50] bit5      */
static bool     s_dtm;                  /* demod cfg14 variant - RE §4.4      */
static bool     s_auto_arm_rx;          /* TX->RX T_IFS continuation flag     */
static bool     s_rx_armed;             /* engine waiting for a frame         */

static uint8_t *s_rxbuf;                /* RX frame buffer (header first)     */
static uint8_t  s_last_rx_len;          /* len byte of last RX (RE §5.3)      */
static uint32_t s_last_anchor;          /* cputime @ clock-capture event      */

static uint8_t  s_rspbuf[2 + 255];      /* auto-ack response frame            */

/* ------------------------------------------------------------------------- */
/* Small helpers                                                             */
/* ------------------------------------------------------------------------- */

static uint32_t
load32_le(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void
store32_le(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static void
wch_fence(void)
{
    __asm volatile ("fence.i" ::: "memory");
}

/* Poll (*reg & mask) until it equals expect, bounded by cputime. */
static bool
poll_reg(volatile uint32_t *reg, uint32_t mask, uint32_t expect,
         uint32_t timeout_us)
{
    uint32_t start = os_cputime_get32();

    while ((*reg & mask) != expect) {
        if ((uint32_t)(os_cputime_get32() - start) > timeout_us) {
            return false;
        }
    }
    return true;
}

/* ------------------------------------------------------------------------- */
/* Guard word (RE §3/§4.3/§6.3: save [0x4002420C] -> 0 + fence.i, restore)   */
/* ------------------------------------------------------------------------- */

void
wch_rf_guard_enter(void)
{
    s_guard_save = WCH_RF_GUARD_REG;
    WCH_RF_GUARD_REG = 0;
    wch_fence();
}

void
wch_rf_guard_exit(void)
{
    wch_fence();
    WCH_RF_GUARD_REG = s_guard_save;
    wch_fence();
}

/* ------------------------------------------------------------------------- */
/* Channel / synthesizer programming (RE §4.2)                               */
/* ------------------------------------------------------------------------- */

/* BLE channel -> carrier kHz.  Standard allocation, no 40-entry table. */
static uint32_t
freq_for_channel(uint8_t ch)
{
    if (ch <= 36) {
        return (2404UL + 2UL * ch) * 1000UL;        /* data channels      */
    }
    switch (ch) {
    case 37: return 2402000UL;                      /* advertising 37     */
    case 38: return 2426000UL;                      /* advertising 38     */
    case 39: return 2480000UL;                      /* advertising 39     */
    default: return 2402000UL;
    }
}

/* RFEND[0x44] synthesizer programming - RE §4.2 core formula. */
static void
synth_program(uint32_t f_khz)
{
    uint32_t v;

    v = WCH_RFEND_SYNTH;
    v = (v & WCH_RFEND_SYNTH_INT_MASK) |
        (((f_khz / 64000UL) & 0x1FUL) << WCH_RFEND_SYNTH_INT_SHIFT);
    v = (v & WCH_RFEND_SYNTH_FRAC_MASK) |
        ((((f_khz % 64000UL) << 10) / 250UL) & WCH_RFEND_SYNTH_FRAC_MAX);
    WCH_RFEND_SYNTH = v;
}

static uint16_t
lo_inject_for_phy(uint8_t phy)
{
    return (phy == 2) ? g_wch_rf_timing.lo_inject_khz_2m
                      : g_wch_rf_timing.lo_inject_khz_1m;
}

/*
 * RF_DevSetChannel(ch, mode=1) - RE §4.2 frequency mode path:
 *   BB[0x00][5:0] <- channel, RFEND[0x2C] |= 2, synth <- carrier - LO,
 *   bit6 <- whitening-off flag.
 */
static void
dev_set_channel(uint8_t ch, uint16_t lo_inject_khz)
{
    uint32_t f = freq_for_channel(ch);
    uint32_t fs = (f > lo_inject_khz) ? (f - lo_inject_khz) : f;

    WCH_BB_RFCTRL = (WCH_BB_RFCTRL & ~WCH_BB_RFCTRL_CH_MASK) |
                    (ch & WCH_BB_RFCTRL_CH_MASK);
    WCH_RFEND_FREQMODE |= WCH_RFEND_FREQMODE_EN;
    synth_program(fs);
    if (s_whiten_off) {
        WCH_BB_RFCTRL |= WCH_BB_RFCTRL_WHITEN_OFF;
    } else {
        WCH_BB_RFCTRL &= ~WCH_BB_RFCTRL_WHITEN_OFF;
    }
    s_channel = ch;
    s_synth_khz = fs;
}

void
wch_rf_set_channel(uint8_t ble_chan)
{
    if (ble_chan > 39) {
        return;
    }
    wch_rf_guard_enter();
    dev_set_channel(ble_chan, lo_inject_for_phy(s_phy_rx));
    wch_rf_guard_exit();
}

uint8_t
wch_rf_channel_get(void)
{
    return s_channel;
}

void
wch_rf_whiten_set(bool disable)
{
    s_whiten_off = disable;
}

void
wch_rf_phy_mode_set(uint8_t tx_mode, uint8_t rx_mode)
{
    /* Coded PHY not supported in v1 - clamp to 1M (see report limitations). */
    if (tx_mode == 2) {
        s_phy_tx = 2;
    } else {
        s_phy_tx = 1;
    }
    if (rx_mode == 2) {
        s_phy_rx = 2;
    } else {
        s_phy_rx = 1;
    }
}

uint8_t
wch_rf_phy_mode_tx_get(void)
{
    return s_phy_tx;
}

uint8_t
wch_rf_phy_mode_rx_get(void)
{
    return s_phy_rx;
}

void
wch_rf_encrypt_set(bool en)
{
    s_encrypt = en;
}

bool
wch_rf_encrypt_get(void)
{
    return s_encrypt;
}

void
wch_rf_dtm_set(bool en)
{
    s_dtm = en;
}

void
wch_rf_last_rx_len_set(uint8_t len)
{
    s_last_rx_len = len;
}

void
wch_rf_aa_crc_set(uint32_t access_addr, uint32_t crcinit)
{
    s_aa = access_addr;
    s_crcinit = crcinit & 0xFFFFFFUL;
}


uint8_t *
wch_rf_rx_frame(void)
{
    return s_rxbuf;
}

uint32_t
wch_rf_last_anchor(void)
{
    return s_last_anchor;
}

void
wch_rf_auto_arm_rx(bool en)
{
    s_auto_arm_rx = en;
}

bool
wch_rf_auto_arm_rx_get(void)
{
    return s_auto_arm_rx;
}

void
wch_rf_set_callback(wch_rf_cb_t cb)
{
    s_cb = cb;
}

static void
notify(enum wch_rf_event evt, uint32_t payload)
{
    if (s_cb) {
        s_cb(evt, payload);
    }
}

/* ------------------------------------------------------------------------- */
/* PHY mode register groups (RE §4.4 BLE_SetPHYTxMode / BLE_SetPHYRxMode)    */
/* ------------------------------------------------------------------------- */

static void
set_phy_tx_mode(uint8_t phy)
{
    /* PA external control (gPaControl) unused - no external PA on this board. */
    if (phy == 2) {
        WCH_BB_RFCTRL = (WCH_BB_RFCTRL & ~WCH_BB_RFCTRL_PHY_MASK) |
                        WCH_BB_RFCTRL_PHY_2M;
    } else {
        WCH_BB_RFCTRL &= ~WCH_BB_RFCTRL_PHY_MASK;       /* 1M - RE §4.4 */
    }
    /* LLE[0x0C] &= ~0x2000 (RE §4.4) omitted: LLE[0x0C] is the guard word,
     * held at 0 inside a guard section (see wch_rf_guard_enter).           */
}

static void
set_phy_rx_mode(uint8_t phy)
{
    if (phy == 2) {
        WCH_BB_RXCFG20 = WCH_BB_RX_2M_CFG20;            /* RE §4.4 */
        WCH_BB_RXCFG24 = WCH_BB_RX_2M_CFG24;            /* RE §4.4 */
        WCH_BB_RXCFG1C = WCH_BB_RX_2M_CFG1C;            /* RE §4.4 */
        WCH_BB_RFCTRL = (WCH_BB_RFCTRL & ~WCH_BB_RFCTRL_PHY_MASK) |
                        WCH_BB_RFCTRL_PHY_2M;
    } else {
        WCH_BB_RXCFG20 = WCH_BB_RX_1M_CFG20;            /* RE §4.4 */
        WCH_BB_RXCFG24 = WCH_BB_RX_1M_CFG24;            /* RE §4.4 */
        WCH_BB_RXCFG1C = WCH_BB_RX_1M_CFG1C;            /* RE §4.4 */
        WCH_BB_RFCTRL &= ~WCH_BB_RFCTRL_PHY_MASK;       /* 1M       */
    }
    /* Common demodulator / ADC timing group - RE §4.4 */
    WCH_BB_RXCFG28 = WCH_BB_RX_CFG28;
    WCH_BB_RXCFG10 = WCH_BB_RX_1M_CFG10;
    WCH_BB_RXCFG14 = s_dtm ? WCH_BB_RX_CFG14_DTM : WCH_BB_RX_CFG14_NORMAL;
}

/* ------------------------------------------------------------------------- */
/* Engine stop / shutdown (RE §4.3, §12.1)                                   */
/* ------------------------------------------------------------------------- */

/* phy_status_clear() equivalent: halt the LLE engine (RE §4.3/§12.1). */
static void
engine_stop(void)
{
    WCH_LLE_MODE &= ~WCH_LLE_MODE_MASK;             /* LLE[0x50] &= ~7  §12.1 */
    WCH_LLE_TIMEOUT &= ~WCH_LLE_TIMEOUT_MASK;       /* LLE[0x14] &= ~7  §4.3  */
    WCH_LLE_CMD |= WCH_LLE_CMD_FORCE_STOP;          /* LLE[0x00] |= 8   §4.3  */
    WCH_LLE_WINDOW = 0;
}

void
wch_rf_shut(void)
{
    wch_rf_guard_enter();

    /* ble_ll_hw_api_shut() - RE §4.3 */
    engine_stop();
    WCH_BB_RFCTRL = (WCH_BB_RFCTRL & ~WCH_BB_RFCTRL_DIR_MASK) |
                    WCH_BB_RFCTRL_SHUT;             /* (BB&~0x180)|0x80 */
    WCH_RFEND_FECTRL &= WCH_RFEND_FECTRL_SHUT;      /* &= 0xFFCDFFFF    */
    WCH_LLE_TIMEOUT = 0;                            /* LLE[0x14] = 0    */

    s_rx_armed = false;
    s_auto_arm_rx = false;

    wch_rf_guard_exit();
}

/* ------------------------------------------------------------------------- */
/* Analog front end calibration (RE §6)                                      */
/* ------------------------------------------------------------------------- */

/*
 * RFEND_TXCtune - RE §6.1: measure VCO capacitor (CO) / gain (GA) at the
 * three band codes 0xBF/0xD3/0xE7 (2401/2440/2480 MHz), then interpolate a
 * 39-channel x 4-bit table into RFEND[0xA0..].
 */
static void
rfend_txctune(void)
{
    static const uint8_t band_code[3] = { 0xBF, 0xD3, 0xE7 };   /* RE §6.1 */
    uint8_t co[3];
    uint8_t ga[3];
    uint32_t table[WCH_RFEND_VCOTAB_WORDS];
    int i;
    int ch;

    for (i = 0; i < 3; i++) {
        WCH_RFEND_BANDCODE = (WCH_RFEND_BANDCODE & WCH_RFEND_BANDCODE_MASK) |
                             ((uint32_t)band_code[i] << WCH_RFEND_BANDCODE_SHIFT);
        WCH_RFEND_TUNE |= WCH_RFEND_TUNE_TRIG;          /* RE §6.1 */
        (void)poll_reg(&WCH_RFEND_TUNE, WCH_RFEND_TUNE_TRIG, 0,
                       g_wch_rf_timing.tune_timeout_us);
        co[i] = (uint8_t)(WCH_RFEND_TUNE_CO & WCH_RFEND_TUNE_CO_MASK);
        ga[i] = (uint8_t)((WCH_RFEND_TUNE_GA >> WCH_RFEND_TUNE_GA_SHIFT) &
                          WCH_RFEND_TUNE_GA_MASK);
    }

    /*
     * 39-channel interpolation.  RE §6.1 sketches an index-proportional
     * formula; channels are not evenly spaced in frequency (adv 37/38/39),
     * so we interpolate on carrier frequency instead - same endpoints,
     * monotonic in between.  4 bits per channel.
     */
    for (ch = 0; ch < 39; ch++) {
        uint32_t f = freq_for_channel((uint8_t)ch) / 1000UL;    /* MHz */
        uint32_t vco;
        uint32_t gain;
        uint32_t num;
        uint32_t den;

        if (f <= 2440) {
            num = f - 2401;
            den = 39;
            vco = (uint32_t)co[0] + (((uint32_t)(co[1] - co[0]) * num) + den / 2) / den;
            gain = (uint32_t)ga[0] + (((uint32_t)(ga[1] - ga[0]) * num) + den / 2) / den;
        } else {
            num = f - 2440;
            den = 40;
            vco = (uint32_t)co[1] + (((uint32_t)(co[2] - co[1]) * num) + den / 2) / den;
            gain = (uint32_t)ga[1] + (((uint32_t)(ga[2] - ga[1]) * num) + den / 2) / den;
        }

        vco &= 0x0FUL;
        gain &= 0x0FUL;
        table[ch / 8] |= vco << (4 * (ch % 8));

        /*
         * The GA table write-back register was not identified during RE
         * (the lib writes it to "another register group", RE §6.1).  The
         * interpolated GA values are computed for diagnostics only until a
         * lib-calibrated RFEND dump locates the real register.
         */
        (void)gain;
    }
    for (i = 0; i < WCH_RFEND_VCOTAB_WORDS; i++) {
        WCH_RFEND_VCOTAB(i) = table[i];
    }
}

/* RFEND_TXFtune - RE §4.9: RFEND[0x04] |= 0x100 */
static void
rfend_txftune(void)
{
    WCH_RFEND_TUNE |= WCH_RFEND_TUNE_TXFTUNE;
}

/* RFEND_RXFilter - RE §6.2 (uses the LLE timer as measurement window) */
static void
rfend_rxfilter(void)
{
    WCH_RFEND_CFG50 &= ~WCH_RFEND_CFG50_FILT_EN;    /* &= ~0x10000   */
    WCH_RFEND_FECTRL |= WCH_RFEND_FECTRL_RXFILT;    /* |= 0x200000   */
    WCH_RFEND_CFG0C ^= WCH_RFEND_CFG0C_FILT_TOGGLE; /* bit4 toggle 1 */
    WCH_RFEND_CFG0C ^= WCH_RFEND_CFG0C_FILT_TOGGLE; /* bit4 toggle 2 */
    WCH_LLE_WINDOW = 0x50;                          /* 25 us window  */
    (void)poll_reg(&WCH_RFEND_FILTMEAS, WCH_RFEND_FILTMEAS_RDY,
                   WCH_RFEND_FILTMEAS_RDY, g_wch_rf_timing.tune_timeout_us);
    {
        uint32_t v = WCH_RFEND_CFG50 & WCH_RFEND_CFG50_FILT_MASK;
        v |= WCH_RFEND_FILTMEAS & WCH_RFEND_FILTMEAS_MASK;
        WCH_RFEND_CFG50 = v;
    }
    WCH_RFEND_FECTRL &= ~WCH_RFEND_FECTRL_RXFILT;
    WCH_LLE_WINDOW = 0;
}

/* RFEND_RXAdc - RE §6.2 */
static void
rfend_rxadc(void)
{
    WCH_RFEND_CFG58 &= ~WCH_RFEND_CFG58_RXADC;      /* +0x58 bit16 clear */
    WCH_RFEND_FECTRL |= WCH_RFEND_FECTRL_RXADC;     /* RFEND[8] |= 0x10000 */
    WCH_RFEND_CFG0C &= ~WCH_RFEND_CFG0C_RXADC;      /* +0x0C bit8 clear  */
    WCH_RFEND_CFG0C |= WCH_RFEND_CFG0C_RXADC;       /* +0x0C bit8 set    */
    WCH_RFEND_TUNE &= ~WCH_RFEND_TUNE_RXADC;        /* RFEND[4] bit16 clr */
    WCH_RFEND_TUNE |= WCH_RFEND_TUNE_RXADC;         /* RFEND[4] bit16 set */
}

/*
 * RFEND_SetSensitivity - RE §6.2.  The documented portion clears RFEND[0x5C]
 * bits 28/27/19 and sets RFEND[0x50] bits[15:12] = 9; the remaining
 * RFEND[0x48]/[0x4C] "magic combos" are the DevInit baselines applied in
 * rfend_dev_init() below (RE §3.3).
 */
static void
rfend_set_sensitivity(void)
{
    WCH_RFEND_CFG5C &= ~WCH_RFEND_CFG5C_SENS_CLR;
    {
        uint32_t v = WCH_RFEND_CFG50 & WCH_RFEND_CFG50_SENS_MASK;
        v |= WCH_RFEND_CFG50_SENS_9;
        WCH_RFEND_CFG50 = v;
    }
}

/* ------------------------------------------------------------------------- */
/* Device init (RE §3)                                                       */
/* ------------------------------------------------------------------------- */

/* LLE_DevInit - RE §3.2 */
static void
lle_dev_init(void)
{
    WCH_LLE_TIMEOUT = 0x8C;         /* RE §3.2 */
    WCH_LLE_TURNAROUND = 0x6C;      /* RE §3.2 */
    WCH_LLE_INIT24 = 0x8C;          /* RE §3.2 */
    WCH_LLE_INIT2C = 0x3C;          /* RE §3.2 */
    WCH_LLE_INIT34 = 0x8C;          /* RE §3.2 */
    WCH_LLE_INIT3C = 0x3C;          /* RE §3.2 */
    WCH_LLE_INIT44 = 0x8C;          /* RE §3.2 */
    WCH_LLE_INIT4C = 0x6C;          /* RE §3.2 */

    /* LLE[0x74] = &(shared RX frame pointer variable) - RE §3.2.  We mirror
     * the lib layout: the register holds the address of our rxbuf variable. */
    WCH_LLE_RXBUFPTR_VAR = (uint32_t)&s_rxbuf;

    WCH_LLE_IRQSTAT = 0xFFFF;       /* RE §3.2 */
    WCH_REG32(WCH_LLE_BASE, 0x0C) = 0xF00F;     /* state mask - RE §3.2 */
    s_lle_mask = 0xF00F;
}

/* RFEND_DevInit - RE §3.3 (RFEND_Reset() not captured by RE; the documented
 * post-reset programming below is applied to POR defaults instead). */
static void
rfend_dev_init(void)
{
    uint32_t v;

    WCH_RFEND_CFG28 = WCH_RFEND_CFG28_INIT;         /* RFEND[40] = 1152 */

    v = WCH_RFEND_CFG48;                            /* RE §3.3 sequence */
    v &= 0x8FFFFFFFUL;
    v |= 0x20000000UL;
    v &= 0xF8FFFFFFUL;
    v |= 0x40000000UL;
    v &= ~0x0000000FUL;
    v |= 0x00000009UL;
    v &= 0xFFF8FFFFUL;
    WCH_RFEND_CFG48 = v;

    v = WCH_RFEND_CFG4C;                            /* RE §3.3 sequence */
    v |= 0x00000003UL;
    v &= ~0x00000070UL;
    v |= 0x00000030UL;
    v |= 0x00000700UL;
    v |= 0x01000000UL;
    v &= ~0x8F000000UL;
    WCH_RFEND_CFG4C = v;
}

/* BB_DevInit - RE §3.1 */
static void
bb_dev_init(void)
{
    uint32_t cfg = 7;       /* sleep-clock accuracy class, low 6 bits (RE §3.1;
                             * lib takes cfg = ble+0x14; 7 ~ 50 ppm class)     */
    uint32_t v;

    WCH_BB_RFCTRL |= 0x80000000UL;                  /* RE §3.1 */
    WCH_BB_RFCTRL |= 0x00010000UL;                  /* RE §3.1 */
    WCH_BB_CFG34 = WCH_BB_CFG34_INIT;               /* 464     */

    v = (cfg << WCH_BB_AUTOCFG_CFG_SHIFT) | 0x80010EC8UL;
    v &= WCH_BB_AUTOCFG_CFG_MASK;
    v |= (cfg & 0x3FUL) << WCH_BB_AUTOCFG_CFG_SHIFT;
    WCH_BB_AUTOCFG = v;                             /* RE §3.1 */

    WCH_BB_RXCFG20 = 0x00090083UL;                  /* RE §3.1 (+0x20) */
}

/* ------------------------------------------------------------------------- */
/* Init / recalibrate (RE §3 + §6)                                           */
/* ------------------------------------------------------------------------- */

int
wch_rf_init(void)
{
    /* 32M crystal load capacitance and current - EXAM/BLE/HAL/MCU.c
     * WCHBLE_Init() (3 lines, verbatim). */
    OSC->HSE_CAL_CTRL &= ~(0x07UL << 28);
    OSC->HSE_CAL_CTRL |= 0x03UL << 28;
    OSC->HSE_CAL_CTRL |= 3UL << 24;

    /* RE §3 init order: LLE_DevInit -> RFEND_DevInit -> BB_DevInit */
    lle_dev_init();
    rfend_dev_init();
    bb_dev_init();

    /* First-shot analog calibration, same primitive order as the periodic
     * calibration of RE §6.3. */
    rfend_txctune();
    rfend_txftune();
    rfend_rxfilter();
    rfend_rxadc();
    rfend_set_sensitivity();

    s_channel = 0xFF;
    s_rx_armed = false;
    s_auto_arm_rx = false;
    s_rxbuf = NULL;
    s_last_anchor = 0;

    /* NVIC: BB and LLE at priority 1 (numerically lower = higher than the
     * platform TIM3 at 2). */
    NVIC_SetPriority(BB_IRQn, 1);
    NVIC_SetPriority(LLE_IRQn, 1);
    NVIC_EnableIRQ(BB_IRQn);
    NVIC_EnableIRQ(LLE_IRQn);

    return 0;
}

/*
 * Periodic calibration - RE §6.3 BLE_RegInit state machine, minus the TMOS
 * task plumbing.  Refuses to run while the engine is busy.
 */
void
wch_rf_recalibrate(void)
{
    if (WCH_LLE_CMD & WCH_LLE_CMD_BUSY_MASK) {
        return;     /* engine busy: skip this round (RE §6.3 reports via
                     * rfStatusCB(17,...); we just skip) */
    }

    wch_rf_guard_enter();

    /* RE §6.3: BB[0] &= ~0x3001 -> |= 0x10000 -> &= ~0x181 -> |= 0x100 */
    WCH_BB_RFCTRL &= ~0x00003001UL;
    WCH_BB_RFCTRL |= 0x00010000UL;
    WCH_BB_RFCTRL &= ~0x00000181UL;
    WCH_BB_RFCTRL |= 0x00000100UL;

    WCH_RFEND_FECTRL |= WCH_RFEND_FECTRL_RF_ON;     /* RFEND[8] |= 0x330000 */
    WCH_LLE_MODE = WCH_LLE_TIMEOUT_CAL;             /* LLE[0x50] = 93 (0x5D) */

    rfend_txctune();
    rfend_txftune();
    rfend_rxfilter();
    rfend_rxadc();
    rfend_set_sensitivity();

    /* RE §6.3: BB[0] = (BB[0] & ~0x181) | 0x80 */
    WCH_BB_RFCTRL = (WCH_BB_RFCTRL & ~0x00000181UL) | 0x00000080UL;
    WCH_RFEND_FECTRL &= ~0x00320000UL;              /* RFEND[8] &= ~0x320000 */
    WCH_LLE_MODE = 0;                               /* LLE[0x50] = 0 */

    wch_rf_guard_exit();
}

/* ------------------------------------------------------------------------- */
/* Diagnostics (RE §4.9)                                                     */
/* ------------------------------------------------------------------------- */

int8_t
wch_rf_rssi(void)
{
    return (int8_t)(WCH_BB_RSSICFO >> WCH_BB_RSSI_SHIFT);
}

int16_t
wch_rf_cfo(void)
{
    uint32_t v = (WCH_BB_RSSICFO & 0xFFFFUL) >> WCH_BB_CFO_SHIFT;

    if (v & 0x0400UL) {                             /* 11-bit sign extend */
        v |= 0xF800UL;
    }
    return (int16_t)v;
}

/* ------------------------------------------------------------------------- */
/* TX / RX primitives                                                        */
/* ------------------------------------------------------------------------- */

/*
 * Basic transmit - RE §4.6 RF_Tx (hop bookkeeping and TMOS process
 * registration omitted; auto-ack arm bit not set for basic TX).
 */
int
wch_rf_tx_basic(const uint8_t *frame, uint16_t n, uint32_t timeout_half_us)
{
    uint32_t deadline;
    int rc = 0;

    if ((frame == NULL) || (n < 2)) {
        return -1;
    }
    if (WCH_LLE_CMD & WCH_LLE_CMD_BUSY_MASK) {
        return -1;                                  /* rfInfo busy - RE §4.6 */
    }

    wch_rf_guard_enter();

    engine_stop();                                  /* phy_status_clear equiv */

    WCH_LLE_WINDOW = 0;                             /* clear busy/sched      */
    WCH_LLE_IRQSTAT = WCH_LLE_IRQ_CLEAR_TX;         /* LLE[0x08]=2, RE §4.4  */

    dev_set_channel(s_channel, lo_inject_for_phy(s_phy_tx));    /* RE §4.6 */

    /* BB[0x00] = (BB[0x00] & ~0x180) | 0x100 - RE §4.6 */
    WCH_BB_RFCTRL = (WCH_BB_RFCTRL & ~WCH_BB_RFCTRL_DIR_MASK) |
                    WCH_BB_RFCTRL_DIR_RX;

    WCH_RFEND_FECTRL |= WCH_RFEND_FECTRL_RF_ON;     /* RFEND[0x08] |= 0x330000 */
    WCH_LLE_TIMEOUT = WCH_LLE_TIMEOUT_TX;           /* LLE[0x14] = 0x5A        */

    WCH_BB_ACCESSADDR = s_aa;                       /* BB[0x08] = accessAddress */
    WCH_BB_CRCINIT = s_crcinit;                     /* BB[0x04] = CRCInit       */
    WCH_LLE_CRC_AA_CFG &= ~0x1UL;                   /* basic mode bit0 = 0      */

    /* TX buffer: [hdr][len][payload] == lib's [pktTxType][txLen][data] */
    WCH_LLE_FRAMEPTR = (uint32_t)frame;             /* LLE[0x70] - RE §4.6      */

    set_phy_tx_mode(s_phy_tx);                      /* BLE_SetPHYTxMode - §4.4  */

    /* Launch tail - RE §4.6: BB[0x00] |= 0x800000; BB[0x2C] &= ~3; LLE[0]=2 */
    WCH_BB_RFCTRL |= WCH_BB_RFCTRL_TX_TRIG;
    WCH_BB_AUTOCFG &= ~WCH_BB_AUTOCFG_ARM_MASK;
    WCH_LLE_WINDOW = timeout_half_us;               /* TX window (0.5 us units) */
    WCH_LLE_CMD = WCH_LLE_CMD_START_TX;

    /* Block until busy flag clears - RE §4.3 ll_tx_wait_finish. */
    deadline = os_cputime_get32() + (timeout_half_us / 2U) + 2U;
    while (WCH_LLE_WINDOW != 0) {
        if ((int32_t)(os_cputime_get32() - deadline) >= 0) {
            rc = -1;
            break;
        }
    }

    wch_rf_guard_exit();
    return rc;
}

/*
 * Basic receive - RE §4.7 RF_Rx (basic variant: no auto response prefill,
 * no hop retry task).  Non-blocking: arms and returns.
 */
int
wch_rf_rx_basic(uint8_t *rxbuf, uint16_t maxlen, uint32_t window_half_us)
{
    (void)maxlen;   /* RxMaxlen limit enforced by the PHY layer buffer size */

    if (rxbuf == NULL) {
        return -1;
    }

    wch_rf_guard_enter();

    if (WCH_LLE_CMD & WCH_LLE_CMD_BUSY_MASK) {
        /* RE §4.7: engine running -> halt first */
        WCH_LLE_TIMEOUT &= ~WCH_LLE_TIMEOUT_MASK;
        WCH_LLE_CMD |= WCH_LLE_CMD_FORCE_STOP;
    }

    WCH_LLE_WINDOW = 0;                             /* RE §4.7 */
    WCH_LLE_IRQSTAT = WCH_LLE_IRQ_CLEAR_RX;         /* LLE[0x08]=0x2000, §4.7 */

    dev_set_channel(s_channel, lo_inject_for_phy(s_phy_rx));    /* RE §4.7 */

    set_phy_rx_mode(s_phy_rx);                      /* BLE_SetPHYRxMode - §4.4 */

    WCH_BB_ACCESSADDR = s_aa;                       /* BB[0x08] - RE §4.7 */
    WCH_BB_CRCINIT = s_crcinit;                     /* BB[0x04] - RE §4.7 */
    WCH_LLE_CRC_AA_CFG &= ~0x1UL;                   /* LLE[0x04] |= cfg&1 */

    WCH_LLE_FRAMEPTR = (uint32_t)rxbuf;             /* LLE[0x70] = RX buffer */

    /* BB[0x00] = (BB[0x00] & ~0x180) | 0x100; RFEND[0x08] |= 0x330000 */
    WCH_BB_RFCTRL = (WCH_BB_RFCTRL & ~WCH_BB_RFCTRL_DIR_MASK) |
                    WCH_BB_RFCTRL_DIR_RX;
    WCH_RFEND_FECTRL |= WCH_RFEND_FECTRL_RF_ON;

    WCH_LLE_TIMEOUT = WCH_LLE_TIMEOUT_RX;           /* LLE[0x14] = 0x59, §4.7 */

    /* RX window width in 0.5 us units (LLE[0x64]) - RE §12.1 step 7.
     * Clock-capture driven window reload (RE §5.2 BB bit6) not replicated:
     * the window is pre-loaded here.  TODO(board): verify against HW. */
    WCH_LLE_WINDOW = window_half_us;

    s_rxbuf = rxbuf;
    s_rx_armed = true;

    WCH_LLE_CMD = WCH_LLE_CMD_START_RX;             /* LLE[0x00] = 1 - RE §4.7 */

    wch_rf_guard_exit();
    return 0;
}

/*
 * Auto-ack arm - RE §4.7 auto mode: pre-fill the response frame and set the
 * BB[0x2C] RX-arm bit.  v1: interface retained, not exercised by NimBLE.
 */
int
wch_rf_arm_auto_ack(const uint8_t *rsp, uint16_t n)
{
    if ((rsp == NULL) || (n < 2) || (n > sizeof(s_rspbuf))) {
        return -1;
    }
    memcpy(s_rspbuf, rsp, n);

    wch_rf_guard_enter();
    WCH_BB_AUTOCFG = (WCH_BB_AUTOCFG & ~WCH_BB_AUTOCFG_ARM_MASK) |
                     WCH_BB_AUTOCFG_ARM_RX;         /* BB[0x2C] |= 1 */
    wch_rf_guard_exit();
    return 0;
}

/* ------------------------------------------------------------------------- */
/* AES hardware (RE §9.7)                                                    */
/* ------------------------------------------------------------------------- */

int
wch_aes_ecb(const uint8_t key[16], const uint8_t in[16], uint8_t out[16])
{
    int i;

    if ((key == NULL) || (in == NULL) || (out == NULL)) {
        return -1;
    }

    WCH_AES_CTRL = WCH_AES_CTRL_RESET;              /* AES[0x00] = 0x100 */
    WCH_AES_CTRL = 0;                               /* AES[0x00] = 0     */

    for (i = 0; i < 4; i++) {
        WCH_AES_KEY(i) = load32_le(key + i * 4);    /* AES[0x28..] key   */
    }
    for (i = 0; i < 4; i++) {
        WCH_AES_BLOCK(i) = load32_le(in + i * 4);   /* AES[0x18..] data  */
    }

    WCH_AES_STATUS &= ~0x2UL;                       /* AES[0x04] &= ~2   */
    WCH_AES_STATUS |= 0x1UL;                        /* AES[0x04] |= 1    */

    WCH_AES_CTRL |= WCH_AES_CTRL_START;             /* AES[0x00] |= 1    */

    if (!poll_reg(&WCH_AES_STATUS, WCH_AES_STATUS_BUSY, 0,
                  g_wch_rf_timing.tune_timeout_us)) {
        return -1;                                  /* AES never finished */
    }

    for (i = 0; i < 4; i++) {
        store32_le(out + i * 4, WCH_AES_BLOCK(i));  /* ciphertext in place */
    }

    WCH_AES_STATUS &= ~WCH_AES_STATUS_DONE_MASK;    /* clear done flags §5.2 */
    return 0;
}

/* ------------------------------------------------------------------------- */
/* Interrupt handling (RE §5.2 / §5.3)                                       */
/* ------------------------------------------------------------------------- */

/*
 * LLE[0x1C] turnaround constant selector - RE §5.3.  Plain 1M/2M keyed on
 * the length byte of the frame just received; encrypted / coded variants.
 */
static uint32_t
turnaround_value(void)
{
    /* Coded PHY unsupported in v1 -> no 108 branch. */
    if (s_encrypt) {
        return (s_last_rx_len == 0) ? 93UL : 107UL;
    }
    switch (s_last_rx_len) {
    case 0:  return 93UL;
    case 1:  return 97UL;
    case 2:  return 101UL;
    case 3:  return 105UL;
    default: return 107UL;
    }
}

/*
 * TX->RX T_IFS continuation - RE §4.8 rf_tx_auto_txProcess, relocated from
 * the lib's TMOS task into the TX-done ISR (no TMOS in this port):
 *   1. reprogram synthesizer for the ACK window (RE §4.8)
 *   2. apply the RX demodulator group (BLE_SetPHYRxMode, RE §4.4)
 *   3. load the ACK RX window into LLE[0x64] (0x196 @1M, RE §4.8/§12.1)
 *   4. point LLE[0x70] at the RX buffer
 *   5. set BB[0x2C] RX-arm (RE §5.3 TX-done handling)
 */
static void
arm_ack_rx(void)
{
    dev_set_channel(s_channel, lo_inject_for_phy(s_phy_rx));
    set_phy_rx_mode(s_phy_rx);
    WCH_LLE_WINDOW = (s_phy_rx == 2) ? g_wch_rf_timing.ack_window_half_us_2m
                                     : g_wch_rf_timing.ack_window_half_us_1m;
    WCH_LLE_FRAMEPTR = (uint32_t)s_rxbuf;
    WCH_BB_AUTOCFG = (WCH_BB_AUTOCFG & ~WCH_BB_AUTOCFG_ARM_MASK) |
                     WCH_BB_AUTOCFG_ARM_RX;
    s_rx_armed = true;
}

void
wch_rf_lle_isr(void)
{
    uint32_t st = WCH_LLE_IRQSTAT;                  /* RE §5.3: status source */

    /* bit2 = TX done (RE §5.3): turnaround constant FIRST, then optional
     * auto RX-arm, then write 4 to clear. */
    if (st & WCH_LLE_IRQ_TX_DONE) {
        WCH_LLE_TURNAROUND = turnaround_value();
        if (s_auto_arm_rx) {
            arm_ack_rx();
        }
        WCH_LLE_IRQSTAT = WCH_LLE_IRQ_TX_DONE;      /* write 4 to clear */
        notify(WCH_RF_EVT_TX_DONE, os_cputime_get32());
    }

    /* bit1 = RX done (RE §5.3): write 2 to clear. */
    if (st & WCH_LLE_IRQ_RX_DONE) {
        if (s_rxbuf != NULL) {
            s_last_rx_len = s_rxbuf[1];             /* len byte, RE §5.3 */
        }
        s_rx_armed = false;
        s_auto_arm_rx = false;
        WCH_LLE_IRQSTAT = WCH_LLE_IRQ_RX_DONE;      /* write 2 to clear */
        notify(WCH_RF_EVT_RX_DONE, os_cputime_get32());
    }

    /* bit0 / bit3 = aux state events (RE §5.3). Clear; counted via callback
     * payload only if a consumer cares. */
    if (st & 0x1UL) {
        WCH_LLE_IRQSTAT = 0x1UL;
    }
    if (st & 0x8UL) {
        WCH_LLE_IRQSTAT = 0x8UL;
    }

    /* bit14 = LLE tick (RE §5.3): reload tick, write 0x40000 to clear.
     * Only when the LLE[0x0C] mask has bit14 enabled. */
    if ((st & 0x4000UL) && (s_lle_mask & 0x4000UL)) {
        WCH_LLE_TICK = 0xFFFFFFFFUL;
        WCH_LLE_IRQSTAT = WCH_LLE_IRQ_TICK_CLR;
    }

    /* bit12 / bit13 / bit15: extended-advertising aux state bits (RE §5.3).
     * v1 NimBLE glue does not use ext-adv aux state machines; clear only. */
    if (st & 0x1000UL) {
        WCH_LLE_IRQSTAT = 0x1000UL;
    }
    if (st & 0x2000UL) {
        WCH_LLE_IRQSTAT = 0x2000UL;
    }
    if (st & 0x8000UL) {
        WCH_LLE_IRQSTAT = 0x8000UL;
    }
}

void
wch_rf_bb_isr(void)
{
    uint32_t st = WCH_BB_IRQSTAT;                   /* RE §5.2: status source */

    /* bit6 = clock capture / connection anchor (RE §5.2): record cputime,
     * write 0x60 to clear.  The lib's 32k-calibration sub-branch (gBleIPPara
     * bit5) and the deferred RX-window reload are not used by this port. */
    if (st & WCH_BB_IRQSTAT_CLKCAP) {
        s_last_anchor = os_cputime_get32();
        WCH_BB_IRQSTAT = WCH_BB_IRQSTAT_CLKCAP_CLR;
    }

    /* bit4 / bit7 = error / timeout (RE §5.2): write 0x10 / 0x80 to clear.
     * While an RX was armed with no frame received this is the RX window
     * timeout (wfr). */
    if (st & WCH_BB_IRQSTAT_ERR) {
        WCH_BB_IRQSTAT = WCH_BB_IRQSTAT_ERR_CLR;
        if (s_rx_armed) {
            s_rx_armed = false;
            notify(WCH_RF_EVT_RX_TIMEOUT, 0);
        } else {
            notify(WCH_RF_EVT_HW_ERR, st);
        }
    }
    if (st & WCH_BB_IRQSTAT_ERR2) {
        WCH_BB_IRQSTAT = WCH_BB_IRQSTAT_ERR2_CLR;
        if (s_rx_armed) {
            s_rx_armed = false;
            notify(WCH_RF_EVT_RX_TIMEOUT, 0);
        } else {
            notify(WCH_RF_EVT_HW_ERR, st);
        }
    }

    /* AES completion is delivered through the BB vector - RE §5.2:
     * AES[0x04] bit1 set -> clear with ~3. */
    if (WCH_AES_STATUS & 0x2UL) {
        WCH_AES_STATUS &= ~WCH_AES_STATUS_DONE_MASK;
    }
}
