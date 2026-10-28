/*
 * ble_hw_wch.c - Apache NimBLE controller ble_hw.h adapter for CH32V208.
 *
 * - Whitelist: pure software, 8 entries (the WCH modem has no LL whitelist
 *   match primitive exposed by RE - the lib filters in software too,
 *   RE §12.6 ll_advertise_legacy_rx).
 * - Encrypt: CH32V208 hardware AES-128 single block (RE §9.7).
 * - RNG: software xorshift128+ seeded from ADC temperature sensor + flash
 *   UID + cputime.  WARNING below.
 * - Resolv list: empty (LL privacy disabled in this build).
 * - Public address: SDK FLASH_GetMACAddress (D8W only).
 */

#include <stdint.h>
#include <string.h>

#include "syscfg/syscfg.h"
#include "os/os_cputime.h"
#include "nimble/ble.h"
#include "nimble/nimble_opt.h"
#include "controller/ble_hw.h"
#include "controller/ble_phy.h"
#include "controller/ble_ll.h"      /* BLE_LL_PDU_HDR_LEN */

#include "ch32v20x.h"           /* ADC1, RCC               */
#include "ch32v20x_rcc.h"
#include "ch32v20x_adc.h"
#include "ch32v20x_flash.h"     /* FLASH_GetMACAddress     */
#include "core_riscv.h"

#include "wch_rf.h"

/* ------------------------------------------------------------------------- */
/* Software whitelist - 8 entries                                            */
/* ------------------------------------------------------------------------- */

#define BLE_WCH_WHITE_LIST_SIZE     (8)

struct wch_whitelist_entry
{
    uint8_t addr[6];
    uint8_t addr_type;
    uint8_t valid;
};

static struct wch_whitelist_entry g_wch_whitelist[BLE_WCH_WHITE_LIST_SIZE];
static uint8_t g_wch_whitelist_enabled;

uint8_t
ble_hw_whitelist_size(void)
{
    return BLE_WCH_WHITE_LIST_SIZE;
}

void
ble_hw_whitelist_clear(void)
{
    memset(g_wch_whitelist, 0, sizeof(g_wch_whitelist));
}

int
ble_hw_whitelist_add(const uint8_t *addr, uint8_t addr_type)
{
    int i;

    if (addr == NULL) {
        return BLE_ERR_INV_HCI_CMD_PARMS;
    }
    for (i = 0; i < BLE_WCH_WHITE_LIST_SIZE; i++) {
        if (g_wch_whitelist[i].valid &&
            (g_wch_whitelist[i].addr_type == addr_type) &&
            (memcmp(g_wch_whitelist[i].addr, addr, 6) == 0)) {
            return BLE_ERR_SUCCESS;         /* already present */
        }
    }
    for (i = 0; i < BLE_WCH_WHITE_LIST_SIZE; i++) {
        if (!g_wch_whitelist[i].valid) {
            memcpy(g_wch_whitelist[i].addr, addr, 6);
            g_wch_whitelist[i].addr_type = addr_type;
            g_wch_whitelist[i].valid = 1;
            return BLE_ERR_SUCCESS;
        }
    }
    return BLE_ERR_MEM_CAPACITY;
}

void
ble_hw_whitelist_rmv(const uint8_t *addr, uint8_t addr_type)
{
    int i;

    if (addr == NULL) {
        return;
    }
    for (i = 0; i < BLE_WCH_WHITE_LIST_SIZE; i++) {
        if (g_wch_whitelist[i].valid &&
            (g_wch_whitelist[i].addr_type == addr_type) &&
            (memcmp(g_wch_whitelist[i].addr, addr, 6) == 0)) {
            g_wch_whitelist[i].valid = 0;
            return;
        }
    }
}

void
ble_hw_whitelist_enable(void)
{
    g_wch_whitelist_enabled = 1;
}

void
ble_hw_whitelist_disable(void)
{
    g_wch_whitelist_enabled = 0;
}

/*
 * Called by the LL with the just-received advertising PDU available in the
 * PHY RX frame buffer ([hdr][len][payload]).  PDU layout per Core spec:
 * header 2 bytes, then AdvA (offset 2..7) and, for directed PDUs, TargetA
 * (offset 8..13).  v1 limitation: only legacy advertising PDU layouts are
 * recognized.
 */
