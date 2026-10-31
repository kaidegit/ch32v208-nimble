/*
 * app_ble.c - NimBLE peripheral bring-up for CH32V208.
 *
 * Pattern follows NIMBLE/apps/bleprph (Apache mynewt-nimble), adapted to a
 * FreeRTOS port where:
 *   - nimble_port_init() (port/nimble_port_wch.c) initializes msys pools, the
 *     controller (ble_ll_init), the transport and the host (ble_hs_init via
 *     ble_transport_hs_init), plus the WCH BLE hardware.
 *   - nimble_port_freertos_init() spawns the "ll" and "ble" tasks; the host
 *     task runs nimble_port_run() which pumps the default event queue.
 *
 * GATT registration timing (verified against NIMBLE master, ble_gatts.c):
 *   ble_hs_init() -> ble_gatts_init() only resets stats/counters. Services
 *   added with ble_gatts_add_svcs() are kept on a heap list; the ATT database
 *   is materialized later in ble_gatts_start(), called from ble_hs_start()
 *   via the "stage1" start event that ble_hs_init() enqueued on the default
 *   event queue.  That event only runs once the host task starts consuming
 *   the queue.  Therefore: registering services any time between
 *   nimble_port_init() and the first nimble_port_run() iteration is safe.
 *   We register right after configuring ble_hs_cfg, before spawning tasks,
 *   mirroring the ESP-IDF NimBLE peripheral example.  (mynewt builds instead
 *   use the .bss.nimble_svc NES section; that mechanism is absent here and
 *   not needed.)
 *
 * Address handling: NIMBLE's host/util (ble_hs_util_ensure_addr) is NOT part
 * of this build (nimble/host/src only).  sync_cb therefore relies on the
 * host start sequence having read the controller's BD_ADDR (ble_hs_startup.c
 * -> ble_hs_id_set_pub) and calls ble_hs_id_infer_auto(0, ...); if no
 * identity address is available it generates a static random address and
 * retries.
 *
 * Application logs go through console_printf() (USART1 DMA backend).
 */

#include <string.h>
#include <stdio.h>

#include "FreeRTOS.h"
#include "task.h"

#include "console/console.h"

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "nimble/ble.h"
#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_store.h"
#include "host/ble_uuid.h"
#include "host/ble_l2cap.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#include "app_ble.h"
#include "app_perf.h"

/* ---------------------------------------------------- forward declarations */
static void app_ble_advertise(void);
static int app_ble_gap_event(struct ble_gap_event *event, void *arg);

/* ------------------------------------------------------------ constants --- */

#define APP_BLE_DEV_NAME        "CH32V208-NIMBLE"

/* Legacy advertising interval in BLE units (0.625 ms): 160 -> 100 ms. */
#define APP_BLE_ADV_ITVL_MIN    160
#define APP_BLE_ADV_ITVL_MAX    160

/* Two-byte vendor field appended to the advertising payload for sniffer
 * identification ('W', 'C'). */
static const uint8_t app_ble_mfg_data[2] = { 0x57, 0x43 };

/* Private test service, 128-bit UUIDs (listed MSB-first here for readability;
 * BLE_UUID128_INIT takes little-endian byte order). */
/* 7a02480e-6d4d-4e8e-b7a2-d1c4a2f13e01 */
static const ble_uuid128_t app_svc_uuid =
    BLE_UUID128_INIT(0x01, 0x3e, 0xf1, 0xa2, 0xc4, 0xd1, 0xa2, 0xb7,
                     0x8e, 0x4e, 0x4d, 0x6d, 0x0e, 0x48, 0x02, 0x7a);
/* 7a02480e-6d4d-4e8e-b7a2-d1c4a2f13e02 - uptime seconds (read) */
static const ble_uuid128_t app_chr_uptime_uuid =
    BLE_UUID128_INIT(0x02, 0x3e, 0xf1, 0xa2, 0xc4, 0xd1, 0xa2, 0xb7,
                     0x8e, 0x4e, 0x4d, 0x6d, 0x0e, 0x48, 0x02, 0x7a);
