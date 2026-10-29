/*
 * wch_rf.c - WCH CH32V208 BLE baseband / LLE / RF-AFE register level driver.
 *
 * Corrected sequences cite verified vendor ELF instructions. Inherited RE
 * notes remain pointers for unvalidated features, not register authority.
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
#include "core_riscv.h"     /* core register access helpers          */

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
static bool     s_rx_crc_ok;
static bool s_rx_progress;
volatile uint32_t g_wch_rf_tx_elapsed_us[16];
volatile uint8_t g_wch_rf_tx_payload_len[16];
static bool     s_rx_armed;             /* engine waiting for a frame         */

static uint8_t s_rx_dma[WCH_RF_RX_BUFFER_SIZE] __attribute__((aligned(4)));
static uint8_t *s_rxbuf = s_rx_dma; /* direct hardware RX destination */
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
        return ((ch <= 10 ? 2404UL : 2406UL) + 2UL * ch) * 1000UL;        /* data channels      */
    }
    switch (ch) {
    case 37: return 2402000UL;                      /* advertising 37     */
    case 38: return 2426000UL;                      /* advertising 38     */
    case 39: return 2480000UL;                      /* advertising 39     */
    default: return 2402000UL;
    }
}

static uint16_t
lo_inject_for_phy(uint8_t phy)
{
    return (phy == 2) ? g_wch_rf_timing.lo_inject_khz_2m
                      : g_wch_rf_timing.lo_inject_khz_1m;
}

/* Standard BLE channel mode, matching ll_advertise_tx 0x17cb8..0x17cce.
 * RF_DevSetChannel's explicit-frequency branch is for the proprietary RF API;
 * BLE lets hardware derive the LO from the channel and PHY selection. */
static void
dev_set_channel(uint8_t ch, uint16_t lo_inject_khz)
{
    WCH_RFEND_FREQMODE &= ~WCH_RFEND_FREQMODE_EN;
    WCH_BB_RFCTRL = (WCH_BB_RFCTRL & ~0x7fUL) |
                    (ch & WCH_BB_RFCTRL_CH_MASK) |
                    (s_whiten_off ? WCH_BB_RFCTRL_WHITEN_OFF : 0);
    s_channel = ch;
    s_synth_khz = freq_for_channel(ch) - lo_inject_khz;
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
set_phy_tx_mode(uint8_t phy, uint8_t payload_len)
{
    /* Vendor BLE_SetPHYTxMode 0x136dc..13712 (1M), 0x135fc..13638 (2M). */
    WCH_BB_RFCTRL = (WCH_BB_RFCTRL & ~WCH_BB_RFCTRL_PHY_MASK) |
                    (phy == 2 ? WCH_BB_RFCTRL_PHY_2M : WCH_BB_RFCTRL_PHY_1M);
    uint32_t saved = WCH_RF_GUARD_REG;
    WCH_RF_GUARD_REG = saved & ~0x2000UL;
    __asm volatile("nop; nop" ::: "memory");
    WCH_LLE_IRQSTAT = 0x2000;
    WCH_LLE_WINDOW = phy == 2 ? 8UL * (payload_len + 11UL) + 316UL
                              : 16UL * (payload_len + 10UL) + 316UL;
    WCH_RF_GUARD_REG = saved;
}

static void
set_phy_rx_mode(uint8_t phy)
{
    /* Vendor enum0=1M/enum1=2M share this group (0x137c2..1382c).
     * The (enum & 2) branch at0x13748 is coded PHY, unsupported here. */
    WCH_REG32(WCH_BB_BASE, 0x20) = 0x00090083UL;
    WCH_REG32(WCH_BB_BASE, 0x14) = 0x08101901UL;
    WCH_REG32(WCH_BB_BASE, 0x18) = 0x00031624UL;
    WCH_BB_RFCTRL = (WCH_BB_RFCTRL & ~WCH_BB_RFCTRL_PHY_MASK) |
                    (phy == 2 ? WCH_BB_RFCTRL_PHY_2M : WCH_BB_RFCTRL_PHY_1M);
    WCH_REG32(WCH_BB_BASE, 0x28) = 0x28be;
    WCH_REG32(WCH_BB_BASE, 0x24) = 0x01006310;
    WCH_REG32(WCH_BB_BASE, 0x10) = s_dtm ? 0x003fa2ceUL : 0x003222d0UL;
}

/* ------------------------------------------------------------------------- */
/* Engine stop / shutdown (RE §4.3, §12.1)                                   */
/* ------------------------------------------------------------------------- */

/* phy_status_clear() equivalent: halt the LLE engine (RE §4.3/§12.1). */
static void
engine_stop(void)
{
    WCH_LLE_MODE &= ~WCH_LLE_MODE_MASK;             /* LLE[0x50] &= ~7  §12.1 */
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
    WCH_LLE_MODE = 0;

    s_rx_armed = false;
    s_rx_progress = false;
    s_auto_arm_rx = false;

    wch_rf_guard_exit();
}

/* Analog sequences live separately and use verified byte offsets. */
extern int wch_rf_analog_init(void);
extern void wch_rf_analog_dev_init(void);

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

    /* Vendor stores the RX data base VALUE, never the address of a pointer.
     * A pointer-to-pointer here makes received RF bytes overwrite globals. */
    s_rxbuf = s_rx_dma;
    WCH_LLE_RXBUFPTR = (uint32_t)s_rxbuf;

    WCH_LLE_IRQSTAT = 0xFFFF;       /* RE §3.2 */
    WCH_REG32(WCH_LLE_BASE, 0x0C) = 0xF00F;     /* state mask - RE §3.2 */
    s_lle_mask = 0xF00F;
}

