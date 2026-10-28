/*
 * syscfg_overrides.h - CH32V208 (FreeRTOS, controller+host on one chip)
 *
 * Included at the very top of config/syscfg/syscfg.h, *before* the stock
 * defaults.  The stock header wraps every default in "#ifndef", so a plain
 * #define here overrides it.  Names marked [new] do not exist in the vendored
 * porting/nimble/include/syscfg/syscfg.h at all (that header only carries the
 * host/porting defaults) and are mandatory for the controller/transport
 * sources to compile; their values mirror the nimble syscfg.yml defaults
 * evaluated for our configuration.
 *
 * Every value below was verified against:
 *   nimble/transport/syscfg.yml  (BLE_TRANSPORT_*)
 *   nimble/controller/syscfg.yml (BLE_LL_* / BLE_CONTROLLER / FEM)
 *   nimble/host/syscfg.yml       (BLE_* host settings)
 */

#ifndef H_SYSCFG_OVERRIDES_WCH_
#define H_SYSCFG_OVERRIDES_WCH_

/* ------------------------------------------------------------- buildout ---
 * controller/syscfg.yml:20  "BLE_CONTROLLER: 1".
 * [new] not present in vendored syscfg.h - needed by host/transport glue
 * (#if MYNEWT_VAL(BLE_CONTROLLER) in ble_hs_startup.c, hci_*.c ...). */
#define MYNEWT_VAL_BLE_CONTROLLER (1)
/* host: vendored syscfg.h:1075 already defaults to 1; kept explicit. */
#define MYNEWT_VAL_BLE_HOST (1)

/* ----------------------------------------------------------- transport ----
 * transport/syscfg.yml:22-50: BLE_TRANSPORT_HS / BLE_TRANSPORT_LL choices.
 * "native" == host and controller run on the same core, HCI goes through
 * direct function calls.  Choice macro form is MYNEWT_VAL_<NAME>__<choice>.
 * LL__native MUST be overridden: vendored syscfg.h:1563 defaults LL__native
 * to 0 and LL__socket to 1. */
#define MYNEWT_VAL_BLE_TRANSPORT_HS__native (1)
#define MYNEWT_VAL_BLE_TRANSPORT_LL__native (1)
#define MYNEWT_VAL_BLE_TRANSPORT_LL__socket (0)
/* The non-selected transports stay at their vendored defaults (all 0):
 * HS__cdc/custom/dialog_cmac/nrf5340/uart/usb = 0 (syscfg.h:1510-1529),
 * LL__apollo3/custom/dialog_cmac/emspi/nrf5340/uart_ll = 0 (syscfg.h:1551-1573).
 *
 * MSYS / transport pools: 沿用 vendored 默认
 *   MSYS_1: 12 blocks x 292 B (syscfg.h:402-407)  - covers 251 B ACL + overhead
 *   MSYS_2: disabled        (syscfg.h:414-419)
 *   BLE_TRANSPORT_ACL_COUNT=10/251B, EVT_COUNT=4, EVT_DISCARDABLE_COUNT=16,
 *   EVT_SIZE=70 (syscfg.h:1490-1507; also transport/syscfg.yml:52-89)
 * With HS__native && LL__native the transport ACL/ISO pools are compiled out
 * (transport.c POOL_ACL_COUNT=0), so these defaults are only RAM-light.
 *
 * BLE_TRANSPORT_RX_TASK_STACK_SIZE: intentionally NOT defined here.  The
 * vendored header #undefs it (syscfg.h:1579) and rx_task.c then only
 * instantiates the rx event queue, which nothing drains in the native/native
 * configuration - the LL hands events straight to the host task. */

/* ------------------------------------------------------------- cputime ----
 * vendored syscfg.h:447/451 already default to 1 MHz / timer 0; kept explicit.
 * 1 MHz == OS_CPUTIME_FREQ_1MHZ in os/os_cputime.h: usecs_to_ticks() becomes
 * the identity, and controller/ble_ll_tmr.h USECS_PER_TICK becomes 1, so all
 * LL scheduling math is exact (the 32768 Hz shortcut path in ble_ll_tmr.h
 * is not compiled).  Backed by TIM3 (see port/hal_timer_wch.c). */
#define MYNEWT_VAL_OS_CPUTIME_FREQ (1000000)
#define MYNEWT_VAL_OS_CPUTIME_TIMER_NUM (0)

/* ------------------------------------------------- controller features ----
 * v1 bring-up: single connection, no extended advertising / privacy /
 * 2M / coded PHY.  Encryption on (SMP legacy + LE secure Pairing Request
 * feature bits); DTM on for RF verification. */