/* 7a02480e-6d4d-4e8e-b7a2-d1c4a2f13e03 - echo buffer (read + write) */
static const ble_uuid128_t app_chr_echo_uuid =
    BLE_UUID128_INIT(0x03, 0x3e, 0xf1, 0xa2, 0xc4, 0xd1, 0xa2, 0xb7,
                     0x8e, 0x4e, 0x4d, 0x6d, 0x0e, 0x48, 0x02, 0x7a);

/* PHY test control/status: write 1 (1M), 2 (2M), 3 (coded S=2 / 500kbps) or
 * 4 (coded S=8 / 125kbps); read version, HCI TX/RX PHY (3 = coded), pending,
 * last update status, requested selector (six bytes). */
static const ble_uuid128_t app_chr_phy_uuid =
    BLE_UUID128_INIT(0x04, 0x3e, 0xf1, 0xa2, 0xc4, 0xd1, 0xa2, 0xb7,
                     0x8e, 0x4e, 0x4d, 0x6d, 0x0e, 0x48, 0x02, 0x7a);
static uint16_t app_chr_phy_handle;
static const ble_uuid128_t app_chr_perf_uuid =
    BLE_UUID128_INIT(0x05, 0x3e, 0xf1, 0xa2, 0xc4, 0xd1, 0xa2, 0xb7,
                     0x8e, 0x4e, 0x4d, 0x6d, 0x0e, 0x48, 0x02, 0x7a);
static const ble_uuid128_t app_chr_bulk_uuid =
    BLE_UUID128_INIT(0x06, 0x3e, 0xf1, 0xa2, 0xc4, 0xd1, 0xa2, 0xb7,
                     0x8e, 0x4e, 0x4d, 0x6d, 0x0e, 0x48, 0x02, 0x7a);
static uint8_t app_phy_pending, app_phy_status, app_phy_requested;

/* GATT state: value handles are filled in by the gatts_register_cb. */
static uint16_t app_chr_uptime_handle;
static uint16_t app_chr_echo_handle;

/* Backing store of the read/write characteristic. */
#define APP_ECHO_BUF_LEN 64
static uint8_t app_echo_buf[APP_ECHO_BUF_LEN];
static uint16_t app_echo_len;

static uint8_t app_own_addr_type;

/* Raw byte echo, LE CoC PSM 0x0080. Client uses stop-and-wait writes <=512 B.
 * SDUs may span several MPS/ACL packets; never assume a contiguous mbuf. */
#define APP_COC_PSM 0x0080
#define APP_COC_MTU 2048
static uint32_t app_coc_bytes, app_coc_sdus;
static uint8_t app_coc_stalled;

static int
app_coc_rx_ready(struct ble_l2cap_chan *chan)
{
    struct os_mbuf *rx = os_msys_get_pkthdr(0, 0);
    if (!rx) {
        return BLE_HS_ENOMEM;
    }
    /* This NimBLE revision attaches rx before looking up the connection;
     * channel teardown owns it even when recv_ready returns an error. */
    return ble_l2cap_recv_ready(chan, rx);
}