int
ble_hw_whitelist_match(void)
{
    const uint8_t *pdu;
    int i;

    if (!g_wch_whitelist_enabled) {
        return 0;
    }
    pdu = wch_rf_rx_frame();
    if ((pdu == NULL) || (pdu[1] < 6)) {
        return 0;
    }

    for (i = 0; i < BLE_WCH_WHITE_LIST_SIZE; i++) {
        if (!g_wch_whitelist[i].valid) {
            continue;
        }
        if ((g_wch_whitelist[i].addr_type & 1) != ((pdu[0] >> 6) & 1)) {
            /* TxAdd bit (PDU header bit6) vs entry address type */
            continue;
        }
        if (memcmp(g_wch_whitelist[i].addr, pdu + BLE_LL_PDU_HDR_LEN, 6) == 0) {
            return 1;                   /* AdvA / InitA match */
        }
        if ((pdu[1] >= 12) &&
            (memcmp(g_wch_whitelist[i].addr, pdu + BLE_LL_PDU_HDR_LEN + 6,
                    6) == 0)) {
            return 1;                   /* TargetA match (direct adv) */
        }
    }
    return 0;
}

/* ------------------------------------------------------------------------- */
/* Hardware AES-128 single block (RE §9.7)                                   */
/* ------------------------------------------------------------------------- */

int
ble_hw_encrypt_block(struct ble_encryption_block *ecb)
{
    if (ecb == NULL) {
        return -1;
    }
    /* SM callers byte-reverse into ecb before calling (RE §13.1 sm_c1new);
     * the AES core consumes bytes in memory order, LSB-first per word. */
    return wch_aes_ecb(ecb->key, ecb->plain_text, ecb->cipher_text);
}

/* ------------------------------------------------------------------------- */
/* RNG - xorshift128+ seeded from board entropy                              */
/* ------------------------------------------------------------------------- */

/*
 * !!! WARNING !!!  WARNING !!!  WARNING !!!  WARNING !!!  WARNING !!!  !!!!
 * !!!                                                                  !!!!
 * !!!  THE V208 HAS NO TRUE RANDOM NUMBER GENERATOR.  THIS IS A SOFTWARE  !!!
 * !!!  XORSHIFT128+ SEEDED FROM *WEAK* BOARD ENTROPY (ADC TEMP SENSOR +   !!!
 * !!!  FLASH UID + CPUTIME).  IT IS FINE FOR BRING-UP / DTM / LOGGING,    !!!
 * !!!  BUT PRODUCT PAIRING (SM LTK / IRK / CSRK GENERATION) REQUIRES A    !!!
 * !!!  REAL ENTROPY SOURCE.  REPLACE BEFORE ANY SECURE USE.               !!!
 * !!!                                                                  !!!
 * !!! WARNING !!!  WARNING !!!  WARNING !!!  WARNING !!!  WARNING !!!  !!!!
 */
static uint64_t g_rng_state[2];
static uint8_t g_rng_seeded;
static ble_rng_isr_cb_t g_rng_cb;
static int g_rng_started;

/* One ADC sample of the internal temperature sensor (simplified
 * HAL_GetInterTempValue sequence, EXAM/BLE/HAL). */
static uint16_t
adc_temp_sample(void)
{
    ADC_InitTypeDef init;
    uint32_t guard = 200000;
    uint16_t v = 0;

    RCC_APB2PeriphClockCmd(RCC_APB2Periph_ADC1, ENABLE);
    ADC_TempSensorVrefintCmd(ENABLE);
    ADC_StructInit(&init);
    init.ADC_ScanConvMode = DISABLE;
    init.ADC_ContinuousConvMode = DISABLE;
    init.ADC_ExternalTrigConv = ADC_ExternalTrigConv_None;
    ADC_Init(ADC1, &init);
    ADC_RegularChannelConfig(ADC1, ADC_Channel_TempSensor, 1,
                             ADC_SampleTime_239Cycles5);
    ADC_Cmd(ADC1, ENABLE);
    ADC_SoftwareStartConvCmd(ADC1, ENABLE);
    while ((ADC_GetFlagStatus(ADC1, ADC_FLAG_EOC) == RESET) && (--guard)) {
    }
    if (guard) {
        v = ADC_GetConversionValue(ADC1);
    }
    ADC_Cmd(ADC1, DISABLE);
    ADC_TempSensorVrefintCmd(DISABLE);
    return v;
}

static void
ble_hw_rng_seed(void)
{
    uint16_t temp = adc_temp_sample();
    uint8_t mac[6];
    uint32_t uid0;
    uint32_t uid1;

    /* Flash unique UID - same words FLASH_GetMACAddress is built on
     * (ch32v20x_flash.c: 0x1FFFF7E8 / 0x1FFFF7EC). */
    uid0 = *(volatile uint32_t *)0x1FFFF7E8UL;
    uid1 = *(volatile uint32_t *)0x1FFFF7ECUL;
    FLASH_GetMACAddress(mac);

    g_rng_state[0] = ((uint64_t)uid0 << 32) ^ ((uint64_t)uid1) ^
                     ((uint64_t)temp << 16) ^
                     ((uint64_t)mac[0] | ((uint64_t)mac[1] << 8) |
                      ((uint64_t)mac[2] << 16) | ((uint64_t)mac[3] << 24));
    g_rng_state[1] = ((uint64_t)os_cputime_get32() << 32) ^
                     ((uint64_t)mac[4] | ((uint64_t)mac[5] << 8)) ^
                     ((uint64_t)temp << 48);

    /* xorshift128+ must leave the state non-zero. */
    if ((g_rng_state[0] == 0) && (g_rng_state[1] == 0)) {
        g_rng_state[0] = 0x9E3779B97F4A7C15ULL;
    }
    /* Discard a few outputs to wash out seed correlations. */
    (void)ble_hw_rng_read();
    (void)ble_hw_rng_read();
    (void)ble_hw_rng_read();
    (void)ble_hw_rng_read();

    g_rng_seeded = 1;
}

