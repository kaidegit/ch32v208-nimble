/*
 * Local NimBLE integration fix: a coding preference can change while the
 * HCI PHY stays LE Coded. The upstream Set PHY path updates pref_opts but
 * skips the PHY procedure when the current PHY already matches. In that
 * case tx_phy_mode was left unchanged forever, despite reporting success.
 *
 * Apply only the local transmitter's coding, under the same IRQ exclusion
 * used by the LL. The peer selects its own coding in each packet's CI.
 * --wrap keeps this fix in the port without modifying the sibling checkout.
 */
#include "os/os.h"
#include "controller/ble_ll_conn.h"

int __real_ble_ll_conn_hci_le_set_phy(const uint8_t *cmdbuf, uint8_t len);

int
__wrap_ble_ll_conn_hci_le_set_phy(const uint8_t *cmdbuf, uint8_t len)
{
    int rc = __real_ble_ll_conn_hci_le_set_phy(cmdbuf, len);

    if (rc != BLE_ERR_SUCCESS) {
        return rc;
    }
#if MYNEWT_VAL(BLE_LL_CFG_FEAT_LE_CODED_PHY)
    const struct ble_hci_le_set_phy_cp *cmd = (const void *)cmdbuf;
    struct ble_ll_conn_sm *connsm;
    os_sr_t sr;

    OS_ENTER_CRITICAL(sr);
    connsm = ble_ll_conn_find_by_handle(le16toh(cmd->conn_handle));
    if (connsm && connsm->phy_data.cur_tx_phy == BLE_PHY_CODED) {
        connsm->phy_data.tx_phy_mode = ble_ll_phy_to_phy_mode(
            BLE_PHY_CODED, connsm->phy_data.pref_opts);
    }
    OS_EXIT_CRITICAL(sr);
#endif
    return rc;
}