static int
app_coc_event(struct ble_l2cap_event *event, void *arg)
{
    struct ble_l2cap_chan_info info;
    struct ble_l2cap_chan *chan;
    struct os_mbuf *sdu;
    int rc;

    switch (event->type) {
    case BLE_L2CAP_EVENT_COC_ACCEPT:
        if (event->accept.peer_sdu_size < 512) {
            return BLE_HS_EINVAL;
        }
        return app_coc_rx_ready(event->accept.chan);
    case BLE_L2CAP_EVENT_COC_CONNECTED:
        app_coc_bytes = app_coc_sdus = app_coc_stalled = 0;
        if (event->connect.status == 0 &&
            ble_l2cap_get_chan_info(event->connect.chan, &info) == 0) {
            app_perf_coc_connected(event->connect.chan);
            console_printf("l2cap: connected psm=%u mtu=%u peer_mtu=%u mps=%u\n",
                           info.psm, info.our_coc_mtu, info.peer_coc_mtu,
                           MYNEWT_VAL(BLE_L2CAP_COC_MPS));
        } else {
            console_printf("l2cap: connect status=%d\n", event->connect.status);
        }
        return 0;
    case BLE_L2CAP_EVENT_COC_DATA_RECEIVED:
        if (app_perf_coc_receive(event->receive.chan, event->receive.sdu_rx)) return 0;
        chan = event->receive.chan;
        sdu = event->receive.sdu_rx;
        app_coc_bytes += OS_MBUF_PKTLEN(sdu);
        app_coc_sdus++;
        if (app_coc_stalled) {
            os_mbuf_free_chain(sdu);
            rc = BLE_HS_EBUSY;
        } else {
            rc = ble_l2cap_send(chan, sdu);
            /* EBUSY/EBADDATA do not consume sdu; other errors from this
             * NimBLE revision's continue_tx already free it. */
            if (rc == BLE_HS_EBUSY || rc == BLE_HS_EBADDATA) {
                os_mbuf_free_chain(sdu);
            }
        }
        if (rc == BLE_HS_ESTALLED) {
            app_coc_stalled = 1;
            return 0; /* Resume receive/credits when TX unblocks. */
        }
        if (rc == 0) {
            rc = app_coc_rx_ready(chan);
        }
        break;
    case BLE_L2CAP_EVENT_COC_TX_UNSTALLED:
        app_perf_coc_unstalled(event->tx_unstalled.status);
        if (!app_coc_stalled) return 0;
        chan = event->tx_unstalled.chan;
        app_coc_stalled = 0;
        rc = event->tx_unstalled.status;
        if (rc == 0) {
            rc = app_coc_rx_ready(chan);
        }
        break;
    case BLE_L2CAP_EVENT_COC_DISCONNECTED:
        app_perf_coc_disconnected();
        console_printf("l2cap: disconnected sdus=%lu bytes=%lu\n",
                       (unsigned long)app_coc_sdus, (unsigned long)app_coc_bytes);
        app_coc_stalled = 0;
        return 0;
    default:
        return 0;
    }
    if (rc != 0) {
        console_printf("l2cap: echo failed rc=%d\n", rc);
        ble_l2cap_disconnect(chan);
    }
    return 0;
}

/* --------------------------------------------------------------- GATT ----- */

static uint32_t
app_uptime_seconds(void)
{
    return (uint32_t)(xTaskGetTickCount() / configTICK_RATE_HZ);
}

static int
app_gatt_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                   struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    uint32_t uptime;
#if WCH_BLE_PACKET_LOGS
    uint16_t i;
