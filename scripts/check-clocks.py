#!/usr/bin/env python3
"""Check production clock/timestamp/deadline code with simulated MMIO/time.

This checks arithmetic and scheduling boundaries, not on-air timing. Run the
three HSE configurations on the board as well (see docs/clock-rates.md).
"""
import os
from pathlib import Path
import runpy
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
function = runpy.run_path(str(ROOT / 'scripts/check-phy.py'))['function']


def main():
    timer = (ROOT / 'port/hal_timer_wch.c').read_text()
    phy = (ROOT / 'drivers/wch_ble/ble_phy_wch.c').read_text()
    rf = (ROOT / 'drivers/wch_ble/wch_rf.c').read_text()
    hw = (ROOT / 'drivers/wch_ble/ble_hw_wch.c').read_text()
    harness = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "wch_rf.h"
static uint32_t bb[64], lle[64], rf[64];
#undef WCH_REG32
#define WCH_REG32(base, off) (*((base) == WCH_BB_BASE ? &bb[(off)/4] : \
    (base) == WCH_LLE_BASE ? &lle[(off)/4] : &rf[(off)/4]))
typedef struct { uint32_t PCLK1_Frequency, PCLK2_Frequency; } RCC_ClocksTypeDef;
static struct { uint32_t CFGR0; } rcc;
#define RCC (&rcc)
#define RCC_PPRE1 0x700U
#define RCC_PPRE1_DIV1 0U
static uint32_t pclk1, pclk2;
void RCC_GetClocksFreq(RCC_ClocksTypeDef *clocks) {
    clocks->PCLK1_Frequency = pclk1;
    clocks->PCLK2_Frequency = pclk2;
}
static uint32_t now, prep_us, tx_tick, capture;
static unsigned guard, observed_tx;
static bool crc_ok, captured;
static bool timeout_race, drop_tx_done;
static uint8_t s_phy_tx = 1, s_channel = 37;
static uint32_t s_aa, s_crcinit;
volatile uint32_t g_wch_rf_tx_elapsed_us[4], g_wch_rf_tx_start_cputime;
volatile uint8_t g_wch_rf_tx_payload_len[4];
volatile uint32_t g_wch_rf_tx_rfctrl[4];
volatile uint32_t g_wch_phy_capture_latency_us, g_wch_phy_capture_samples;
static uint32_t s_capture_latency_q3;
#define BLE_PHY_MODE_1M 1
#define BLE_PHY_MODE_CODED_500KBPS 0
#define BLE_PHY_MODE_CODED_125KBPS 3
uint32_t ble_ll_pdu_us(uint8_t len, uint8_t mode) {
    const unsigned base[] = {462, 80, 44, 720}, octet[] = {16, 8, 4, 64};
    return base[mode] + len * octet[mode];
}
uint32_t ble_ll_pdu_syncword_us(uint8_t mode) { return mode == 1 ? 40 : 376; }
bool wch_rf_rx_captured(void) { return captured; }
bool wch_rf_rx_crc_ok(void) { return crc_ok; }
uint32_t wch_rf_last_anchor(void) { return capture; }
void wch_rf_guard_enter(void) { assert(!guard); guard = 1; }
void wch_rf_guard_exit(void) { assert(guard); guard = 0; }
static void engine_stop(void) { WCH_LLE_CMD = 0; }
static uint16_t lo_inject_for_phy(uint8_t mode) { (void)mode; return 1000; }
static void dev_set_channel(uint8_t ch, uint16_t lo) { (void)ch; (void)lo; }
static void set_phy_tx_mode(uint8_t mode, uint8_t len) {
    WCH_LLE_WINDOW = 2 * (ble_ll_pdu_us(len, mode) + 158);
    now += prep_us;
}
uint32_t os_cputime_get32(void) {
    if (WCH_LLE_CMD == WCH_LLE_CMD_START_TX && !observed_tx) {
        /* First poll after command must see the complete TX window. */
        assert(WCH_LLE_WINDOW == 2 * (80 + 8 * 3 + 158));
        tx_tick = now;
        observed_tx = 1;
        if (!drop_tx_done) { WCH_LLE_IRQSTAT |= WCH_LLE_IRQ_TX_DONE; }
        if (timeout_race) { now += 2000; }
    }
    return now++;
}
typedef struct { unsigned ADC_ScanConvMode, ADC_ContinuousConvMode,
    ADC_ExternalTrigConv; } ADC_InitTypeDef;
#define RCC_APB2Periph_ADC1 1
#define RCC_PCLK2_Div2 0
#define RCC_PCLK2_Div4 0x4000
#define RCC_PCLK2_Div6 0x8000
#define RCC_PCLK2_Div8 0xc000
#define ENABLE 1
#define DISABLE 0
#define RESET 0
#define ADC1 1
#define ADC_ExternalTrigConv_None 0
#define ADC_Channel_TempSensor 16
#define ADC_SampleTime_239Cycles5 239
#define ADC_FLAG_EOC 1
static unsigned adc_div, adc_enabled, sensor_enabled, adc_done = 1;
void RCC_APB2PeriphClockCmd(unsigned a, unsigned b) { (void)a; (void)b; }
void RCC_ADCCLKConfig(unsigned div) { adc_div = div; }
void ADC_TempSensorVrefintCmd(unsigned en) { sensor_enabled = en; }
void ADC_StructInit(ADC_InitTypeDef *init) { (void)init; }
void ADC_Init(unsigned adc, ADC_InitTypeDef *init) { (void)adc; (void)init; }
void ADC_RegularChannelConfig(unsigned a, unsigned b, unsigned c, unsigned d) {
    (void)a; (void)b; (void)c; (void)d;
}
void ADC_Cmd(unsigned adc, unsigned en) { (void)adc; adc_enabled = en; }
void ADC_SoftwareStartConvCmd(unsigned adc, unsigned en) { (void)adc; (void)en; }
unsigned ADC_GetFlagStatus(unsigned adc, unsigned flag) { (void)adc; (void)flag; return adc_done; }
uint16_t ADC_GetConversionValue(unsigned adc) { (void)adc; return 1234; }
void os_cputime_delay_usecs(uint32_t us) { now += us; }
''' + function(timer, 'wch_timer_input_freq') + '\n' + \
        function(phy, 'rx_beg_cputime') + '\n' + \
        function(hw, 'adc_temp_sample') + '\n' + \
        function(rf, 'wch_rf_tx_basic') + r'''
