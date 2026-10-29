/*
 * ble_phy_wch.c - Apache NimBLE controller PHY adapter for WCH CH32V208.
 *
 * Implements the full controller/ble_phy.h contract on top of the
 * register-level CH32V208 BLE modem driver (wch_rf.h/c).  Structure follows
 * nimble/drivers/native/src/ble_phy.c; behavior follows the reverse
 * engineered libwchble programming model (WCH_BLE_LIB_RE_FINDINGS.md,
 * referenced as "RE §x.y").
 *
 * Frame format on the WCH LLE (RE §4.6/§4.8):
 *   TX/RX frame = [PDU header][len][payload] - identical to the lib's
 *   [pktTxType][txLen][data] "type first" layout (rfConfig bit7 = 1).
 *
 * v1 timing model is software scheduled (see ble_phy_tx_set_start_time).
 * All board-calibratable constants live in struct wch_rf_timing (wch_rf.h).
 */

#include <stdint.h>
#include <string.h>

#include "syscfg/syscfg.h"
#include "os/os.h"
#include "os/os_cputime.h"
#include "hal/hal_timer.h"
#include "nimble/ble.h"
#include "nimble/nimble_opt.h"
#include "controller/ble_phy.h"
#include "controller/ble_ll.h"

#include "wch_rf.h"

#ifndef min
#define min(a, b) ((a) < (b) ? (a) : (b))
#endif

/* ------------------------------------------------------------------------- */
/* State                                                                     */
/* ------------------------------------------------------------------------- */

struct ble_phy_obj
{
    uint8_t phy_state;          /* BLE_PHY_STATE_*                        */
    uint8_t phy_transition;     /* BLE_PHY_TRANSITION_* for the TX in fly */
    uint8_t phy_rx_started;     /* frame reception has started            */
    uint8_t phy_encrypted;      /* link encryption armed                  */
    uint8_t phy_privacy;        /* resolv list enabled flag               */
    uint8_t phy_chan;           /* current BLE channel 0..39              */
    uint8_t phy_tx_phy_mode;    /* BLE_PHY_MODE_1M / _2M                  */
    uint8_t phy_rx_phy_mode;
    uint8_t phy_dtm;            /* DTM mode flag (demod cfg, RE §4.4)     */
    uint32_t phy_access_address;
    uint32_t phy_crcinit;
    struct ble_mbuf_hdr rxhdr;
    void *txend_arg;
    ble_phy_tx_end_func txend_cb;
};

static struct ble_phy_obj g_ble_phy_data;

/* Plain counters instead of STATS_* to avoid a stats dependency (task spec) */
struct ble_phy_stats
{
    uint32_t phy_isrs;
    uint32_t tx_good;
    uint32_t tx_started;
    uint32_t tx_fail;
    uint32_t tx_late;
    uint32_t tx_bytes;
    uint32_t rx_starts;
    uint32_t rx_aborts;
    uint32_t rx_valid;
    uint32_t rx_crc_err;
    uint32_t rx_late;
    uint32_t rx_timeouts;
    uint32_t radio_state_errs;
    uint32_t no_bufs;
    uint32_t hw_errs;
};

static struct ble_phy_stats g_ble_phy_stats;

/* TX frame: [hdr][len][payload]; RX buffer keeps a small guard tail */
static uint8_t g_ble_phy_tx_buf[BLE_PHY_MAX_PDU_LEN] __attribute__((aligned(4)));
#define g_ble_phy_rx_buf (wch_rf_rx_frame())
static struct hal_timer s_phy_wfr_timer;
static void phy_rx_timeout_isr(void);

static void phy_wfr_expired(void *arg)
{
    (void)arg;
    phy_rx_timeout_isr();
}

/* Software scheduling (v1 timing model) */
static uint32_t s_rx_end_cputime;
static uint8_t s_rx_end_payload_len;
/* Board-calibrated at 96 MHz: wait from RX done before RF preparation.
 * The remaining RF path plus ~34 us hardware start delay completes T_IFS.
 * Scope-level timing/conformance and other clock rates remain unvalidated. */