#define MYNEWT_VAL_BLE_MAX_CONNECTIONS (1)          /* host syscfg.yml; vendored syscfg.h:827 = 1. NOTE: master has no BLE_LL_MAX_CONNECTIONS (verified by grep) */
#define MYNEWT_VAL_BLE_LL_CFG_FEAT_LE_ENCRYPTION (1)   /* controller/syscfg.yml:241 default is already 1 for C||P; explicit */
#define MYNEWT_VAL_BLE_LL_CFG_FEAT_LL_EXT_ADV (0)      /* controller/syscfg.yml:307 = BLE_EXT_ADV = 0 */
#define MYNEWT_VAL_BLE_LL_CFG_FEAT_LL_PRIVACY (0)      /* controller/syscfg.yml:286 default 1 -> overridden off */
#define MYNEWT_VAL_BLE_LL_CFG_FEAT_LE_2M_PHY (0)       /* controller/syscfg.yml:297 = BLE_PHY_2M = 0 */
#define MYNEWT_VAL_BLE_LL_CFG_FEAT_LE_CODED_PHY (0)    /* controller/syscfg.yml:302 = BLE_PHY_CODED = 0 */
#define MYNEWT_VAL_BLE_LL_DTM (1)                      /* controller/syscfg.yml:408 default 0 -> overridden on (RF bring-up) */
#define MYNEWT_VAL_BLE_LL_WHITELIST_SIZE (8)           /* controller/syscfg.yml:102 default '8'; explicit */
#define MYNEWT_VAL_BLE_LL_SCA (20)                     /* controller/syscfg.yml:61 default 60 -> 20 ppm: our cputime is HSE(32MHz)-derived, crystal class */

/* Flow control / monitor / HCI logging: 沿用默认并显式化 (syscfg.h:1091). */
#define MYNEWT_VAL_BLE_HS_FLOW_CTRL (0)
/* [new] transport monitor (monitor.c); vendored syscfg.h has no default. */
#define MYNEWT_VAL_BLE_MONITOR (0)

/* BLE_LL_SCAN_AUX_SEGMENT_CNT: default 0 (controller/syscfg.yml:384-388);
 * the "8" in syscfg.vals (yml:685) only applies when EXT_ADV is on. */
#define MYNEWT_VAL_BLE_LL_SCAN_AUX_SEGMENT_CNT (0)
/* [new] BLE_HW_WHITELIST_ENABLE: controller/syscfg.yml:46 default 1, which
 * makes the LL use ble_hw_whitelist_*().  WCH BB has no whitelist assist, so
 * keep the LL software whitelist (BLE_LL_WHITELIST_SIZE above) instead. */
#define MYNEWT_VAL_BLE_HW_WHITELIST_ENABLE (0)

/* --- The remaining [new] BLE_LL_* values: controller/syscfg.yml defaults,
 *     evaluated for ROLE_* = 1 (vendored syscfg.h:1060s BLE_ROLE_* all 1). --*/
