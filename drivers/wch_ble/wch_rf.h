/*
 * wch_rf.h - WCH CH32V208 BLE baseband / link-layer-engine / RF-AFE register
 *            level driver (reverse-engineered from libwchble.a / wchble_rom).
 *
 * Vendor instructions and measured behavior take precedence over comments.
 * Historical notes in WCH_BLE_LIB_RE_FINDINGS.md (workspace root, "RE §x.y")
 * include unverified assumptions; they are investigation pointers.
 *
 * This driver implements the raw RF primitives ONLY. The Apache NimBLE glue
 * lives in ble_phy_wch.c / ble_hw_wch.c next to this file.
 *
 * No libwchble.a, no TMOS. All radio operations are direct register writes.
 */

#ifndef H_WCH_RF_
#define H_WCH_RF_

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------------- */
/* Register base addresses - RE §3 (BLE_IPCoreInit assigns these constants)  */
/* ------------------------------------------------------------------------- */

#define WCH_BLE_TOP_BASE    0x40024000UL    /* RE §3: BLE IP top level            */
#define WCH_BB_BASE         0x40024100UL    /* RE §3: BB (baseband digital)       */
#define WCH_LLE_BASE        0x40024200UL    /* RE §3: LLE (link layer engine)     */
#define WCH_AES_BASE        0x40024300UL    /* RE §3/§9.7: AES hardware           */
#define WCH_RFEND_BASE      0x40025000UL    /* RE §3: RFEND (RF analog front end) */
#define WCH_BLE_MISC_BASE   0x40022000UL    /* RE §3: misc area ([0x48] CTE bit)  */

#define WCH_REG32(base, off) \
    (*(volatile uint32_t *)((uintptr_t)(base) + (uintptr_t)(off)))

/* Guard word 0x4002420C == LLE + 0x0C - RE §3 / §4.3 (save/clear + fence.i) */
#define WCH_RF_GUARD_REG    WCH_REG32(WCH_LLE_BASE, 0x0C)

/* ------------------------------------------------------------------------- */
/* BB registers (RE §3.1, §4.2, §4.4, §4.6, §4.9, §5.2)                      */
/* ------------------------------------------------------------------------- */

/*
 * BB +0x00 (RFCTRL) bit fields - RE §4.2:
 *   [5:0]   BLE channel number
 *   bit6    whitening disabled
 *   bit7    "shut" state
 *   bit8    direction (both TX and RX sequences set (b&~0x180)|0x100)
 *   [13:12] PHY mode: 01=1M, 00=2M, 10=coded; coded bit14 set = S=2 coding
 *           (vendor BLE_SetPHYTxMode 0x135ba: phy2 -> 0x2000, phy3 -> 0x6000;
 *           phy2 window matches S=8 airtime +190us, phy3 matches S=2 +190us)
 *   bit23   TX trigger (RF_Tx final step: BB[0x00]|=0x800000, RE §4.6)
 *   bit29   CTE enable (not used here)
 *   bit28   set by BB_DevInit (0x10000000)
 *
 * RX never sets bit14: the demodulator learns the coding scheme from the
 * CI field of each received packet (BLE_SetPHYRxMode 0x13752 writes 0x2000
 * for both coded vendor enums).
 */
#define WCH_BB_RFCTRL           (WCH_REG32(WCH_BB_BASE, 0x00))
#define WCH_BB_RFCTRL_CH_MASK       0x0000003FUL
#define WCH_BB_RFCTRL_WHITEN_OFF    0x00000040UL
#define WCH_BB_RFCTRL_SHUT          0x00000080UL
#define WCH_BB_RFCTRL_DIR_RX        0x00000100UL
#define WCH_BB_RFCTRL_DIR_MASK      0x00000180UL
#define WCH_BB_RFCTRL_TX_PRETRIG    0x00800000UL
#define WCH_BB_RFCTRL_PHY_MASK      0x00003000UL
#define WCH_BB_RFCTRL_PHY_1M        0x00001000UL
#define WCH_BB_RFCTRL_PHY_2M        0x00000000UL    /* RE §4.4 */
#define WCH_BB_RFCTRL_PHY_CODED     0x00002000UL    /* bits[13:12]=10, RE §4.4 */
#define WCH_BB_RFCTRL_PHY_S2        0x00004000UL    /* bit14: S=2 TX coding    */
#define WCH_BB_RFCTRL_CODING_MASK   0x0000C000UL    /* vendor clears bits15:14 before coded TX */
#define WCH_BB_RFCTRL_TX_TRIG       0x00800000UL    /* bit23 - RE §4.6 */