#endif
    uint8_t phy[6] = {1, 0, 0, 0, 0, 0};
    uint16_t len;
    int rc;

    if (attr_handle == app_perf_control_handle || attr_handle == app_perf_data_handle)
        return app_perf_access(conn_handle, attr_handle, ctxt);
    switch (ctxt->op) {
    case BLE_GATT_ACCESS_OP_READ_CHR:
        if (attr_handle == app_chr_phy_handle) {
            rc = ble_gap_read_le_phy(conn_handle, &phy[1], &phy[2]);
            if (rc != 0) {
                return BLE_ATT_ERR_UNLIKELY;
            }
            phy[3] = app_phy_pending;
            phy[4] = app_phy_status;
            phy[5] = app_phy_requested;
            return os_mbuf_append(ctxt->om, phy, sizeof phy) == 0 ?
                   0 : BLE_ATT_ERR_INSUFFICIENT_RES;
        }
        if (attr_handle == app_chr_uptime_handle) {
            uptime = app_uptime_seconds();
            rc = os_mbuf_append(ctxt->om, &uptime, sizeof uptime);
            return rc == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
        }
        if (attr_handle == app_chr_echo_handle) {
            rc = os_mbuf_append(ctxt->om, app_echo_buf, app_echo_len);
            return rc == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
        }
        return BLE_ATT_ERR_READ_NOT_PERMITTED;

    case BLE_GATT_ACCESS_OP_WRITE_CHR:
        if (attr_handle == app_chr_phy_handle) {
            uint8_t mask;
            uint16_t opts;
            rc = ble_hs_mbuf_to_flat(ctxt->om, phy, 1, &len);
            if (rc != 0 || len != 1 || phy[0] < 1 || phy[0] > 4) {
                return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
            }
            if (app_phy_pending) {
                return BLE_ATT_ERR_UNLIKELY;
            }
            switch (phy[0]) {
            case 2:
                mask = BLE_GAP_LE_PHY_2M_MASK;
                opts = BLE_GAP_LE_PHY_CODED_ANY;
                break;
            case 3:
                mask = BLE_GAP_LE_PHY_CODED_MASK;
                opts = BLE_GAP_LE_PHY_CODED_S2;
                break;
            case 4:
                mask = BLE_GAP_LE_PHY_CODED_MASK;
                opts = BLE_GAP_LE_PHY_CODED_S8;
                break;
            default:
                mask = BLE_GAP_LE_PHY_1M_MASK;
                opts = BLE_GAP_LE_PHY_CODED_ANY;
                break;
            }
            rc = ble_gap_set_prefered_le_phy(conn_handle, mask, mask, opts);
            console_printf("phy: request handle=%u sel=%u mask=%u opts=%u rc=%d\n",
                           conn_handle, phy[0], mask, opts, rc);
            if (rc != 0) {
                return BLE_ATT_ERR_UNLIKELY;
            }
            app_phy_requested = phy[0];
            app_phy_pending = 1;
            app_phy_status = 0xff;
            return 0;
        }
        if (attr_handle == app_chr_echo_handle) {
            rc = ble_hs_mbuf_to_flat(ctxt->om, app_echo_buf,
                                     sizeof app_echo_buf, &app_echo_len);
            if (rc != 0) {
                return BLE_ATT_ERR_UNLIKELY;
            }
#if WCH_BLE_PACKET_LOGS
            console_printf("gatt write: handle=%u len=%u data:",
                           attr_handle, app_echo_len);
            for (i = 0; i < app_echo_len; i++) {
                console_printf(" %02x", app_echo_buf[i]);
            }
            console_printf("\n");
#endif
            return 0;
        }
        return BLE_ATT_ERR_WRITE_NOT_PERMITTED;

    default:
        return BLE_ATT_ERR_UNLIKELY;
    }
}

static const struct ble_gatt_svc_def app_gatt_svcs[] = { {
    .type = BLE_GATT_SVC_TYPE_PRIMARY,
    .uuid = &app_svc_uuid.u,
    .characteristics = (struct ble_gatt_chr_def[]) { {
        /* Uptime seconds, read-only. */
        .uuid = &app_chr_uptime_uuid.u,
        .access_cb = app_gatt_access_cb,
        .arg = NULL,
        .flags = BLE_GATT_CHR_F_READ,
        .val_handle = &app_chr_uptime_handle,
    }, {
        /* Echo buffer, read + write. */
        .uuid = &app_chr_echo_uuid.u,
        .access_cb = app_gatt_access_cb,
        .arg = NULL,
        .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE,
        .val_handle = &app_chr_echo_handle,
    }, {
        .uuid = &app_chr_phy_uuid.u,
        .access_cb = app_gatt_access_cb,
        .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE,
        .val_handle = &app_chr_phy_handle,
    }, {
        .uuid = &app_chr_perf_uuid.u,
        .access_cb = app_gatt_access_cb,
        .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE,
        .val_handle = &app_perf_control_handle,
    }, {
        .uuid = &app_chr_bulk_uuid.u,
        .access_cb = app_gatt_access_cb,
        .flags = BLE_GATT_CHR_F_WRITE_NO_RSP | BLE_GATT_CHR_F_NOTIFY,
        .val_handle = &app_perf_data_handle,
    }, {
        0, /* no more characteristics */
    } },
}, {
    0, /* no more services */
} };

