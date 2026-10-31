#!/usr/bin/env python3
"""Compile the real PHY register recipes with mocked MMIO and timeout hooks.

Checks vendor-derived register/window vectors and RX abort recovery. This does
not simulate RF hardware, interrupts, or on-air timing; those need board tests.
"""
import os
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def function(source, name):
    match = re.search(r'\n' + name + r'\([^)]*\)\n\{', source)
    if not match:
        raise RuntimeError(f'cannot locate production function {name}')
    start = match.start() + 1
    depth = 1
    end = match.end()
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    # All selected functions have a single-line return type.
    return source[source.rfind('\n', 0, start - 1) + 1:end]


def main():
    rf = (ROOT / 'drivers/wch_ble/wch_rf.c').read_text()
    phy = (ROOT / 'drivers/wch_ble/ble_phy_wch.c').read_text()
    recipes = '\n'.join(function(rf, name) for name in (
        'bb_phy_bits_tx', 'bb_phy_bits_rx', 'phy_is_coded',
        'set_phy_tx_mode', 'set_phy_rx_mode'))
    recipes += '\n' + function(phy, 'tx_window_half_us')
    recipes += '\n' + function(phy, 'phy_rx_timeout_isr')
    recipes += '\n' + function((ROOT / 'port/ble_ll_coded.c').read_text(),
                                '__wrap_ble_ll_conn_hci_le_set_phy')
    harness = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include "wch_rf.h"