/* BB +0x04 = CRC init value; BB +0x08 = access address (RE §4.6/§4.7/§12.6) */
#define WCH_BB_CRCINIT          (WCH_REG32(WCH_BB_BASE, 0x04))
#define WCH_BB_ACCESSADDR       (WCH_REG32(WCH_BB_BASE, 0x08))

/* RX group byte offsets verified in BLE_SetPHYRxMode 0x137c2..1382c.
 * 1M: +10=0x3222D0 (+10=0x3FA2CE in DTM), +14=0x08101901,
 * +18=0x31624, +20=0x90083, +24=0x01006310, +28=0x28BE.
 * Coded (vendor enum 2/3, 0x13752..0x137c0; one group for S=2 AND S=8):
 * +10=0x34A4DF (+10=0x3FA4DF in DTM), +14=0x08301FF1, +18=0x31619,
 * +20=0x90086, +24=0x01006310, +28=0x28DE. */
#define WCH_BB_RXCFG10          (WCH_REG32(WCH_BB_BASE, 0x10))
#define WCH_BB_RXCFG14          (WCH_REG32(WCH_BB_BASE, 0x14))
#define WCH_BB_RXCFG18          (WCH_REG32(WCH_BB_BASE, 0x18))
#define WCH_BB_RXCFG20          (WCH_REG32(WCH_BB_BASE, 0x20))
#define WCH_BB_RXCFG24          (WCH_REG32(WCH_BB_BASE, 0x24))
#define WCH_BB_RXCFG28          (WCH_REG32(WCH_BB_BASE, 0x28))

/* Coded RX demod group (BLE_SetPHYRxMode 0x13760..0x1379c write order) */
#define WCH_BB_RXCFG10_CODED        0x034A4DFUL
#define WCH_BB_RXCFG10_CODED_DTM    0x03FA4DFUL
#define WCH_BB_RXCFG14_CODED        0x08301FF1UL
#define WCH_BB_RXCFG18_CODED        0x00031619UL
#define WCH_BB_RXCFG20_CODED        0x00090086UL
#define WCH_BB_RXCFG24_CODED        0x01006310UL
#define WCH_BB_RXCFG28_CODED        0x000028DEUL

/*
 * BB +0x2C:
 *   [1:0]  RX arm / auto-ack control: |=1 arms RX after TX (auto ack),
 *          &=~3 clears (RE §3.1, §4.6, §5.3)
 *   [30:25] TX power enum (LL_SetTxPowerLevel 0x348c; 0 dBm = enum 9).
 */
#define WCH_BB_AUTOCFG          (WCH_REG32(WCH_BB_BASE, 0x2C))
#define WCH_BB_AUTOCFG_ARM_MASK     0x00000003UL
#define WCH_BB_AUTOCFG_ARM_RX       0x00000001UL
#define WCH_BB_AUTOCFG_CFG_MASK     0x81FFFFFFUL    /* RE §3.1 */
#define WCH_BB_AUTOCFG_CFG_SHIFT    25

/* BB +0x34 = 464 (0x1D0) at init - RE §3.1 */
#define WCH_BB_CFG34            (WCH_REG32(WCH_BB_BASE, 0x34))
#define WCH_BB_CFG34_INIT       464UL

/*
 * BB +0x38 = IRQ status register (RE §5.2). Write the bit's clear-value to
 * acknowledge: bit6 <- 0x60, bit4 <- 0x10, bit7 <- 0x80.
 *   bit6: clock capture / sleep timing event (connection anchor capture)
 *   bit4/bit7: vendor acknowledges and sets a timer flag; not RX verdicts.
 */
#define WCH_BB_IRQSTAT          (WCH_REG32(WCH_BB_BASE, 0x38))
#define WCH_BB_IRQSTAT_CLKCAP       0x00000040UL
#define WCH_BB_IRQSTAT_CLKCAP_CLR   0x00000060UL