/* Called by the host while the GATT database is being materialized inside
 * ble_gatts_start(); logs the assigned attribute handles. */
static void
app_gatt_register_cb(struct ble_gatt_register_ctxt *ctxt, void *arg)
{
    char buf[BLE_UUID_STR_LEN];

    switch (ctxt->op) {
    case BLE_GATT_REGISTER_OP_SVC:
        console_printf("gatt: svc %s handle=%d\n",
                       ble_uuid_to_str(ctxt->svc.svc_def->uuid, buf),
                       ctxt->svc.handle);
        break;
    case BLE_GATT_REGISTER_OP_CHR:
        console_printf("gatt: chr %s def=%d val=%d\n",
                       ble_uuid_to_str(ctxt->chr.chr_def->uuid, buf),
                       ctxt->chr.def_handle, ctxt->chr.val_handle);
        break;
    default:
        break;
    }
}

/* --------------------------------------------------------- advertising ---- */

static void
app_ble_advertise(void)
{
    struct ble_gap_adv_params adv_params;
    struct ble_hs_adv_fields fields;
    const char *name = APP_BLE_DEV_NAME;
    int rc;

    memset(&fields, 0, sizeof fields);

    /* General discoverable, BR/EDR unsupported.
     * Payload budget check (legacy adv, 31 octets):
     *   flags 3 + name 2+15 + mfg 2+2 = 24. */
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;

    fields.name = (const uint8_t *)name;
    fields.name_len = strlen(name);
    fields.name_is_complete = 1;

    fields.mfg_data = app_ble_mfg_data;
    fields.mfg_data_len = sizeof app_ble_mfg_data;

    rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        console_printf("adv: set fields failed rc=%d\n", rc);
        return;
    }

    /* Repeat the complete name in scan response. In active scans macOS can
     * retain the previous firmware's response name for the same public MAC. */
    memset(&fields, 0, sizeof fields);
    fields.name = (const uint8_t *)name;
    fields.name_len = strlen(name);
    fields.name_is_complete = 1;
    rc = ble_gap_adv_rsp_set_fields(&fields);
    if (rc != 0) {
        console_printf("adv: set scan response failed rc=%d\n", rc);
        return;
    }

    memset(&adv_params, 0, sizeof adv_params);
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    adv_params.itvl_min = APP_BLE_ADV_ITVL_MIN;
    adv_params.itvl_max = APP_BLE_ADV_ITVL_MAX;

    rc = ble_gap_adv_start(app_own_addr_type, NULL, BLE_HS_FOREVER,
                           &adv_params, app_ble_gap_event, NULL);
    if (rc != 0) {
        console_printf("adv: start failed rc=%d\n", rc);
        return;
    }
    console_printf("adv: advertising (%s addr), itvl=%d..%d units\n",
                   app_own_addr_type == BLE_OWN_ADDR_PUBLIC ? "public"
                                                            : "random",
                   APP_BLE_ADV_ITVL_MIN, APP_BLE_ADV_ITVL_MAX);
}

/* Make sure an identity address exists and pick the own-address type for
 * advertising (public preferred; no privacy in v1).  Replacement for the
 * NimBLE host/util ble_hs_util_ensure_addr(), which is not built here. */