/* BB_DevInit - RE §3.1 */
static void
bb_dev_init(void)
{
    uint32_t cfg = 9;       /* vendor TX power enum: SetTxPower(0 dBm)=9;
                             * LL_SetTxPowerLevel writes BB+0x2c bits30:25. */
    uint32_t v;

    WCH_BB_RFCTRL |= 0x00800000UL;                  /* RE §3.1 */
    WCH_BB_RFCTRL |= 0x10000000UL;                  /* vendor 0x28ac */
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
    /* BLE IP (BB/LLE/RFEND/AES) is gated on the AHB and comes up unclocked
     * after reset.  The EVT closed lib opens it inside BLE_LibInit(); this
     * register-level port must do it before the first BB/LLE/RFEND access,
     * otherwise the calibration polls below can never see their flags set.
     * RCC_AHBPeriph_BLE_CRC = AHBPCENR bits 17/18 (BLE IP) + bit 6 (CRC). */
    RCC_AHBPeriphClockCmd(RCC_AHBPeriph_BLE_CRC, ENABLE);

    /* 32M crystal load capacitance and current - EXAM/BLE/HAL/MCU.c
     * WCHBLE_Init() (3 lines, verbatim). */
    OSC->HSE_CAL_CTRL &= ~(0x07UL << 28);
    OSC->HSE_CAL_CTRL |= 0x03UL << 28;
    OSC->HSE_CAL_CTRL |= 3UL << 24;

    NVIC_DisableIRQ(BB_IRQn);
    NVIC_DisableIRQ(LLE_IRQn);
    lle_dev_init();
    wch_rf_analog_dev_init();
    bb_dev_init();
    int rc = wch_rf_analog_init();
    if (rc != 0) {
        return rc;
    }

    s_channel = 0xFF;
    s_rx_armed = false;
    s_rx_progress = false;
    s_auto_arm_rx = false;
    s_rxbuf = s_rx_dma;
    s_last_anchor = 0;

    WCH_BB_IRQSTAT = 0xf0;
    WCH_LLE_IRQSTAT = 0xffff;
    NVIC_ClearPendingIRQ(BB_IRQn);
    NVIC_ClearPendingIRQ(LLE_IRQn);
    NVIC_SetPriority(LLE_IRQn, 0x20);
    NVIC_SetPriority(BB_IRQn, 0x40);
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
    if ((WCH_LLE_CMD & WCH_LLE_CMD_BUSY_MASK) == 0) {
        (void)wch_rf_analog_init();
    }
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
    WCH_LLE_MODE = WCH_LLE_TIMEOUT_TX;           /* LLE+0x50, vendor 0x29838 */

    WCH_BB_ACCESSADDR = s_aa;                       /* BB[0x08] = accessAddress */
    WCH_BB_CRCINIT = s_crcinit;                     /* BB[0x04] = CRCInit       */
    WCH_LLE_CRC_AA_CFG &= ~0x1UL;                   /* basic mode bit0 = 0      */

    /* TX buffer: [hdr][len][payload] == lib's [pktTxType][txLen][data] */
    WCH_LLE_FRAMEPTR = (uint32_t)frame;             /* LLE[0x70] - RE §4.6      */

    set_phy_tx_mode(s_phy_tx, frame[1]);                      /* BLE_SetPHYTxMode - §4.4  */

    /* Launch tail - RE §4.6: BB[0x00] |= 0x800000; BB[0x2C] &= ~3; LLE[0]=2 */
    WCH_BB_RFCTRL |= WCH_BB_RFCTRL_TX_TRIG;
    WCH_BB_AUTOCFG &= ~WCH_BB_AUTOCFG_ARM_MASK;
    uint32_t hw_window = WCH_LLE_WINDOW;
    (void)timeout_half_us; /* hardware timing comes from payload length */
    uint32_t started = os_cputime_get32();
    WCH_LLE_CMD = WCH_LLE_CMD_START_TX;

    /* Countdown expiry alone is not packet success. The basic TX stage
     * raises bit1, as consumed by rf_tx_basic_txProcess in the vendor lib. */
    deadline = started + hw_window / 2U + 200U;
    while ((WCH_LLE_IRQSTAT & WCH_LLE_IRQ_TX_DONE) == 0) {
        if ((int32_t)(os_cputime_get32() - deadline) >= 0) {
            rc = -1;
            break;
        }
    }

    g_wch_rf_tx_elapsed_us[frame[0] & 15U] = os_cputime_get32() - started;
    g_wch_rf_tx_payload_len[frame[0] & 15U] = frame[1];
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


    if (rxbuf == NULL || maxlen < WCH_RF_RX_BUFFER_SIZE) {
        return -1;
    }

    wch_rf_guard_enter();

    if (WCH_LLE_CMD & WCH_LLE_CMD_BUSY_MASK) {
        /* RE §4.7: engine running -> halt first */
        WCH_LLE_MODE &= ~WCH_LLE_MODE_MASK;
        WCH_LLE_CMD |= WCH_LLE_CMD_FORCE_STOP;
    }

    WCH_LLE_WINDOW = 0;                             /* RE §4.7 */
    WCH_LLE_IRQSTAT = WCH_LLE_IRQ_CLEAR_RX;         /* LLE[0x08]=0x2000, §4.7 */

    dev_set_channel(s_channel, lo_inject_for_phy(s_phy_rx));    /* RE §4.7 */

    set_phy_rx_mode(s_phy_rx);                      /* BLE_SetPHYRxMode - §4.4 */

    WCH_BB_ACCESSADDR = s_aa;                       /* BB[0x08] - RE §4.7 */
    WCH_BB_CRCINIT = s_crcinit;                     /* BB[0x04] - RE §4.7 */
    WCH_LLE_CRC_AA_CFG &= ~0x1UL;                   /* LLE[0x04] |= cfg&1 */

    WCH_LLE_RXBUFPTR = (uint32_t)rxbuf; /* independent RX DMA base */

    /* BB[0x00] = (BB[0x00] & ~0x180) | 0x100; RFEND[0x08] |= 0x330000 */
    WCH_BB_RFCTRL = (WCH_BB_RFCTRL & ~WCH_BB_RFCTRL_DIR_MASK) |
                    WCH_BB_RFCTRL_DIR_RX;
    WCH_RFEND_FECTRL |= WCH_RFEND_FECTRL_RF_ON;

    WCH_LLE_MODE = WCH_LLE_TIMEOUT_RX;           /* LLE+0x50, vendor basic RX */

    /* RX window width in 0.5 us units (LLE[0x64]) - RE §12.1 step 7.
     * Clock-capture driven window reload (RE §5.2 BB bit6) not replicated:
     * the window is pre-loaded here.  TODO(board): verify against HW. */
    WCH_LLE_WINDOW = window_half_us;

    s_rxbuf = rxbuf;
    s_rx_armed = true;
    s_rx_progress = false;
    WCH_BB_IRQSTAT = 0xf0;

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

bool
wch_rf_rx_crc_ok(void)
{
    return s_rx_crc_ok;
}

void
wch_rf_lle_isr(void)
{
    uint32_t st = WCH_LLE_IRQSTAT;
    bool was_rx = s_rx_armed;
    /* Basic mode only. Auto sequencer uses different stage/event meanings. */
    if (st & WCH_LLE_IRQ_RX_DONE) {
        s_rx_crc_ok = (WCH_BB_IRQSTAT & 0x20UL) == 0;
        WCH_LLE_IRQSTAT = WCH_LLE_IRQ_RX_DONE;
        if (was_rx && s_rxbuf != NULL) {
            s_last_rx_len = s_rxbuf[1];
            s_rx_armed = false;
            notify(WCH_RF_EVT_RX_DONE, os_cputime_get32());
        }
    }
    if (st & WCH_LLE_IRQ_TX_DONE) {
        WCH_LLE_IRQSTAT = WCH_LLE_IRQ_TX_DONE;
        notify(WCH_RF_EVT_TX_DONE, os_cputime_get32());
    }
    if (st & 0x2000UL) {
        WCH_LLE_IRQSTAT = 0x2000;
        if (was_rx && !(st & WCH_LLE_IRQ_RX_DONE)) {
            s_rx_armed = false;
            notify(WCH_RF_EVT_RX_TIMEOUT, 0);
        }
    }
    WCH_LLE_IRQSTAT = st & (0xffffUL & ~0x2003UL);
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
        if (s_rx_armed && !s_rx_progress) {
            s_rx_progress = true;
            /* Address match closes WFR, then allow the entire PDU. */
            /* BLE_SetPHYRxMode 0x13808..0x1382c: (max+5)*
             * (1M:16, 2M:8)+60 half-us after address capture. */
            WCH_LLE_WINDOW = (s_phy_rx == 2 ? 8U : 16U) * (255U + 5U) + 60U;
            notify(WCH_RF_EVT_RX_START, s_last_anchor);
        }
    }

    /* Vendor 0x3191a..0x31946 only acknowledges these timer flags. */
    WCH_BB_IRQSTAT = st & 0x90UL;

    /* AES completion is delivered through the BB vector - RE §5.2:
     * AES[0x04] bit1 set -> clear with ~3. */
    if (WCH_AES_STATUS & 0x2UL) {
        WCH_AES_STATUS &= ~WCH_AES_STATUS_DONE_MASK;
    }
}