/* BB +0x30: bits[22:15] signed RSSI, bits[15:5] signed 11-bit CFO (RE §4.9) */
#define WCH_BB_RSSICFO          (WCH_REG32(WCH_BB_BASE, 0x30))
#define WCH_BB_RSSI_SHIFT           15
#define WCH_BB_CFO_SHIFT            5

/* ------------------------------------------------------------------------- */
/* LLE registers (RE §3.2, §4.3, §4.6, §4.7, §5.3, §12)                      */
/* ------------------------------------------------------------------------- */

/*
 * LLE +0x00 = engine command:
 *   1 = start RX, 2 = start TX (RE §4.3/§4.6/§4.7)
 *   (read & 3) != 0 = engine busy; |= 8 = force stop (RE §4.3/§12.1)
 */
#define WCH_LLE_CMD             (WCH_REG32(WCH_LLE_BASE, 0x00))
#define WCH_LLE_CMD_START_RX        0x00000001UL
#define WCH_LLE_CMD_START_TX        0x00000002UL
#define WCH_LLE_CMD_BUSY_MASK       0x00000003UL
#define WCH_LLE_CMD_FORCE_STOP      0x00000008UL

/*
 * LLE +0x04: in basic/advertise mode bit0 carries the LLEMode auto-ack bit
 * (vendor RF_Tx/RF_Rx: LLE[0x04]=(LLE[0x04]&~1)|(rfConfig&1)).
 * Other fields are not established by the basic-mode audit.
 */
#define WCH_LLE_CRC_AA_CFG      (WCH_REG32(WCH_LLE_BASE, 0x04))

/* LLE +0x08 W1C status; BASIC stages: bit0 RX done, bit1 TX done.
 * Auto sequencer meanings differ (vendor LLE_IRQSubHandler 0x33840).
 * Bit13=window expiry, bit14=clock tick. */
#define WCH_LLE_IRQSTAT         (WCH_REG32(WCH_LLE_BASE, 0x08))
#define WCH_LLE_IRQ_RX_DONE         0x00000001UL
#define WCH_LLE_IRQ_TX_DONE         0x00000002UL
#define WCH_LLE_IRQ_TICK_CLR        0x00004000UL
#define WCH_LLE_IRQ_CLEAR_RX        0x00002000UL    /* pre-arm clear - RE §4.7 */
#define WCH_LLE_IRQ_CLEAR_TX        0x00002000UL    /* pre-tx clear  - RE §4.4 */

/* LLE +0x0C == guard word 0x4002420C (see WCH_RF_GUARD_REG) */

/* LLE +0x14 is initialized to0x8C; it is not the mode register.
 * TX/RX/calibration mode values below are written to +0x50. */
#define WCH_LLE_TIMEOUT         (WCH_REG32(WCH_LLE_BASE, 0x14))
#define WCH_LLE_TIMEOUT_MASK        0x00000007UL
#define WCH_LLE_TIMEOUT_TX          0x5AUL      /* RE §4.6 */
#define WCH_LLE_TIMEOUT_RX          0x59UL      /* RE §4.7 */
#define WCH_LLE_TIMEOUT_CAL         93UL        /* RE §6.3 */

/*
 * LLE +0x1C = TX->RX turnaround constant written in the TX-done ISR
 * (RE §5.3). Plain 1M/2M by just-received length byte:
 *   0 -> 93, 1 -> 97, 2 -> 101, 3 -> 105, >=4 -> 107
 * encrypted: 0 -> 93, else 107; coded PHY: 108.  Unit presumed 0.5us.
 */
#define WCH_LLE_TURNAROUND      (WCH_REG32(WCH_LLE_BASE, 0x1C))

/*
 * LLE[0x24/0x2C/0x34/0x3C/0x44/0x4C] init table {0x8C,0x3C,0x8C,0x3C,0x8C,0x6C}
 * = per-PDU-type timeout baselines in LLE ticks (RE §3.2).
 */