static int
app_ble_ensure_addr(void)
{
    ble_addr_t addr;
    int rc;

#if APP_BLE_TEST_IDENTITY
    /* Separate, deterministic identity for cache-sensitive GATT development.
     * No option bytes, Flash MAC or persistent storage are changed.
     * 2026-10-30: XOR constant bumped 0x5a -> 0x5b - the GAP/GATT standard
     * service registration added in 49bcbdf shifted the custom service
     * handles, and the macOS GATT cache for the previous identity address
     * returned stale handle data (uptime chr read the device name).
     * 2026-10-31: 0x5c gives the new perf control/bulk characteristics a
     * fresh macOS discovery identity; existing value handles stay stable. */
    rc = ble_hs_id_copy_addr(BLE_ADDR_PUBLIC, addr.val, NULL);
    if (rc != 0) {
        rc = ble_hs_id_gen_rnd(0, &addr);
        if (rc != 0) {
            return rc;
        }
    }
    addr.val[0] ^= 0x5c;
    addr.val[5] |= 0xc0;
    rc = ble_hs_id_set_rnd(addr.val);
    if (rc == 0) {
        app_own_addr_type = BLE_OWN_ADDR_RANDOM;
        console_printf("ble: separate static random test identity enabled\n");
    }
    return rc;
#endif
    rc = ble_hs_id_infer_auto(0, &app_own_addr_type);
    if (rc == 0) {
        return 0;
    }

    /* Host startup failed to obtain a BD_ADDR from the controller (e.g. the
     * LL does not implement HCI Read BD_ADDR).  Fall back to a static random
     * address generated from the controller RNG. */
    console_printf("ble: no identity addr (rc=%d), generating static rnd\n",
                   rc);
    rc = ble_hs_id_gen_rnd(0, &addr);
    if (rc != 0) {
        return rc;
    }
    rc = ble_hs_id_set_rnd(addr.val);
    if (rc != 0) {
        return rc;
    }
    return ble_hs_id_infer_auto(0, &app_own_addr_type);
}

/* ----------------------------------------------------------- GAP events --- */

static int
app_ble_gap_event(struct ble_gap_event *event, void *arg)
{
    struct ble_gap_conn_desc desc;
    int rc;

    app_perf_gap(event);
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            app_phy_pending = app_phy_status = app_phy_requested = 0;
            rc = ble_gap_conn_find(event->connect.conn_handle, &desc);
            if (rc == 0) {
                console_printf("conn: established handle=%d itvl=%d "
                               "latency=%d timeout=%d\n",
                               desc.conn_handle, desc.conn_itvl,
                               desc.conn_latency, desc.supervision_timeout);
            }
            /* Kick off MTU exchange (peripheral side is allowed to as well). */
            rc = ble_gattc_exchange_mtu(event->connect.conn_handle,
                                        NULL, NULL);
            if (rc != 0) {
                console_printf("conn: mtu exchange start failed rc=%d\n", rc);
            }
        } else {
            console_printf("conn: failed status=%d, resuming adv\n",
                           event->connect.status);
            app_ble_advertise();
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        console_printf("conn: disconnected handle=%d reason=0x%02x\n",
                       event->disconnect.conn.conn_handle,
                       event->disconnect.reason);
        console_printf("conn: resources msys_free=%d freertos_free=%u min=%u\n",
                       os_msys_num_free(), (unsigned)xPortGetFreeHeapSize(),
                       (unsigned)xPortGetMinimumEverFreeHeapSize());
        app_ble_advertise();
        return 0;

    case BLE_GAP_EVENT_PHY_UPDATE_COMPLETE:
        app_phy_pending = 0;
        app_phy_status = event->phy_updated.status;
        console_printf("phy: updated handle=%u status=%u tx=%u rx=%u\n",
                       event->phy_updated.conn_handle, event->phy_updated.status,
                       event->phy_updated.tx_phy, event->phy_updated.rx_phy);
        return 0;

    case BLE_GAP_EVENT_CONN_UPDATE:
        console_printf("conn: update status=%d\n", event->conn_update.status);
        return 0;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        console_printf("adv: complete reason=%d, resuming\n",
                       event->adv_complete.reason);
        app_ble_advertise();
        return 0;

    case BLE_GAP_EVENT_MTU:
        console_printf("conn: mtu updated handle=%d cid=%d mtu=%d\n",
                       event->mtu.conn_handle, event->mtu.channel_id,
                       event->mtu.value);
        return 0;

    case BLE_GAP_EVENT_SUBSCRIBE:
        console_printf("gatt: subscribe handle=%d notify=%d indicate=%d\n",
                       event->subscribe.attr_handle,
                       event->subscribe.cur_notify,
                       event->subscribe.cur_indicate);
        return 0;

    default:
        return 0;
    }
}