#define MYNEWT_VAL_BLE_LL_ROLE_CENTRAL (1)              /* yml:22-27 */
#define MYNEWT_VAL_BLE_LL_ROLE_PERIPHERAL (1)           /* yml:28-33 */
#define MYNEWT_VAL_BLE_LL_ROLE_BROADCASTER (1)          /* yml:34-39 */
#define MYNEWT_VAL_BLE_LL_ROLE_OBSERVER (1)             /* yml:40-45 */
#define MYNEWT_VAL_BLE_LL_PRIO (0)                      /* yml:68-70 */
#define MYNEWT_VAL_BLE_LL_TX_PWR_DBM (0)                /* yml:71-74 */
#define MYNEWT_VAL_BLE_LL_TX_PWR_MAX_DBM (20)           /* yml:75-78 */
#define MYNEWT_VAL_BLE_LL_NUM_COMP_PKT_ITVL_MS (2000)   /* yml:80-84 */
#define MYNEWT_VAL_BLE_LL_MANUFACTURER_ID (0x0B65)      /* yml:86-89 */
#define MYNEWT_VAL_BLE_LL_NUM_SCAN_DUP_ADVS (8)         /* yml:93-95 */
#define MYNEWT_VAL_BLE_LL_NUM_SCAN_RSP_ADVS (8)         /* yml:96-100 */
#define MYNEWT_VAL_BLE_LL_RESOLV_LIST_SIZE (4)          /* yml:106-108 */
#define MYNEWT_VAL_BLE_LL_CONN_PHY_DEFAULT_PREF_MASK (0x07) /* yml:110-114 */
#define MYNEWT_VAL_BLE_LL_CONN_PHY_PREFER_2M (0)        /* yml:115-121 */
#define MYNEWT_VAL_BLE_LL_CONN_PHY_INIT_UPDATE (0)      /* yml:122-126 */
#define MYNEWT_VAL_BLE_LL_MAX_PKT_SIZE (251)            /* yml:131-133 */
#define MYNEWT_VAL_BLE_LL_SUPP_MAX_RX_BYTES (251)       /* yml:134-136 */
#define MYNEWT_VAL_BLE_LL_SUPP_MAX_TX_BYTES (251)       /* yml:137-139 */
#define MYNEWT_VAL_BLE_LL_CONN_INIT_MAX_TX_BYTES (27)   /* yml:140-145 */
#define MYNEWT_VAL_BLE_LL_CONN_INIT_AUTO_DLE (1)        /* yml:146-151 */
#define MYNEWT_VAL_BLE_LL_CONN_INIT_SLOTS (4)           /* yml:154-163 */
#define MYNEWT_VAL_BLE_LL_CONN_INIT_MIN_WIN_OFFSET (0)  /* yml:165-173 */
#define MYNEWT_VAL_BLE_LL_CONN_STRICT_SCHED (0)         /* yml:175-185 */
#define MYNEWT_VAL_BLE_LL_CONN_STRICT_SCHED_FIXED (0)   /* yml:186-191 */
#define MYNEWT_VAL_BLE_LL_CONN_STRICT_SCHED_SLOT_US (3750)   /* yml:192-195 */
#define MYNEWT_VAL_BLE_LL_CONN_STRICT_SCHED_PERIOD_SLOTS (8) /* yml:196-200 */
#define MYNEWT_VAL_BLE_LL_RNG_BUFSIZE (32)              /* yml:203-209 */
#define MYNEWT_VAL_BLE_LL_RFMGMT_ENABLE_TIME (0)        /* yml:211-222: 0 = keep PHY always enabled */
#define MYNEWT_VAL_BLE_LL_HCI_LLCP_TRACE (0)            /* yml:224-227 */
#define MYNEWT_VAL_BLE_LL_ENC_SHORT_PKT_CNTR (0)        /* yml:247-255 */
#define MYNEWT_VAL_BLE_LL_CONN_EVENT_END_MARGIN (0)     /* yml:660-667 */
#define MYNEWT_VAL_BLE_LL_CFG_FEAT_CONN_PARAM_REQ (1)   /* yml:257-262: C||P */
#define MYNEWT_VAL_BLE_LL_CFG_FEAT_PERIPH_INIT_FEAT_XCHG (1) /* yml:264-269: C||P */
#define MYNEWT_VAL_BLE_LL_CFG_FEAT_LE_PING (1)          /* yml:271-276: = ENCRYPTION */
#define MYNEWT_VAL_BLE_LL_CFG_FEAT_DATA_LEN_EXT (1)     /* yml:278-284: C||P */
#define MYNEWT_VAL_BLE_LL_CFG_FEAT_LE_CSA2 (0)          /* yml:291-295 default '0' (only forced on with EXT_ADV, yml:683) */
#define MYNEWT_VAL_BLE_LL_CFG_FEAT_LL_PERIODIC_ADV (0)  /* yml:314-318 = BLE_PERIODIC_ADV = 0 */
#define MYNEWT_VAL_BLE_LL_CFG_FEAT_LL_PERIODIC_ADV_SYNC_CNT (1)        /* yml:320-323 = BLE_MAX_PERIODIC_SYNCS = 1 */
#define MYNEWT_VAL_BLE_LL_CFG_FEAT_LL_PERIODIC_ADV_SYNC_LIST_CNT (1)   /* yml:325-328 */
#define MYNEWT_VAL_BLE_LL_CFG_FEAT_LL_PERIODIC_ADV_SYNC_TRANSFER (0)   /* yml:330-334 */
#define MYNEWT_VAL_BLE_LL_CFG_FEAT_LL_PERIODIC_ADV_ADI_SUPPORT (0)     /* yml:336-340 */
#define MYNEWT_VAL_BLE_LL_CFG_FEAT_CTRL_TO_HOST_FLOW_CONTROL (0)       /* yml:345-350 */
#define MYNEWT_VAL_BLE_LL_CFG_FEAT_LL_SCA_UPDATE (0)    /* yml:354-357 */
#define MYNEWT_VAL_BLE_LL_CFG_FEAT_LL_ENHANCED_CONN_UPDATE (0) /* yml:361-366 = BLE_CONN_SUBRATING = 0 */
#define MYNEWT_VAL_BLE_LL_ADV_CODING_SELECTION (0)      /* yml:370-373 */
#define MYNEWT_VAL_BLE_LL_PERIODIC_ADV_SYNC_BIGINFO_REPORTS (0) /* yml:377-382 */
#define MYNEWT_VAL_BLE_LL_SCAN_ACTIVE_SCAN_NRPA (0)     /* yml:390-396 = LL_PRIVACY = 0 */
#define MYNEWT_VAL_BLE_LL_PUBLIC_DEV_ADDR (0x000000000000ULL) /* yml:400-406 */
#define MYNEWT_VAL_BLE_LL_DTM_EXTENSIONS (0)            /* yml:412-429 */
#define MYNEWT_VAL_BLE_LL_HCI_VS (0)                    /* yml:431-434 = BLE_HCI_VS = 0 */
#define MYNEWT_VAL_BLE_LL_HCI_VS_CONN_STRICT_SCHED (0)  /* yml:435-438 */
#define MYNEWT_VAL_BLE_LL_HCI_VS_LOCAL_IRK (0)          /* yml:441-449 */
#define MYNEWT_VAL_BLE_LL_HCI_VS_SET_SCAN_CFG (0)       /* yml:452-458 */
#define MYNEWT_VAL_BLE_LL_HCI_VS_EVENT_ON_ASSERT (0)    /* yml:465-470 default 0 (vals block yml:696 only for !BLE_HOST) */
#define MYNEWT_VAL_BLE_LL_EXT_ADV_ADVA_IN_AUX (1)       /* yml:668-672 (inert while EXT_ADV=0) */
#define MYNEWT_VAL_BLE_LL_STACK_SIZE (120)              /* yml:674-677 (200 only with ISO broadcaster, yml:691) */
#define MYNEWT_VAL_BLE_LL_PHY (1)                       /* yml def: 1 (forced by yml:679 block too since 2M/coded off: keep LL PHY code, controller reports 1M only) */
#define MYNEWT_VAL_BLE_LL_CHANNEL_SOUNDING (0)          /* = BLE_CHANNEL_SOUNDING = 0 */
#define MYNEWT_VAL_BLE_LL_ISO (0)                       /* yml:519-527 = BLE_ISO = 0 */
#define MYNEWT_VAL_BLE_LL_ISO_BROADCASTER (0)           /* yml:528-535 default 0 */
#define MYNEWT_VAL_BLE_LL_ISO_HCI_FEEDBACK_INTERVAL_MS (0) /* yml:537-551 */
#define MYNEWT_VAL_BLE_LL_ISO_HCI_DISCARD_THRESHOLD (0)
#define MYNEWT_VAL_BLE_LL_ISOAL_MUX_PREFILL (0)
#define MYNEWT_VAL_BLE_LL_EXT (0)