#define WCH_LLE_INIT24          (WCH_REG32(WCH_LLE_BASE, 0x24))
#define WCH_LLE_INIT2C          (WCH_REG32(WCH_LLE_BASE, 0x2C))
#define WCH_LLE_INIT34          (WCH_REG32(WCH_LLE_BASE, 0x34))
#define WCH_LLE_INIT3C          (WCH_REG32(WCH_LLE_BASE, 0x3C))
#define WCH_LLE_INIT44          (WCH_REG32(WCH_LLE_BASE, 0x44))
#define WCH_LLE_INIT4C          (WCH_REG32(WCH_LLE_BASE, 0x4C))

/*
 * LLE +0x50 = mode word (RE §12.1 step 10):
 *   base 0x58 | (RX=1 | TX=2) | (CCM encrypt: 0x20); CCM armed flag |= 0x80
 *   (RE §12.5); calibration writes 93/0x5D, shutdown clears bits[2:0].
 */
#define WCH_LLE_MODE            (WCH_REG32(WCH_LLE_BASE, 0x50))
#define WCH_LLE_MODE_BASE           0x58UL
#define WCH_LLE_MODE_RX             0x01UL
#define WCH_LLE_MODE_TX             0x02UL
#define WCH_LLE_MODE_CCM            0x20UL
#define WCH_LLE_MODE_CCM_ARMED      0x80UL      /* RE §12.5 */
#define WCH_LLE_MODE_MASK           0x00000007UL

/*
 * LLE +0x64 dual role (RE §4.3, §5.3, §12.1/§12.2):
 *   read != 0 = engine busy (ll_tx/rx_wait_finish spin)
 *   write     = "next action" scheduling value in 0.5 us units:
 *             0 = clear, 0xA0 = TX (basic), RX window width, ACK window
 *             (vendor: 2M 0x196 = 203us, both coded modes 0x43E = 543us,
 *              1M 0x1BE = 223us; this basic-mode port adds its own IFS).
 */
#define WCH_LLE_WINDOW          (WCH_REG32(WCH_LLE_BASE, 0x64))
#define WCH_LLE_WINDOW_TX           0xA0UL      /* RE §4.6/§12.6 */

/* LLE +0x68 = tick reload (RE §5.3); +0x6C = 32k cal load word (RE §5.2) */
#define WCH_LLE_TICK            (WCH_REG32(WCH_LLE_BASE, 0x68))
#define WCH_LLE_CALLOAD         (WCH_REG32(WCH_LLE_BASE, 0x6C))

/* LLE +0x70 = direct TX frame DMA address; RX has separate +0x74. */
#define WCH_LLE_FRAMEPTR        (WCH_REG32(WCH_LLE_BASE, 0x70))
/* LLE +0x74 = direct RX DMA buffer address (vendor 0x13f22..13f2a,0x281c4). */
#define WCH_LLE_RXBUFPTR    (WCH_REG32(WCH_LLE_BASE, 0x74))

/* ------------------------------------------------------------------------- */
/* AES registers (RE §9.7 single-block ECB, §12.5 packet CCM handover)       */
/* ------------------------------------------------------------------------- */

/*
 * AES +0x00:
 *   ECB: 0x100 then 0 = reset, |1 = start (RE §9.7)
 *   CCM: dir<<5 | 0x80 (0=encrypt/1=decrypt), |0x40 = commit (RE §12.5)
 */
#define WCH_AES_CTRL            (WCH_REG32(WCH_AES_BASE, 0x00))
#define WCH_AES_CTRL_RESET          0x00000100UL
#define WCH_AES_CTRL_START          0x00000001UL
#define WCH_AES_CTRL_DIR_DECRYPT    0x00000020UL    /* bit5: 1=decrypt */
#define WCH_AES_CTRL_CCM            0x00000080UL
#define WCH_AES_CTRL_CCM_COMMIT     0x00000040UL

/* AES +0x04: bit0 busy (poll == 0), bits[1:0] done flags, clear with ~3 */
#define WCH_AES_STATUS          (WCH_REG32(WCH_AES_BASE, 0x04))
#define WCH_AES_STATUS_BUSY         0x00000001UL
#define WCH_AES_STATUS_DONE_MASK    0x00000003UL