/* --------------------------------------------------------- host config ---- */

static void
app_ble_on_sync(void)
{
    uint8_t addr_val[6];
    int rc;

    rc = app_ble_ensure_addr();
    if (rc != 0) {
        console_printf("ble: cannot determine address rc=%d\n", rc);
        return;
    }

    rc = ble_hs_id_copy_addr(
        app_own_addr_type == BLE_OWN_ADDR_PUBLIC ? BLE_ADDR_PUBLIC
                                                 : BLE_ADDR_RANDOM,
        addr_val, NULL);
    if (rc == 0) {
        console_printf("ble: synced, own addr "
                       "%02x:%02x:%02x:%02x:%02x:%02x (%s)\n",
                       addr_val[5], addr_val[4], addr_val[3],
                       addr_val[2], addr_val[1], addr_val[0],
                       app_own_addr_type == BLE_OWN_ADDR_PUBLIC ? "public"
                                                                : "random");
    }

    app_ble_advertise();
}

static void
app_ble_on_reset(int reason)
{
    console_printf("ble: host reset, reason=%d\n", reason);
}

/* ------------------------------------------------------------ host task --- */

static void
app_ble_host_task(void *param)
{
    (void)param;

    console_printf("ble: host task start\n");

    /* Blocks forever servicing the default event queue (HCI events, host
     * start stage1 -> ble_gatts_start() -> sync_cb, GAP timers).  Returns
     * only if someone calls nimble_port_stop(); restore state then and end
     * the task cleanly - a FreeRTOS task function must not return. */
    nimble_port_run();

    console_printf("ble: host task exit\n");
    vTaskDelete(NULL);
}

/* -------------------------------------------------------------- public ---- */

void
app_ble_start(void)
{
    int rc;

    console_printf("ble: bringing up NimBLE controller+host\n");

    /* 1. LL/transport/host init + WCH BLE hardware (port/nimble_port_wch.c).
     *    Internally calls ble_hs_init() which enqueues the host-start event
     *    on the default event queue (processed later by the host task). */
    nimble_port_init();
    app_perf_init();

    /* 2. Host configuration - must happen before the start event runs. */
    ble_hs_cfg.reset_cb = app_ble_on_reset;
    ble_hs_cfg.sync_cb = app_ble_on_sync;
    ble_hs_cfg.gatts_register_cb = app_gatt_register_cb;
    /* Ignore-store-errors policy: no persistence backend in v1 (no pairing
     * / bonding anyway). */
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;

    /* 3. Standard services are not installed by ble_hs_init(). Register
     * them explicitly, as the upstream bleprph application does. */
    ble_svc_gap_init();
    ble_svc_gatt_init();
    rc = ble_svc_gap_device_name_set(APP_BLE_DEV_NAME);
    configASSERT(rc == 0);

    /* GATT service registration: safe here - ble_gatts_add_svcs() only
     *    queues the definition; the ATT database is built in
     *    ble_gatts_start() once the host task services the start event. */
    rc = ble_gatts_count_cfg(app_gatt_svcs);
    if (rc != 0) {
        console_printf("ble: gatts count_cfg failed rc=%d\n", rc);
        return;
    }
    rc = ble_gatts_add_svcs(app_gatt_svcs);
    if (rc != 0) {
        console_printf("ble: gatts add_svcs failed rc=%d\n", rc);
        return;
    }

    rc = ble_l2cap_create_server(APP_COC_PSM, APP_COC_MTU, app_coc_event, NULL);
    configASSERT(rc == 0);
    console_printf("l2cap: echo server psm=%u mtu=%u\n", APP_COC_PSM, APP_COC_MTU);

    /* 4. Spawn "ll" (highest prio) and "ble" tasks; the host task drives
     *    everything from here on. */
    nimble_port_freertos_init(app_ble_host_task);

    console_printf("ble: stack started, waiting for host sync\n");
}