static uint32_t bb[64], lle[64];
#undef WCH_REG32
#define WCH_REG32(base, off) (*((base) == WCH_BB_BASE ? &bb[(off)/4] : &lle[(off)/4]))
static bool s_dtm;
void os_cputime_delay_usecs(uint32_t usecs) { (void)usecs; }
#define BLE_PHY_STATE_RX 2
static struct { unsigned phy_state, phy_rx_started; } g_ble_phy_data;
static struct { unsigned rx_aborts, rx_timeouts, rx_partial_timeouts, hw_errs; } g_ble_phy_stats;
static unsigned disables, wfr, hw_errors;
void ble_phy_disable(void) {
    disables++;
    g_ble_phy_data.phy_state = 0;
    g_ble_phy_data.phy_rx_started = 0;
}
void ble_ll_wfr_timer_exp(void *arg) {
    (void)arg;
    assert(g_ble_phy_data.phy_rx_started == 0);
    wfr++;
}
void ble_ll_hw_error(void) { hw_errors++; }
#define MYNEWT_VAL(x) 1
#define BLE_ERR_SUCCESS 0
#define BLE_PHY_CODED 3
#define le16toh(x) (x)
typedef unsigned os_sr_t;
static unsigned irq_locked;
#define OS_ENTER_CRITICAL(sr) ((sr) = 1, irq_locked++)
#define OS_EXIT_CRITICAL(sr) ((void)(sr), irq_locked--)
struct ble_hci_le_set_phy_cp { uint16_t conn_handle; };
struct ble_ll_conn_sm { struct {
    uint8_t cur_tx_phy, tx_phy_mode, pref_opts, rx_phy_mode;
} phy_data; };
static struct ble_ll_conn_sm connection;
static int set_phy_rc;
int __real_ble_ll_conn_hci_le_set_phy(const uint8_t *buf, uint8_t len) {
    (void)buf; (void)len;
    return set_phy_rc;
}
struct ble_ll_conn_sm *ble_ll_conn_find_by_handle(uint16_t handle) {
    assert(irq_locked);
    return handle == 1 ? &connection : NULL;
}
int ble_ll_phy_to_phy_mode(int phy, int options) {
    return phy == 3 && options == 1 ? 0 : phy;
}
/* Independent Core packet-layout reference, indexed by NimBLE mode. */
uint32_t ble_ll_pdu_us(uint8_t len, uint8_t mode) {
    const unsigned base[] = {462, 80, 44, 720};
    const unsigned octet[] = {16, 8, 4, 64};
    return base[mode] + len * octet[mode];
}
''' + recipes + r'''
int main(void) {
    const unsigned bits[] = {0x6000, 0x1000, 0, 0x2000};
    const unsigned lengths[] = {0, 1, 27, 64, 251, 253, 254, 255};
    for (unsigned previous = 0; previous < 4; previous++) {
        for (unsigned mode = 0; mode < 4; mode++) {
            for (unsigned i = 0; i < sizeof(lengths)/sizeof(lengths[0]); i++) {
                unsigned len = lengths[i];
                bb[0] = 0x01800125 | bits[previous] | 0x8000;
                lle[3] = 0xf00f;
                set_phy_tx_mode(mode, len);
                assert((bb[0] & 0x3000) == (bits[mode] & 0x3000));
                if (mode == 0 || mode == 3) {
                    assert((bb[0] & 0xf000) == bits[mode]);
                }
                assert((bb[0] & 0x01800125) == 0x01800125);
                assert(lle[3] == 0xf00f);
                unsigned air = ble_ll_pdu_us(len, mode);
                unsigned guard = (mode == 0 || mode == 3) ? 190 : 158;
                assert(lle[25] == 2 * (air + guard));
                assert(tx_window_half_us(mode, len) == 2 * air + 100);
            }
            for (unsigned dtm = 0; dtm < 2; dtm++) {
                s_dtm = dtm;
                set_phy_rx_mode(mode);
                if (mode == 0 || mode == 3) {
                    assert((bb[0] & 0x3000) == 0x2000);
                    assert(bb[4] == (dtm ? 0x3fa4df : 0x34a4df));
                    assert(bb[5] == 0x8301ff1 && bb[6] == 0x31619);
                    assert(bb[8] == 0x90086 && bb[9] == 0x1006310 && bb[10] == 0x28de);
                } else {
                    assert(bb[4] == (dtm ? 0x3fa2ce : 0x3222d0));
                    assert(bb[5] == 0x8101901 && bb[6] == 0x31624);
                }
            }
        }
    }
    for (unsigned started = 0; started < 2; started++) {
        g_ble_phy_data.phy_state = BLE_PHY_STATE_RX;
        g_ble_phy_data.phy_rx_started = started;
        phy_rx_timeout_isr();
        assert(disables == started + 1 && wfr == started + 1);
        assert(g_ble_phy_stats.rx_timeouts == started + 1);
        assert(g_ble_phy_stats.rx_aborts == started);
        assert(g_ble_phy_stats.rx_partial_timeouts == started);
        assert(hw_errors == 0);
    }
    phy_rx_timeout_isr(); /* stale timeout after disabling is ignored */
    assert(disables == 2 && wfr == 2 && hw_errors == 0);
    struct ble_hci_le_set_phy_cp cmd = {1};
    connection.phy_data.cur_tx_phy = 3;
    connection.phy_data.rx_phy_mode = 1; /* never change peer RX coding */
    for (unsigned i = 0; i < 6; i++) {
        connection.phy_data.pref_opts = i % 2 + 1;
        assert(__wrap_ble_ll_conn_hci_le_set_phy((const void *)&cmd, sizeof(cmd)) == 0);
        assert(connection.phy_data.tx_phy_mode == (i % 2 ? 3 : 0));
        assert(connection.phy_data.rx_phy_mode == 1 && irq_locked == 0);
    }
    set_phy_rc = 0x12;
    connection.phy_data.pref_opts = 1;
    assert(__wrap_ble_ll_conn_hci_le_set_phy((const void *)&cmd, sizeof(cmd)) == 0x12);
    assert(connection.phy_data.tx_phy_mode == 3 && irq_locked == 0);
    set_phy_rc = 0;
    connection.phy_data.cur_tx_phy = 1;
    connection.phy_data.tx_phy_mode = 1;
    __wrap_ble_ll_conn_hci_le_set_phy((const void *)&cmd, sizeof(cmd));
    assert(connection.phy_data.tx_phy_mode == 1 && irq_locked == 0);
    puts("PASS: PHY transitions, RX recipes, 0..255-byte bounds, RX aborts, coded-only changes");
    return 0;
}
'''
    with tempfile.TemporaryDirectory(prefix='wch-phy-check-') as temp:
        source = Path(temp) / 'phy.c'
        binary = Path(temp) / 'phy-check'
        source.write_text(harness)
        subprocess.run([os.environ.get('CC', 'cc'), '-std=c11', '-Wall', '-Wextra',
                        '-Werror', '-I', str(ROOT / 'drivers/wch_ble'), str(source),
                        '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == '__main__':
    main()