/* AES +0x08/+0x0C = CCM counter/nonce words (RE §12.5) */
#define WCH_AES_CNT_LO          (WCH_REG32(WCH_AES_BASE, 0x08))
#define WCH_AES_CNT_HI          (WCH_REG32(WCH_AES_BASE, 0x0C))
/* AES +0x10/+0x14 = CCM IV, 8 bytes (RE §12.5) */
#define WCH_AES_IV_LO           (WCH_REG32(WCH_AES_BASE, 0x10))
#define WCH_AES_IV_HI           (WCH_REG32(WCH_AES_BASE, 0x14))
/* AES +0x18..0x24 = ECB data block in / out (RE §9.7) */
#define WCH_AES_BLOCK(i)        (WCH_REG32(WCH_AES_BASE, 0x18 + (i) * 4))
/* AES +0x28..0x34 = ECB/CCM key, 16 bytes (RE §9.7) */
#define WCH_AES_KEY(i)          (WCH_REG32(WCH_AES_BASE, 0x28 + (i) * 4))

/* ------------------------------------------------------------------------- */
/* RFEND registers (RE §3.3, §4.2, §4.9, §6)                                 */
/* ------------------------------------------------------------------------- */

/*
 * RFEND +0x04 trigger bits:
 *   bit0  = VCO tune trigger (TXCtune, RE §6.1)
 *   bit4  = RXFilter measure toggle (RE §6.2)
 *   bit16 = RX ADC re-init (RE §6.2)
 *   bit8  = TXFtune (RFEND[0x04] |= 0x100, RE §4.9)
 */
#define WCH_RFEND_TUNE          (WCH_REG32(WCH_RFEND_BASE, 0x04))
#define WCH_RFEND_TUNE_TRIG         0x00000001UL
#define WCH_RFEND_TUNE_TXFTUNE      0x00000100UL
#define WCH_RFEND_TUNE_RXADC        0x00010000UL

/* RFEND +0x08: bit16 RX ADC, bit21 RX filter, bits[21:20] "0x330000" = RF on */
#define WCH_RFEND_FECTRL        (WCH_REG32(WCH_RFEND_BASE, 0x08))
#define WCH_RFEND_FECTRL_RF_ON      0x00330000UL    /* RE §4.6/§4.7 */
#define WCH_RFEND_FECTRL_RXFILT     0x00200000UL    /* RE §6.2 */
#define WCH_RFEND_FECTRL_RXADC      0x00010000UL    /* RE §6.2 */
#define WCH_RFEND_FECTRL_SHUT       0xFFCDFFFFUL    /* RE §4.3: &=0xFFCDFFFF */

/* RFEND +0x0C: bit4 filter-measure toggle x2, bit8 RX ADC (RE §6.2) */
#define WCH_RFEND_CFG0C         (WCH_REG32(WCH_RFEND_BASE, 0x0C))
#define WCH_RFEND_CFG0C_FILT_TOGGLE 0x00000010UL
#define WCH_RFEND_CFG0C_RXADC       0x00000100UL

/*
 * RFEND +0x2C bit1: 0 = channel-number mode, 1 = frequency mode (RE §4.2).
 * BLE always uses frequency mode (exact synthesized carrier).
 */
#define WCH_RFEND_FREQMODE      (WCH_REG32(WCH_RFEND_BASE, 0x2C))
#define WCH_RFEND_FREQMODE_EN       0x00000002UL

/*
 * RFEND +0x38 bits[15:8] = TXCtune band code. Calibrated at 0xBF(2401MHz),
 * 0xD3(2440MHz), 0xE7(2480MHz) (RE §6.1).
 */
#define WCH_RFEND_BANDCODE      (WCH_REG32(WCH_RFEND_BASE, 0x38))
#define WCH_RFEND_BANDCODE_MASK     0xFFFE00FFUL    /* RE §6.1 */
#define WCH_RFEND_BANDCODE_SHIFT    8

/* RFEND +0x28 = 1152 at DevInit ("RFEND[40]=1152", RE §3.3) */
#define WCH_RFEND_CFG28         (WCH_REG32(WCH_RFEND_BASE, 0x28))
#define WCH_RFEND_CFG28_INIT        1152UL