/* FEM: none on CH32V208 (controller/syscfg.yml:472-517). */
#define MYNEWT_VAL_BLE_FEM_PA (0)
#define MYNEWT_VAL_BLE_FEM_PA_GAIN (0)
#define MYNEWT_VAL_BLE_FEM_PA_GAIN_TUNABLE (0)
#define MYNEWT_VAL_BLE_FEM_LNA (0)
#define MYNEWT_VAL_BLE_FEM_LNA_GAIN (0)
#define MYNEWT_VAL_BLE_FEM_LNA_GAIN_TUNABLE (0)
#define MYNEWT_VAL_BLE_FEM_ANTENNA (0)

/* --- Misc values referenced by compiled sources but absent from the -------
 * --- vendored syscfg.h (all at their "disabled" defaults).                 */
#define MYNEWT_VAL_BLE_GATT_DISC_CHRS_UUID (0)   /* nimble_opt_auto.h:63 #if */
#define MYNEWT_VAL_BLE_AUDIO_MAX_CODEC_RECORDS (0) /* ble_hs.c #if */
#define MYNEWT_VAL_BLE_LL_HBD_FAKE_DUAL_MODE (0) /* ble_ll_hci.c #if */
#define MYNEWT_VAL_TRNG (0)                      /* ble_ll_rand.c / ble_sm_alg.c: use HCI LE Rand via ble_hs_hci_rand */
#define MYNEWT_VAL_SELFTEST (0)                  /* ble_hs.c / ble_sm_alg.c test hooks */

#endif /* H_SYSCFG_OVERRIDES_WCH_ */