static uint64_t
rng_next_u64(void)
{
    uint64_t s1 = g_rng_state[0];
    const uint64_t s0 = g_rng_state[1];

    g_rng_state[0] = s0;
    s1 ^= s1 << 23;
    g_rng_state[1] = s1 ^ s0 ^ (s1 >> 18) ^ (s0 >> 5);
    return g_rng_state[1] + s0;
}

int
ble_hw_rng_init(ble_rng_isr_cb_t cb, int bias)
{
    (void)bias;     /* no HW threshold to tune */

    g_rng_cb = cb;
    if (!g_rng_seeded) {
        ble_hw_rng_seed();
    }
    return 0;
}

int
ble_hw_rng_start(void)
{
    /* Native-driver contract: block in the caller (LL task) and push bytes
     * into the LL entropy pool through the ISR callback. */
    g_rng_started = 1;
    if (g_rng_cb) {
        while (g_rng_started) {
            uint64_t v = rng_next_u64();
            g_rng_cb((uint8_t)v);
            g_rng_cb((uint8_t)(v >> 8));
            g_rng_cb((uint8_t)(v >> 16));
            g_rng_cb((uint8_t)(v >> 24));
            g_rng_cb((uint8_t)(v >> 32));
            g_rng_cb((uint8_t)(v >> 40));
            g_rng_cb((uint8_t)(v >> 48));
            g_rng_cb((uint8_t)(v >> 56));
        }
    }
    return 0;
}

int
ble_hw_rng_stop(void)
{
    g_rng_started = 0;
    return 0;
}

uint8_t
ble_hw_rng_read(void)
{
    if (!g_rng_seeded) {
        ble_hw_rng_seed();
    }
    return (uint8_t)rng_next_u64();
}

/* ------------------------------------------------------------------------- */
/* Resolving list - LL privacy disabled in this build                        */
/* ------------------------------------------------------------------------- */

#if MYNEWT_VAL(BLE_LL_CFG_FEAT_LL_PRIVACY)
void
ble_hw_resolv_list_clear(void)
{
    /* Privacy (RPA resolution) disabled for this port. */
}

int
ble_hw_resolv_list_add(uint8_t *irk)
{
    (void)irk;
    return BLE_ERR_MEM_CAPACITY;
}

void
ble_hw_resolv_list_rmv(int index)
{
    (void)index;
}

uint8_t
ble_hw_resolv_list_size(void)
{
    return 0;
}

int
ble_hw_resolv_list_match(void)
{
    return -1;
}
#endif

/* ------------------------------------------------------------------------- */
/* Device addresses                                                          */
/* ------------------------------------------------------------------------- */

/*
 * Byte order decision:
 *   FLASH_GetMACAddress() fills Buffer[0] with the LSB of the UID word at
 *   0x1FFFF7E8 (ch32v20x_flash.c).  The EVT examples pass this buffer
 *   straight into bleConfig_t.MacAddr which wchble.h documents as
 *   little-endian / on-air order (RE §7 field 0x18).  NimBLE ble_addr_t.val
 *   is HCI order = LSB byte first = on-air order.  Therefore val[i] =
 *   Buffer[i], a straight copy.
 */
int
ble_hw_get_public_addr(ble_addr_t *addr)
{
    uint8_t mac[6];

    if (addr == NULL) {
        return -1;
    }
    FLASH_GetMACAddress(mac);
    memcpy(addr->val, mac, 6);
    addr->type = BLE_ADDR_PUBLIC;
    return 0;
}

int
ble_hw_get_static_addr(ble_addr_t *addr)
{
    (void)addr;
    return -1;      /* no factory random static address on V208 */
}

/* ------------------------------------------------------------------------- */
/* Platform bring-up entry (called by port/nimble_port_wch.c)                */
/* ------------------------------------------------------------------------- */

void
wch_ble_hw_init(void)
{
    /* wch_rf_init(): RE §3 + §6 init + calibration + NVIC BB/LLE prio 1 */
    (void)wch_rf_init();
    /* NimBLE PHY layer state + ISR callback wiring */
    (void)ble_phy_init();
    /* Seed the (weak) software RNG once at boot */
    ble_hw_rng_seed();
}