/*
 * RFEND +0x44 = frequency synthesizer programming (RE §4.2) - THE core
 * formula:
 *   int  [24:20] = f_khz / 64000
 *   frac [17:0]  = ((f_khz % 64000) << 10) / 250     (LSB ~ 244 Hz)
 *   f_synth = 64000 kHz * (int + frac / 2^18)
 * BLE uses low-side LO injection: synth = carrier - 1000 kHz (1M PHY) or
 * carrier - 2000 kHz (2M PHY) (RE §4.2 "*").
 */
#define WCH_RFEND_SYNTH         (WCH_REG32(WCH_RFEND_BASE, 0x44))
#define WCH_RFEND_SYNTH_INT_MASK    0xFE0FFFFFUL
#define WCH_RFEND_SYNTH_INT_SHIFT   20
#define WCH_RFEND_SYNTH_FRAC_MASK   0xFFFC0000UL
#define WCH_RFEND_SYNTH_FRAC_MAX    0x0003FFFFUL

/* RFEND +0x48 / +0x4C: analog front end baseline bit fields (RE §3.3) */
#define WCH_RFEND_CFG48         (WCH_REG32(WCH_RFEND_BASE, 0x48))
#define WCH_RFEND_CFG4C         (WCH_REG32(WCH_RFEND_BASE, 0x4C))

/* RFEND +0x50: bits[4:0] RX filter result write-back; bits[15:12]=9 sens. */
#define WCH_RFEND_CFG50         (WCH_REG32(WCH_RFEND_BASE, 0x50))
#define WCH_RFEND_CFG50_FILT_MASK   0x0000FFE0UL    /* clear bits[4:0]|bit16 */
#define WCH_RFEND_CFG50_FILT_EN     0x00010000UL    /* &=~0x10000 - RE §6.2  */
#define WCH_RFEND_CFG50_SENS_MASK   0x0000F0FFUL    /* bits[15:12] */
#define WCH_RFEND_CFG50_SENS_9      0x00009000UL    /* RE §6.2 */

/* RFEND +0x58 bit16 clear in RXAdc (RE §6.2) */
#define WCH_RFEND_CFG58         (WCH_REG32(WCH_RFEND_BASE, 0x58))
#define WCH_RFEND_CFG58_RXADC       0x00010000UL

/* RFEND +0x5C: SetSensitivity clears bits 28/27/19 (RE §6.2) */
#define WCH_RFEND_CFG5C         (WCH_REG32(WCH_RFEND_BASE, 0x5C))
#define WCH_RFEND_CFG5C_SENS_CLR    0x18080000UL

/* RFEND +0x90 bits[5:0] = VCO CO reading; +0x94 bits[16:10] = GA reading */
#define WCH_RFEND_TUNE_CO       (WCH_REG32(WCH_RFEND_BASE, 0x90))
#define WCH_RFEND_TUNE_CO_MASK      0x0000003FUL
#define WCH_RFEND_TUNE_GA       (WCH_REG32(WCH_RFEND_BASE, 0x94))
#define WCH_RFEND_TUNE_GA_SHIFT     10
#define WCH_RFEND_TUNE_GA_MASK      0x0000007FUL

/* RFEND +0x9C: bit8 = RX filter measure ready, bits[4:0] = value (RE §6.2) */
#define WCH_RFEND_FILTMEAS      (WCH_REG32(WCH_RFEND_BASE, 0x9C))
#define WCH_RFEND_FILTMEAS_RDY      0x00000100UL
#define WCH_RFEND_FILTMEAS_MASK     0x0000001FUL

/*
 * RFEND +0xA0.. = 39-channel x 4-bit VCO capacitor lookup table written by
 * TXCtune interpolation (RE §6.1). 39 nibbles = 5 words (0xA0..0xB0),
 * nibble k of word j = channel 8*j + k (LSB nibble = lower channel).
 * TODO(board): confirm nibble order against a lib-calibrated dump.
 */
#define WCH_RFEND_VCOTAB(j)     (WCH_REG32(WCH_RFEND_BASE, 0xA0 + (j) * 4))
#define WCH_RFEND_VCOTAB_WORDS      5

/* ------------------------------------------------------------------------- */
/* Guard word (RE §3 / §4.3 / §6.3: save [0x4002420C], clear, fence.i, ...)  */
/* ------------------------------------------------------------------------- */