int main(void) {
    const uint32_t hz[] = {72000000, 96000000, 144000000};
    for (unsigned h = 0; h < 3; h++) {
        /* All APB1 prescalers, including timer doubling when divided. */
        for (unsigned div = 0; div < 5; div++) {
            pclk1 = hz[h] >> div;
            rcc.CFGR0 = div ? (div + 3) << 8 : 0;
            assert(wch_timer_input_freq() == (div ? 2 * pclk1 : pclk1));
        }
    }
    const uint8_t frame[] = {0, 3, 0, 1, 2};
    /* Different preparation costs, crossing both 16-bit and 32-bit wrap. */
    const uint32_t starts[] = {65500, UINT32_MAX - 60};
    for (unsigned i = 0; i < 2; i++) {
        for (prep_us = 2; prep_us <= 30; prep_us += 7) {
            memset(lle, 0, sizeof lle);
            observed_tx = 0;
            now = starts[i];
            uint32_t target = now + 120;
            assert(wch_rf_tx_basic(frame, sizeof frame, 0, &target) == 0);
            assert(observed_tx && !guard);
            assert((uint32_t)(g_wch_rf_tx_start_cputime - target) <= 2);
            assert((uint32_t)(tx_tick - target) <= 3);
        }
    }
    memset(lle, 0, sizeof lle);
    observed_tx = 0;
    now = 1000;
    prep_us = 200;
    uint32_t target = 1120;
    assert(wch_rf_tx_basic(frame, sizeof frame, 0, &target) == -2);
    assert(!observed_tx && !guard && WCH_LLE_WINDOW == 0);

    /* TX_DONE arrives between the loop condition and the deadline check. */
    memset(lle, 0, sizeof lle);
    now = 1000;
    prep_us = 2;
    observed_tx = 0;
    timeout_race = true;
    assert(wch_rf_tx_basic(frame, sizeof frame, 0, NULL) == 0);
    assert(observed_tx && !guard);
    memset(lle, 0, sizeof lle);
    now = 1000;
    observed_tx = 0;
    drop_tx_done = true;
    assert(wch_rf_tx_basic(frame, sizeof frame, 0, NULL) == -1);
    assert(observed_tx && !guard);
    timeout_race = false;
    drop_tx_done = false;
    WCH_LLE_CMD = 0;

    const unsigned peripheral_hz[] = {72000000, 96000000, 72000000};
    const unsigned expected_div[] = {RCC_PCLK2_Div6, RCC_PCLK2_Div8, RCC_PCLK2_Div6};
    for (unsigned i = 0; i < 3; i++) {
        int ok = 0;
        pclk2 = peripheral_hz[i];
        assert(adc_temp_sample(&ok) == 1234 && ok);
        assert(adc_div == expected_div[i] && !adc_enabled && !sensor_enabled);
    }
    int ok = 1;
    pclk2 = 144000000; /* Missing APB2 division must not overclock ADC. */
    assert(adc_temp_sample(&ok) == 0 && !ok && !adc_enabled);
    adc_done = 0;
    pclk2 = 72000000;
    uint32_t before = now;
    assert(adc_temp_sample(&ok) == 0 && !ok && !adc_enabled && !sensor_enabled);
    assert(now - before >= 1020 && now - before <= 1025);

    /* Learn bias from known 1M airtime with several ISR latencies, then
     * reconstruct a coded start independently of S=2/S=8 payload duration. */
    for (uint32_t bias = 8; bias <= 32; bias += 8) {
        g_wch_phy_capture_samples = 0;
        s_capture_latency_q3 = 0;
        captured = crc_ok = true;
        capture = UINT32_MAX - 100;
        uint32_t rx_done = capture + (80 + 8 * 20 - 40) - bias;
        assert(rx_beg_cputime(rx_done, 20, 1) == rx_done - (80 + 8 * 20));
        assert(g_wch_phy_capture_latency_us == bias);
        for (unsigned mode = 0; mode <= 3; mode += 3) {
            capture = 123456 + 376 + bias;
            assert(rx_beg_cputime(999999, 37, mode) == 123456);
        }
        crc_ok = false;
        (void)rx_beg_cputime(rx_done, 20, 1);
        assert(g_wch_phy_capture_samples == 1);
    }
    puts("PASS: APB timer clocks, ADC clock/timeout, TX deadlines/wrap/late rejection, learned RX bias");
}
'''
    with tempfile.TemporaryDirectory(prefix='wch-clock-check-') as temp:
        source, binary = Path(temp) / 'clocks.c', Path(temp) / 'clocks'
        source.write_text(harness)
        subprocess.run([os.environ.get('CC', 'cc'), '-std=c11', '-Wall', '-Wextra',
                        '-Werror', '-Wno-pointer-to-int-cast', '-I',
                        str(ROOT / 'drivers/wch_ble'), str(source), '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == '__main__':
    main()
