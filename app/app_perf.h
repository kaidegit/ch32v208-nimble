#ifndef APP_PERF_H
#define APP_PERF_H
#include "host/ble_hs.h"
#include "host/ble_l2cap.h"
extern uint16_t app_perf_control_handle, app_perf_data_handle;
void app_perf_init(void);
int app_perf_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt);
void app_perf_gap(struct ble_gap_event *event);
void app_perf_coc_connected(struct ble_l2cap_chan *chan);
void app_perf_coc_disconnected(void);
int app_perf_coc_receive(struct ble_l2cap_chan *chan, struct os_mbuf *sdu);
void app_perf_coc_unstalled(int status);
#endif