void wch_rf_guard_enter(void);
void wch_rf_guard_exit(void);

/* ------------------------------------------------------------------------- */
/* Timing parameters (single place to tweak, all board-calibratable)         */
/* ------------------------------------------------------------------------- */

struct wch_rf_timing {
    uint16_t tx_guard_us;           /* busy-wait margin before sched start   */
    uint16_t tx_late_guard_us;      /* "late" threshold after sched start    */
    uint32_t wfr_half_us_1m;        /* default RX window if LL gives none    */
    uint32_t wfr_half_us_coded;     /* ditto, coded PHY (sync=376us+margin)  */
    uint32_t tifs_turnaround_half_us; /* TX->RX turnaround (LLE[0x1C] base)  */
    uint32_t ack_window_half_us_1m; /* post-TX RX window, RE §4.8: 0x196     */
    uint32_t ack_window_half_us_2m; /* post-TX RX window, RE §12.1: 0x196    */
    uint32_t ack_window_half_us_coded; /* post-TX RX sync allowance: IFS +   */
                                       /* this == vendor 0x43E total (543us) */
    uint32_t rx_window_widen_half_us; /* C_PHY allowance (1M 73, RE §12.1)   */
    uint32_t rx_window_widen_half_us_coded; /* C_PHY coded = 0x189 (RE §12.1)*/
    uint32_t tune_timeout_us;       /* VCO tune / filter measure poll budget */
    uint16_t lo_inject_khz_1m;      /* low-side LO injection 1M: 1000 kHz    */
    uint16_t lo_inject_khz_2m;      /* low-side LO injection 2M: 2000 kHz    */
};

extern struct wch_rf_timing g_wch_rf_timing;

/* ------------------------------------------------------------------------- */
/* Events / callback                                                         */
/* ------------------------------------------------------------------------- */

#define WCH_RF_RX_BUFFER_SIZE 272U

enum wch_rf_event {
    WCH_RF_EVT_TX_DONE = 0,     /* payload = frame end cputime */
    WCH_RF_EVT_RX_START,        /* payload = address-match cputime */
    WCH_RF_EVT_RX_DONE,         /* payload = frame end cputime */
    WCH_RF_EVT_RX_TIMEOUT,      /* payload = 0 */
    WCH_RF_EVT_HW_ERR           /* payload = raw status word */
};

typedef void (*wch_rf_cb_t)(enum wch_rf_event evt, uint32_t payload);
void wch_rf_set_callback(wch_rf_cb_t cb);
bool wch_rf_rx_crc_ok(void);

/* ------------------------------------------------------------------------- */
/* API                                                                       */
/* ------------------------------------------------------------------------- */

/*
 * Full init sequence RE §3 + §6:
 *   HSE load trim (3 lines from WCHBLE_Init, EXAM/BLE/HAL/MCU.c) ->
 *   LLE_DevInit -> RFEND_DevInit -> BB_DevInit ->
 *   TXCtune(3 points + 39ch interpolation) -> TXFtune -> RXFilter -> RXAdc ->
 *   NVIC: BB=1, LLE=1.
 * Also performs the one-shot entropy/RNG seeding hook used by ble_hw_wch.c.
 */
int  wch_rf_init(void);

/* Periodic calibration, RE §6.3 (call from an OS timer every ~120 s) */
void wch_rf_recalibrate(void);

/*
 * Set BLE channel, RE §4.2.  ble_chan: 0..36 data (skip advertising 2426 MHz),
 * 37/38/39 advertising (2402/2426/2480 MHz).  Frequency mode (RFEND[0x2C]|=2)
 * with low-side LO injection (-1 MHz @1M, -2 MHz @2M).
 */
void wch_rf_set_channel(uint8_t ble_chan);

/* Currently programmed BLE channel (0xFF = none) */
uint8_t wch_rf_channel_get(void);

/*
 * Basic transmit, RE §4.6 (no auto-ack).  Frame layout
 *   [0] = PDU header byte, [1] = payload length, [2..] = payload
 * (this is exactly the lib's [pktTxType][txLen][data] frame).  Blocks until
 * actual basic TX-done status (bit1) or a bounded deadline.
 * start_cputime: optional absolute TIM3 deadline for the TX command. All
 * preparation precedes the wait; the hardware window is loaded afterwards.
 * Returns 0 on success, -1 on timeout / busy, -2 if more than 10 us late.
 */
