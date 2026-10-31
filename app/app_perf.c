/* Saturated one-way test. Control/status travels over GATT; bulk data does not
 * wait for application acknowledgments. Receivers validate sequence/pattern. */
#include <string.h>
#include "FreeRTOS.h"
#include "task.h"
#include "console/console.h"
#include "nimble/nimble_port.h"
#include "host/ble_gatt.h"
#include "os/os_cputime.h"
#include "controller/ble_ll_conn.h"
#include "app_perf.h"

uint16_t app_perf_control_handle, app_perf_data_handle;
static struct ble_npl_callout pump;
static struct ble_l2cap_chan *coc;
static uint16_t conn, size;
static uint8_t transport, direction, running, done, subscribed, stalled;
static uint32_t bytes, packets, errors, start_us, data_end_us, duration_ms, retries;
static uint8_t buffer[512];
extern void ble_phy_wch_perf_snapshot(uint32_t out[12]);
static uint32_t phy_base[12], blocked_cycles, min_free;
static uint32_t max_ll_queue;

static uint8_t pattern(uint32_t offset) {
    return (uint8_t)(offset * 17u + offset / 251u);
}
static void put32(uint8_t *p, uint32_t n) {
    p[0]=n; p[1]=n>>8; p[2]=n>>16; p[3]=n>>24;
}
static uint32_t get32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1]<<8 | (uint32_t)p[2]<<16 | (uint32_t)p[3]<<24;
}
static uint32_t elapsed(void) {
    return (uint32_t)(os_cputime_get32()-start_us)/1000;
}
static void generate(void) {
    unsigned i;
    if (transport == 1) {
        put32(buffer, packets);
        for (i=4;i<size;i++) buffer[i] = (uint8_t)(packets*131u+i*17u);
    } else {
        for (i=0;i<size;i++) buffer[i]=pattern(bytes+i);
    }
}
static void receive(struct os_mbuf *om) {
    uint16_t n=OS_MBUF_PKTLEN(om), off=0, take, i;
    uint8_t bad=0;
    if (transport==1) {
        if (n!=size || n>sizeof buffer || os_mbuf_copydata(om,0,n,buffer)!=0) bad=1;
        else {
            if (get32(buffer)!=packets) bad=1;
            for (i=4;i<n;i++) if(buffer[i]!=(uint8_t)(packets*131u+i*17u)) bad=1;
        }
    } else {
        while(off<n) {
            take=n-off;
            if(take>sizeof buffer) take=sizeof buffer;
            if(os_mbuf_copydata(om,off,take,buffer)!=0) { bad=1;break; }
            for(i=0;i<take;i++) if(buffer[i]!=pattern(bytes+off+i)) bad=1;
            off+=take;
        }
    }
    errors+=bad; bytes+=n; packets++; data_end_us=os_cputime_get32();
}
static void pump_cb(struct ble_npl_event *event) {
    int rc, budget=8;
    struct os_mbuf *om;
    if(!running || direction!=2) return;
    if(elapsed()>=duration_ms) { running=0;done=1;return; }
    if((uint32_t)os_msys_num_free()<min_free) min_free=os_msys_num_free();
    if(stalled || os_msys_num_free()<(transport==1?5:9)) blocked_cycles++;
    while(budget-- && !stalled && os_msys_num_free()>=(transport==1?5:9)) {
        generate();
        om=ble_hs_mbuf_from_flat(buffer,size);
        if(!om) { retries++;break; }
        if(transport==1) rc=ble_gatts_notify_custom(conn,app_perf_data_handle,om);
        else {
            rc=ble_l2cap_send(coc,om);
            if(rc==BLE_HS_EBUSY || rc==BLE_HS_EBADDATA) os_mbuf_free_chain(om);
        }
        if(rc==0 || rc==BLE_HS_ESTALLED) {
            bytes+=size;packets++;data_end_us=os_cputime_get32();
            taskENTER_CRITICAL();
            struct ble_ll_conn_sm *link=ble_ll_conn_find_by_handle(conn);
            if(link && link->conn_txq_num_data_pkt>max_ll_queue) max_ll_queue=link->conn_txq_num_data_pkt;
            taskEXIT_CRITICAL();
            if(rc==BLE_HS_ESTALLED) stalled=1;
        } else { errors++; running=0;done=1;break; }
    }
    if(running) ble_npl_callout_reset(&pump,1);
}
void app_perf_init(void) {
    ble_npl_callout_init(&pump,nimble_port_get_dflt_eventq(),pump_cb,NULL);
}
int app_perf_access(uint16_t handle, uint16_t attr, struct ble_gatt_access_ctxt *ctxt) {
    uint8_t data[100]={4};
    uint32_t phy[12];
    struct ble_ll_conn_sm *link;
    uint16_t n;
    if(attr==app_perf_data_handle) {
        if(ctxt->op!=BLE_GATT_ACCESS_OP_WRITE_CHR || !running || transport!=1 || direction!=1) return BLE_ATT_ERR_UNLIKELY;
        receive(ctxt->om); return 0;
    }
    if(ctxt->op==BLE_GATT_ACCESS_OP_READ_CHR) {
        data[1]=transport; data[2]=direction; data[3]=running;data[4]=done;
        data[5]=size;data[6]=size>>8;
        put32(data+8,bytes);put32(data+12,packets);put32(data+16,errors);
        put32(data+20,(uint32_t)(data_end_us-start_us)/1000);
        put32(data+24,retries);put32(data+28,os_msys_num_free());
        taskENTER_CRITICAL();
        ble_phy_wch_perf_snapshot(phy);
        link=ble_ll_conn_find_by_handle(handle);
        if(link) {
            data[88]=link->eff_max_tx_octets;data[89]=link->eff_max_rx_octets;
            data[90]=link->eff_max_tx_time;data[91]=link->eff_max_tx_time>>8;
            data[92]=link->eff_max_rx_time;data[93]=link->eff_max_rx_time>>8;
            data[94]=link->conn_itvl;data[95]=link->conn_itvl>>8;
        }
        taskEXIT_CRITICAL();
        for(unsigned i=0;i<12;i++) put32(data+32+i*4,phy[i]-phy_base[i]);
        put32(data+80,blocked_cycles);put32(data+84,min_free);
        put32(data+96,max_ll_queue);
        return os_mbuf_append(ctxt->om,data,sizeof data)==0?0:BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    if(ble_hs_mbuf_to_flat(ctxt->om,data,12,&n)!=0 || n!=12 || data[0]!=1) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    if(data[3]==2) {
        struct ble_gap_upd_params params={0};
        params.itvl_min=6;params.itvl_max=12;params.supervision_timeout=200;
        int rc=ble_gap_update_params(handle,&params);
        console_printf("perf: request CI 7.5..15ms rc=%d\n",rc);
        return 0; /* Central decides; final status reports the actual CI. */
    }
    if(data[3]==0) { running=0;done=1;ble_npl_callout_stop(&pump);return 0; }
    if(data[3]!=1 || data[1]<1 || data[1]>2 || data[2]<1 || data[2]>2 || running) return BLE_ATT_ERR_UNLIKELY;
    n=(uint16_t)data[4]|(uint16_t)data[5]<<8;
    if(n<(data[1]==1?4:1) || n>(data[1]==1?244:512) || (data[1]==2&&!coc) ||
       (data[1]==1&&data[2]==2&&!subscribed) || get32(data+8)==0) return BLE_ATT_ERR_UNLIKELY;
    transport=data[1];direction=data[2];size=n;conn=handle;duration_ms=get32(data+8);
    bytes=packets=errors=retries=stalled=done=0;running=1;start_us=data_end_us=os_cputime_get32();
    blocked_cycles=max_ll_queue=0;min_free=os_msys_num_free();
    taskENTER_CRITICAL();
    ble_phy_wch_perf_snapshot(phy_base);
    taskEXIT_CRITICAL();
    console_printf("perf: start transport=%u direction=%u size=%u duration=%lu\n",transport,direction,size,(unsigned long)duration_ms);
    if(direction==2) ble_npl_callout_reset(&pump,1);
    return 0;
}
void app_perf_gap(struct ble_gap_event *event) {
    if(event->type==BLE_GAP_EVENT_SUBSCRIBE && event->subscribe.attr_handle==app_perf_data_handle) subscribed=event->subscribe.cur_notify;
    if(event->type==BLE_GAP_EVENT_DISCONNECT) {
        console_printf("perf: end bytes=%lu packets=%lu errors=%lu free=%d\n",(unsigned long)bytes,(unsigned long)packets,(unsigned long)errors,os_msys_num_free());
        running=subscribed=0;ble_npl_callout_stop(&pump);coc=NULL;
    }
}
void app_perf_coc_connected(struct ble_l2cap_chan *chan) { coc=chan; }
void app_perf_coc_disconnected(void) { coc=NULL; if(transport==2) {running=0;ble_npl_callout_stop(&pump);} }
int app_perf_coc_receive(struct ble_l2cap_chan *chan, struct os_mbuf *sdu) {
    if(!running || transport!=2 || direction!=1) return 0;
    receive(sdu);os_mbuf_free_chain(sdu);
    sdu=os_msys_get_pkthdr(0,0);
    if(!sdu || ble_l2cap_recv_ready(chan,sdu)!=0) {errors++;ble_l2cap_disconnect(chan);}
    return 1;
}
void app_perf_coc_unstalled(int status) { stalled=0; if(status) {errors++;running=0;done=1;} }