volatile uint32_t g_wch_phy_response_delay_us = 108;
static uint8_t s_tx_sched_valid;
static uint32_t s_tx_sched_cputime;
static uint8_t s_tx_sched_rem_us;
static uint8_t s_rx_sched_valid;
static uint32_t s_rx_air_start_cputime;
static int ble_phy_rx(void);
static uint32_t s_rx_sched_cputime;
static uint8_t s_rx_sched_rem_us;

/* Link encryption (AES-CCM) cache, RE §12.5 */
static uint8_t s_enc_key[16];
static uint8_t s_enc_iv[8];
static uint64_t s_enc_counter;
static uint8_t s_enc_dir_bit;
static uint8_t s_enc_hdr_mask;

/* ------------------------------------------------------------------------- */
/* Helpers                                                                   */
/* ------------------------------------------------------------------------- */

static uint32_t
load32_le(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Generous hardware TX busy-window: preamble+AA+CRC ~ 80 us + 8 us/byte. */
static uint32_t
tx_window_half_us(uint8_t pyld_len)
{
    uint32_t airtime_us = 80UL + 8UL * ((uint32_t)pyld_len + 2UL);

    return 2UL * airtime_us + 100UL;
}

/* Default RX window (0.5 us units) when the LL gives no wfr value. */
static uint32_t
rx_window_default_half_us(void)
{
    return g_wch_rf_timing.wfr_half_us_1m + g_wch_rf_timing.rx_window_widen_half_us;
}

/*
 * v1 software scheduling: spin until (sched_start - tx_guard) is reached.
 * Bounded: returns -1 once (sched_start + tx_late_guard) has passed.
 * TODO(board): replace with a hardware compare/timer driven start
 * (RE §12.10 lists precise scheduling as an open item).
 */
static int
wait_until_start(uint32_t sched_cputime, uint8_t rem_us)
{
    uint32_t guard;
    uint32_t late;
    uint32_t now;

    if (!s_tx_sched_valid && !s_rx_sched_valid) {
        return 0;
    }
    (void)rem_us;   /* cputime runs at 1 MHz; rem_us sub-tick is ignored */

    guard = sched_cputime - g_wch_rf_timing.tx_guard_us;
    late = sched_cputime + g_wch_rf_timing.tx_late_guard_us;

    for (;;) {
        now = os_cputime_get32();
        if ((int32_t)(now - guard) >= 0) {
            if ((int32_t)(now - late) > 0) {
                return -1;
            }
            return 0;
        }
    }
}

/*
 * Program the AES-CCM context for the next packet, RE §12.5
 * AES_DevPktEnc/Dec:
 *   LLE[0x50] |= 0x80 (CCM armed)
 *   AES[0x00] = dir<<5 | 0x80
 *   AES[0x10/0x14] = IV(8B), AES[0x08/0x0C] = counter, AES[0x28..] = key
 *   AES[0x00] |= 0x40 (commit)
 * LLE[0x50] mode-word bit5 (0x20) is ORed in by wch_rf at every TX/RX arm
 * (wch_rf_encrypt_get()).
 *
 * v1: sequence transcribed from RE but NOT yet exercised on hardware.
 * The header mask (Core 5.3 Vol 6 2.2) has no RE-identified register and is
 * cached only - encrypted control PDUs may behave differently (board test).
 */
static void
phy_encrypt_ccm_program(void)
{
    int i;

    WCH_LLE_MODE |= WCH_LLE_MODE_CCM_ARMED;             /* RE §12.5 */
    WCH_AES_CTRL = (s_enc_dir_bit ? WCH_AES_CTRL_DIR_DECRYPT : 0) |
                   WCH_AES_CTRL_CCM;
    WCH_AES_IV_LO = load32_le(s_enc_iv);
    WCH_AES_IV_HI = load32_le(s_enc_iv + 4);
    WCH_AES_CNT_LO = (uint32_t)s_enc_counter;
    /* 39-bit packet counter: RE §12.5 LL_ConnectTx/RxPktnumIncrease. High
     * word usage beyond bit 6 not confirmed; mask conservatively. */
    WCH_AES_CNT_HI = (uint32_t)(s_enc_counter >> 32) & 0x7FUL;
    for (i = 0; i < 4; i++) {
        WCH_AES_KEY(i) = load32_le(s_enc_key + i * 4);
    }
    WCH_AES_CTRL |= WCH_AES_CTRL_CCM_COMMIT;            /* RE §12.5 */

    wch_rf_encrypt_set(true);
}

/* ------------------------------------------------------------------------- */
/* ISR-context handlers (run from wch_rf callback == LLE/BB ISR context)     */
/* ------------------------------------------------------------------------- */

static void
phy_tx_done_isr(uint32_t ts)
{
    uint8_t transition = g_ble_phy_data.phy_transition;

    (void)ts;

    if (g_ble_phy_data.phy_state != BLE_PHY_STATE_TX) {
        return;
    }

    ++g_ble_phy_stats.tx_good;
    if (transition == BLE_PHY_TRANSITION_TX_RX) {
        /* Basic sequencer stages have unambiguous completion flags.
         * Listen after TX while the controller waits for a scan/connect
         * request; the bounded WFR callback also closes an empty window. */
        g_ble_phy_data.phy_state = BLE_PHY_STATE_RX;
        g_ble_phy_data.phy_rx_started = 0;
        uint32_t window_us = BLE_LL_IFS + g_wch_rf_timing.ack_window_half_us_1m / 2U;
        if (wch_rf_rx_basic(g_ble_phy_rx_buf, WCH_RF_RX_BUFFER_SIZE,
                            window_us * 2U) != 0) {
            phy_rx_timeout_isr();
        } else {
            (void)hal_timer_start(&s_phy_wfr_timer, window_us + 100U);
        }
    } else {
        g_ble_phy_data.phy_state = BLE_PHY_STATE_IDLE;
    }

    if (g_ble_phy_data.txend_cb) {
        g_ble_phy_data.txend_cb(g_ble_phy_data.txend_arg);
    }
}

static void
phy_rx_done_isr(uint32_t ts)
{
    uint8_t *rxbuf = g_ble_phy_rx_buf;
    struct ble_mbuf_hdr *ble_hdr = &g_ble_phy_data.rxhdr;
    int rc;

    if (g_ble_phy_data.phy_state != BLE_PHY_STATE_RX) {
        return;
    }

    s_rx_end_cputime = ts;
    s_rx_end_payload_len = rxbuf[1];
    (void)hal_timer_stop(&s_phy_wfr_timer);
    g_ble_phy_data.phy_rx_started = 1;
    ++g_ble_phy_stats.rx_starts;

    /* BB clock capture is after address detection, not the first preamble
     * bit. Infer packet start from completed airtime instead of handing that
     * later capture point to the connection scheduler as its anchor. */
    ble_hdr->rxinfo.flags = ble_ll_state_get();
    ble_hdr->rxinfo.channel = g_ble_phy_data.phy_chan;
    ble_hdr->rxinfo.handle = 0;
    ble_hdr->rxinfo.rssi = wch_rf_rssi();
    ble_hdr->rxinfo.phy_mode = g_ble_phy_data.phy_rx_phy_mode;
    ble_hdr->rxinfo.phy = ble_hdr->rxinfo.phy_mode == BLE_PHY_MODE_2M ?
                          BLE_PHY_2M : BLE_PHY_1M;
    uint32_t airtime_us = ble_hdr->rxinfo.phy_mode == BLE_PHY_MODE_2M ?
                          4U * (rxbuf[1] + 11U) : 8U * (rxbuf[1] + 10U);
    ble_hdr->beg_cputime = ts - airtime_us;
    ble_hdr->rem_usecs = 0;

    /*
     * ble_ll_rx_start(): v1 calls it at RX completion - the WCH modem gives
     * no independent address-match event to the CPU before the whole frame
     * is in (the BB bit6 clock-capture event is the closest analogue and is
     * used for the timestamp).  The rx_isr_start hooks mostly cancel the wfr
     * timer / record state; calling them late is the tolerated deviation
     * (see ble_ll.c wfr logic: rx_started() short-circuits wfr handling).
     */
    rc = ble_ll_rx_start(rxbuf, g_ble_phy_data.phy_chan, ble_hdr);
    if (rc < 0) {
        ble_phy_disable();
        ++g_ble_phy_stats.rx_aborts;
        return;
    }

    /* Vendor ble_ll_chkcrc reads BB+0x38 bit5 after actual RX completion. */
    if (wch_rf_rx_crc_ok()) {
        ble_hdr->rxinfo.flags |= BLE_MBUF_HDR_F_CRC_OK;
        ++g_ble_phy_stats.rx_valid;
    } else {
        ++g_ble_phy_stats.rx_crc_err;
    }

    rc = ble_ll_rx_end(rxbuf, ble_hdr);
    if (rc < 0) {
        ble_phy_disable();
    }
}

static void
phy_rx_timeout_isr(void)
{
    if (g_ble_phy_data.phy_state != BLE_PHY_STATE_RX) {
        return;
    }

    if (!g_ble_phy_data.phy_rx_started) {
        /* Nothing received: wfr expiry.  Same pattern as nrf5x
         * ble_phy_isr DISABLED-without-start -> ble_ll_wfr_timer_exp. */
        ble_phy_disable();
        ++g_ble_phy_stats.rx_timeouts;
        ble_ll_wfr_timer_exp(NULL);
    } else {
        /* Frame started but engine reports timeout - treat as HW error. */
        ble_phy_disable();
        ++g_ble_phy_stats.hw_errs;
        ble_ll_hw_error();
    }
}

static void
wch_phy_rf_event(enum wch_rf_event evt, uint32_t payload)
{
    ++g_ble_phy_stats.phy_isrs;

    switch (evt) {
    case WCH_RF_EVT_TX_DONE:
        phy_tx_done_isr(payload);
        break;
    case WCH_RF_EVT_RX_START:
        if (g_ble_phy_data.phy_state == BLE_PHY_STATE_RX) {
            g_ble_phy_data.phy_rx_started = 1;
            (void)hal_timer_stop(&s_phy_wfr_timer);
        }
        break;
    case WCH_RF_EVT_RX_DONE:
        phy_rx_done_isr(payload);
        break;
    case WCH_RF_EVT_RX_TIMEOUT:
        phy_rx_timeout_isr();
        break;
    default:
        ++g_ble_phy_stats.hw_errs;
        break;
    }
}

/* ------------------------------------------------------------------------- */
/* ble_phy.h implementation                                                  */
/* ------------------------------------------------------------------------- */

int
ble_phy_init(void)
{
    memset(&g_ble_phy_data, 0, sizeof(g_ble_phy_data));
    memset(&g_ble_phy_stats, 0, sizeof(g_ble_phy_stats));

    g_ble_phy_data.phy_state = BLE_PHY_STATE_IDLE;
    g_ble_phy_data.phy_chan = BLE_PHY_NUM_CHANS;    /* invalid: forces setchan */
    g_ble_phy_data.phy_tx_phy_mode = BLE_PHY_MODE_1M;
    g_ble_phy_data.phy_rx_phy_mode = BLE_PHY_MODE_1M;

    s_tx_sched_valid = 0;
    s_rx_sched_valid = 0;

    (void)hal_timer_set_cb(MYNEWT_VAL(OS_CPUTIME_TIMER_NUM), &s_phy_wfr_timer,
                           phy_wfr_expired, NULL);

    /* Driver events arrive through the wch_rf ISR callback. */
    wch_rf_set_callback(wch_phy_rf_event);

    return 0;
}

int
ble_phy_setchan(uint8_t chan, uint32_t access_addr, uint32_t crcinit)
{
    if (chan >= BLE_PHY_NUM_CHANS) {
        return BLE_PHY_ERR_INV_PARAM;
    }

    g_ble_phy_data.phy_access_address = access_addr;
    g_ble_phy_data.phy_crcinit = crcinit & 0xFFFFFFUL;
    g_ble_phy_data.phy_chan = chan;

    /* Program the synthesizer now (RE §4.2); re-programmed again inside the
     * TX/RX arm sequences exactly like RF_Tx/RF_Rx do. */
    wch_rf_set_channel(chan);
    wch_rf_aa_crc_set(access_addr, crcinit);

    return 0;
}

uint8_t
ble_phy_chan_get(void)
{
    return g_ble_phy_data.phy_chan;
}

#if MYNEWT_VAL(BLE_PHY_VARIABLE_TIFS)
void
ble_phy_tifs_set(uint16_t tifs)
{
    /* v1: T_IFS is fixed by the LLE turnaround table (RE §5.3); stored for
     * future use. TODO(board). */
    (void)tifs;
}
#endif

void
ble_phy_tifs_txtx_set(uint16_t usecs, uint8_t anchor)
{
    /* TX-TX transitions not used by this port (no ISO / CTE). */
    (void)usecs;
    (void)anchor;
}

int
ble_phy_tx_set_start_time(uint32_t cputime, uint8_t rem_usecs)
{
    s_tx_sched_valid = 1;
    s_tx_sched_cputime = cputime;
    s_tx_sched_rem_us = rem_usecs;

    /* Late check happens in ble_phy_tx() (returns BLE_PHY_ERR_TX_LATE). */
    return 0;
}

int
ble_phy_rx_set_start_time(uint32_t cputime, uint8_t rem_usecs)
{
    s_rx_sched_valid = 1;
    s_rx_sched_cputime = cputime;
    s_rx_sched_rem_us = rem_usecs;

    if ((int32_t)(os_cputime_get32() -
                  (cputime + g_wch_rf_timing.tx_late_guard_us)) > 0) {
        ++g_ble_phy_stats.rx_late;
        /* Like nrf5x: still attempt reception. */
    }
    /* LL callers do not call a separate RX arm function. */
    return ble_phy_rx();
}

void
ble_phy_set_txend_cb(ble_phy_tx_end_func txend_cb, void *arg)
{
    g_ble_phy_data.txend_cb = txend_cb;
    g_ble_phy_data.txend_arg = arg;
}

int
ble_phy_tx(ble_phy_tx_pducb_t pducb, void *pducb_arg, uint8_t end_trans)
{
    uint8_t hdr_byte;
    uint8_t pyld_len;
    int rc;

    bool response = ble_phy_state_get() == BLE_PHY_STATE_RX &&
                    g_ble_phy_data.phy_rx_started;
    /* The LL requests a scan/data response from ble_ll_rx_end. */
    if (ble_phy_state_get() != BLE_PHY_STATE_IDLE &&
        !(ble_phy_state_get() == BLE_PHY_STATE_RX &&
          g_ble_phy_data.phy_rx_started)) {
        ble_phy_disable();
        ++g_ble_phy_stats.radio_state_errs;
        return BLE_PHY_ERR_RADIO_STATE;
    }

    if (pducb == NULL) {
        return BLE_PHY_ERR_INV_PARAM;
    }

    if (g_ble_phy_data.phy_encrypted) {
        phy_encrypt_ccm_program();
    }

    /* Build the frame: [hdr][len][payload] (pducb writes the payload and
     * returns its length, RE §4.6 [pktTxType][txLen][data]). */
    pyld_len = pducb(g_ble_phy_tx_buf + BLE_LL_PDU_HDR_LEN, pducb_arg,
                     &hdr_byte);
    g_ble_phy_tx_buf[0] = hdr_byte;
    g_ble_phy_tx_buf[1] = pyld_len;

    g_ble_phy_data.phy_transition = end_trans;
    wch_rf_auto_arm_rx(end_trans == BLE_PHY_TRANSITION_TX_RX);

    /* v1 software scheduling: busy-wait to (start - guard). */
    if (s_tx_sched_valid &&
        wait_until_start(s_tx_sched_cputime, s_tx_sched_rem_us) != 0) {
        s_tx_sched_valid = 0;
        ++g_ble_phy_stats.tx_late;
        g_ble_phy_data.phy_state = BLE_PHY_STATE_IDLE;
        wch_rf_auto_arm_rx(false);
        return BLE_PHY_ERR_TX_LATE;
    }
    s_tx_sched_valid = 0;

    if (response) {
        /* Basic-mode responses need an explicit IFS wait. Starting a new
         * TX immediately replies too early for the central's RX window. */
        /* Vendor LLE_IRQSubHandler 0x33902..0x339ee uses turnaround
         * {93,97,101,105,107} half-us for RX payload {0,1,2,3,>=4}.
         * Short frames reach the CPU later relative to their air end. */
        uint32_t short_adjust = s_rx_end_payload_len < 4 ?
            (107U - (93U + 4U * s_rx_end_payload_len)) / 2U : 0U;
        uint32_t delay = g_wch_phy_response_delay_us > short_adjust ?
            g_wch_phy_response_delay_us - short_adjust : 0U;
        uint32_t start = s_rx_end_cputime + delay;
        while ((int32_t)(os_cputime_get32() - start) < 0) {
        }
    }
    g_ble_phy_data.phy_state = BLE_PHY_STATE_TX;

    rc = wch_rf_tx_basic(g_ble_phy_tx_buf,
                         (uint16_t)(pyld_len + BLE_LL_PDU_HDR_LEN),
                         tx_window_half_us(pyld_len));
    if (rc != 0) {
        ++g_ble_phy_stats.tx_fail;
        g_ble_phy_data.phy_state = BLE_PHY_STATE_IDLE;
        wch_rf_auto_arm_rx(false);
        return BLE_PHY_ERR_RADIO_STATE;
    }

    ++g_ble_phy_stats.tx_started;
    g_ble_phy_stats.tx_bytes += (uint32_t)pyld_len + BLE_LL_PDU_HDR_LEN;
    return BLE_ERR_SUCCESS;
}

void
ble_phy_rxpdu_copy(uint8_t *dptr, struct os_mbuf *rxpdu)
{
    struct os_mbuf *m;
    struct ble_mbuf_hdr *ble_hdr;
    uint16_t rem_bytes;
    uint16_t space;

    rem_bytes = OS_MBUF_PKTHDR(rxpdu)->omp_len;
    m = rxpdu;

    while ((m != NULL) && (rem_bytes > 0)) {
        space = m->om_omp->omp_databuf_len - m->om_pkthdr_len;
        if (m == rxpdu) {
            space -= 4;     /* native driver reserves 4 bytes in the head */
        }
        if (space > rem_bytes) {
            space = rem_bytes;
        }
        memcpy(m->om_data + m->om_len, dptr, space);
        m->om_len += space;
        dptr += space;
        rem_bytes -= space;
        m = SLIST_NEXT(m, om_next);
    }

    ble_hdr = BLE_MBUF_HDR_PTR(rxpdu);
    memcpy(ble_hdr, &g_ble_phy_data.rxhdr, sizeof(*ble_hdr));
}

int
ble_phy_tx_power_set(int dbm)
{
    /*
     * RE limitation: the TX power register was not identified (the lib uses
     * an enumeration level, bleConfig_t TxPower default 9, RE §7).  v1 rails
     * and caches the value only; the RFEND default power applies.
     * TODO(board): locate the RFEND TX power field and program it.
     */
    if (dbm > BLE_PHY_MAX_PWR_DBM) {
        dbm = BLE_PHY_MAX_PWR_DBM;
    }
    if (dbm < -20) {
        dbm = -20;
    }
    (void)dbm;
    return 0;
}

int
ble_phy_tx_power_round(int dbm)
{
    if (dbm > BLE_PHY_MAX_PWR_DBM) {
        dbm = BLE_PHY_MAX_PWR_DBM;
    }
    if (dbm < -20) {
        dbm = -20;
    }
    return dbm;
}

int
ble_phy_tx_power_get(void)
{
    return 0;   /* see ble_phy_tx_power_set: not programmable in v1 */
}

void
ble_phy_disable(void)
{
    (void)hal_timer_stop(&s_phy_wfr_timer);
    wch_rf_shut();
    wch_rf_auto_arm_rx(false);
    wch_rf_encrypt_set(false);
    g_ble_phy_data.phy_state = BLE_PHY_STATE_IDLE;
    g_ble_phy_data.phy_rx_started = 0;
    s_tx_sched_valid = 0;
    s_rx_sched_valid = 0;
}

static int
ble_phy_rx(void)
{
    int rc;

    if (ble_phy_state_get() != BLE_PHY_STATE_IDLE) {
        ble_phy_disable();
        ++g_ble_phy_stats.radio_state_errs;
        return BLE_PHY_ERR_RADIO_STATE;
    }

    if (g_ble_phy_data.phy_encrypted) {
        phy_encrypt_ccm_program();
    }

    /* v1 software scheduling: wait until (start - guard).  Never late-error
     * on RX (attempt anyway, like nrf5x). */
    s_rx_air_start_cputime = s_rx_sched_valid ?
        s_rx_sched_cputime + s_rx_sched_rem_us : os_cputime_get32();
    if (s_rx_sched_valid) {
        (void)wait_until_start(s_rx_sched_cputime, s_rx_sched_rem_us);
        s_rx_sched_valid = 0;
    }

    g_ble_phy_data.phy_state = BLE_PHY_STATE_RX;
    g_ble_phy_data.phy_rx_started = 0;

    /* Arm the engine with the default window; ble_phy_wfr_enable() refines
     * the window (LLE[0x64] is a live schedule value, RE §12.2).  Arming
     * here also covers scan flows that never call wfr_enable. */
    rc = wch_rf_rx_basic(g_ble_phy_rx_buf, WCH_RF_RX_BUFFER_SIZE,
                         rx_window_default_half_us());
    if (rc != 0) {
        g_ble_phy_data.phy_state = BLE_PHY_STATE_IDLE;
        ++g_ble_phy_stats.radio_state_errs;
        return BLE_PHY_ERR_RADIO_STATE;
    }

    return 0;
}

void
ble_phy_restart_rx(void)
{
    if (g_ble_phy_data.phy_state == BLE_PHY_STATE_RX) {
        return;
    }
    wch_rf_rx_basic(g_ble_phy_rx_buf, WCH_RF_RX_BUFFER_SIZE,
                    rx_window_default_half_us());
    g_ble_phy_data.phy_state = BLE_PHY_STATE_RX;
    g_ble_phy_data.phy_rx_started = 0;
}

void
ble_phy_wfr_enable(int txrx, uint8_t tx_phy_mode, uint32_t wfr_usecs)
{
    uint32_t window;

    (void)tx_phy_mode;  /* 1M only in v1 */

    if (txrx == BLE_PHY_WFR_ENABLE_TXRX) {
        /*
         * Post-TX RX: with TRANSITION_TX_RX the wch_rf TX-done ISR already
         * armed the T_IFS ACK window (RE §4.8/§5.3) - nothing to do.  If the
         * engine is not in RX (should not happen), fall through and arm.
         */
        if (g_ble_phy_data.phy_state == BLE_PHY_STATE_RX) {
            return;
        }
    }

    if (g_ble_phy_data.phy_state != BLE_PHY_STATE_RX) {
        if (ble_phy_rx() != 0) {
            return;
        }
    }

    /* WFR is relative to the requested on-air RX start, not to the early
     * scheduler callback or the instant we program the receiver. Include
     * preamble/access-address detection and the existing capture allowance. */
    uint32_t addr_us = g_ble_phy_data.phy_rx_phy_mode == BLE_PHY_MODE_2M ? 24U : 40U;
    uint32_t deadline = s_rx_air_start_cputime + wfr_usecs + addr_us +
                       g_wch_rf_timing.rx_window_widen_half_us / 2U;
    int32_t remaining = (int32_t)(deadline - os_cputime_get32());
    if (remaining < 1) {
        remaining = 1;
    }
    window = (uint32_t)remaining * 2U;
    WCH_LLE_WINDOW = window;
    (void)hal_timer_start(&s_phy_wfr_timer, (uint32_t)remaining + 100U);

}

void
ble_phy_rfclk_enable(void)
{
    /* HSE runs continuously (OSC load trim done in wch_rf_init). */
}

void
ble_phy_rfclk_disable(void)
{
}

int
ble_phy_state_get(void)
{
    return g_ble_phy_data.phy_state;
}

uint8_t
ble_phy_xcvr_state_get(void)
{
    return g_ble_phy_data.phy_state;
}

int
ble_phy_rx_started(void)
{
    return g_ble_phy_data.phy_rx_started;
}

uint8_t
ble_phy_max_data_pdu_pyld(void)
{
    return BLE_LL_DATA_PDU_MAX_PYLD;
}

uint32_t
ble_phy_access_addr_get(void)
{
    return g_ble_phy_data.phy_access_address;
}

#if MYNEWT_VAL(BLE_LL_CFG_FEAT_LE_ENCRYPTION)
void
ble_phy_encrypt_enable(const uint8_t *key)
{
    if (key) {
        memcpy(s_enc_key, key, 16);
    }
    g_ble_phy_data.phy_encrypted = 1;
}

void
ble_phy_encrypt_header_mask_set(uint8_t mask)
{
    /* No RE-identified register; cached only (see phy_encrypt_ccm_program). */
    s_enc_hdr_mask = mask;
}

void
ble_phy_encrypt_iv_set(const uint8_t *iv)
{
    if (iv) {
        memcpy(s_enc_iv, iv, 8);
    }
}

void
ble_phy_encrypt_counter_set(uint64_t counter, uint8_t dir_bit)
{
    s_enc_counter = counter;
    s_enc_dir_bit = dir_bit;
}

void
ble_phy_encrypt_disable(void)
{
    g_ble_phy_data.phy_encrypted = 0;
    s_enc_counter = 0;
    wch_rf_encrypt_set(false);
}
#endif

void
ble_phy_resolv_list_enable(void)
{
    g_ble_phy_data.phy_privacy = 1;
}

void
ble_phy_resolv_list_disable(void)
{
    g_ble_phy_data.phy_privacy = 0;
}

/* Unconditional: upstream ble_ll_adv/conn/scan call sites are not feature-
 * gated, so the symbol must exist even with 2M/coded disabled.  v1 clamps
 * every mode to 1M inside wch_rf_phy_mode_set(). */
void
ble_phy_mode_set(uint8_t tx_phy_mode, uint8_t rx_phy_mode)
{
    /* Coded modes clamp to 1M in v1 (see wch_rf_phy_mode_set). */
    wch_rf_phy_mode_set(tx_phy_mode, rx_phy_mode);
    if (tx_phy_mode == BLE_PHY_MODE_2M) {
        g_ble_phy_data.phy_tx_phy_mode = BLE_PHY_MODE_2M;
    } else {
        g_ble_phy_data.phy_tx_phy_mode = BLE_PHY_MODE_1M;
    }
    if (rx_phy_mode == BLE_PHY_MODE_2M) {
        g_ble_phy_data.phy_rx_phy_mode = BLE_PHY_MODE_2M;
    } else {
        g_ble_phy_data.phy_rx_phy_mode = BLE_PHY_MODE_1M;
    }
}

#if MYNEWT_VAL(BLE_LL_DTM)
void
ble_phy_enable_dtm(void)
{
    g_ble_phy_data.phy_dtm = 1;
    wch_rf_dtm_set(true);       /* demod cfg14 0x34A4DF variant - RE §4.4 */
}

void
ble_phy_disable_dtm(void)
{
    g_ble_phy_data.phy_dtm = 0;
    wch_rf_dtm_set(false);
}
#endif