int  wch_rf_tx_basic(const uint8_t *frame, uint16_t n, uint32_t timeout_half_us,
                     const uint32_t *start_cputime);

/*
 * Basic receive, RE §4.7 (no auto-ack).  Non-blocking: arms the engine and
 * returns; completion / timeout is reported through the callback (RX_DONE /
 * RX_TIMEOUT).  window_half_us: RX window width in 0.5 us units (LLE[0x64]).
 * Returns 0 on success, -1 if engine busy.
 */
int  wch_rf_rx_basic(uint8_t *rxbuf, uint16_t maxlen, uint32_t window_half_us);

/*
 * Auto-ack arm, RE §4.7/§4.8 auto mode: pre-load the response frame and set
 * BB[0x2C] RX-arm bit.  v1: interface kept, not exercised by NimBLE glue.
 */
int  wch_rf_arm_auto_ack(const uint8_t *rsp, uint16_t n);

/* Shutdown sequence, RE §4.3 ble_ll_hw_api_shut */
void wch_rf_shut(void);

/* RSSI of the last frame, RE §4.9: (int8_t)(BB[0x30] >> 15) */
int8_t wch_rf_rssi(void);

/* CFO of the last frame (Hz-approx, signed 11-bit @15.625 kHz/LSB), RE §4.9 */
int16_t wch_rf_cfo(void);

/* Hardware AES-128 single block ECB, RE §9.7.  Returns 0 on success. */
int  wch_aes_ecb(const uint8_t key[16], const uint8_t in[16], uint8_t out[16]);

/* Interrupt service routines - called from app/ch32v20x_it.c */
void wch_rf_lle_isr(void);
void wch_rf_bb_isr(void);

/* Anchor timestamp (os_cputime @1MHz) captured on BB clock-capture event */
uint32_t wch_rf_last_anchor(void);

/* True while the BB clock capture fired for the frame in progress
 * (wch_rf_last_anchor() then holds the capture cputime). */
bool wch_rf_rx_captured(void);

/* Auto RX-arm after TX (T_IFS continuation). Set by ble_phy_wch.c only. */
void wch_rf_auto_arm_rx(bool en);
bool wch_rf_auto_arm_rx_get(void);

/*
 * PHY mode in use for TX/RX.  Values are the NimBLE controller modes
 * (ble_phy.h): 0 = coded S=2 (500 kbps), 1 = 1M, 2 = 2M, 3 = coded S=8
 * (125 kbps).  Both coded modes are supported.
 */
void wch_rf_phy_mode_set(uint8_t tx_mode, uint8_t rx_mode);
uint8_t wch_rf_phy_mode_tx_get(void);
uint8_t wch_rf_phy_mode_rx_get(void);

/* Whitening control (NimBLE always enables whitening -> bit6 = 0) */
void wch_rf_whiten_set(bool disable);

/* Encryption state (drives LLE[0x50] mode word bit5, RE §12.1/§12.5) */
void wch_rf_encrypt_set(bool en);
bool wch_rf_encrypt_get(void);

/* RX frame buffer just received (PDU header first byte), for ble_hw */
uint8_t *wch_rf_rx_frame(void);

/* DTM flag - selects demod cfg14 variant (RE §4.4: 0x34A4DF) */
void wch_rf_dtm_set(bool en);

/* Last RX length byte (drives the LLE[0x1C] turnaround rule, RE §5.3) */
void wch_rf_last_rx_len_set(uint8_t len);

/* Access address / CRC init programmed into BB[0x08]/BB[0x04] at TX/RX arm
 * (RE §4.6/§4.7; advertising constants 0x8E89BED6/0x555555, RE §12.6) */
void wch_rf_aa_crc_set(uint32_t access_addr, uint32_t crcinit);

/* Platform bring-up entry: wch_rf_init() + ble_phy_init() + RNG seed.
 * Implemented in ble_hw_wch.c; called by port/nimble_port_wch.c. */
void wch_ble_hw_init(void);

#ifdef __cplusplus
}
#endif

#endif /* H_WCH_RF_ */
