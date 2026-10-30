/*
 * nimble_npl_os_log.h - NPL log backend for CH32V208 (SEGGER RTT ch0).
 *
 * This file shadows porting/npl/freertos/include/nimble/nimble_npl_os_log.h:
 * nimble/nimble_npl_log.h includes it as "nimble/nimble_npl_os_log.h" and the
 * quoted-include search falls through to the -I list, where config/ comes
 * first.  The stock FreeRTOS version routes logs to vprintf() (newlib
 * stdout); we route them to RTT so no _write() syscall path is involved.
 *
 * Every log call site compiles to
 *     BLE_HS_LOG(INFO, "fmt\n", ...)   ==   BLE_HS_LOG_INFO("fmt\n", ...)
 * which becomes a static inline calling ble_npl_log_rtt_put('I', fmt, ap).
 * Output line format: "[I] <message>\n". The caller supplies newlines; a
 * logical line may consist of multiple calls (notably ACL hex dumps).
 */
#ifndef _NIMBLE_NPL_OS_LOG_WCH_H_
#define _NIMBLE_NPL_OS_LOG_WCH_H_

#include <stdarg.h>

/* Implemented in port/console_rtt.c */
void ble_npl_log_rtt_put(char lvl, const char *fmt, va_list args);

#define _BLE_NPL_LOG_LVL_CHR(lvl)   _BLE_NPL_LOG_LVL_CHR_##lvl
#define _BLE_NPL_LOG_LVL_CHR_DEBUG    'D'
#define _BLE_NPL_LOG_LVL_CHR_INFO     'I'
#define _BLE_NPL_LOG_LVL_CHR_WARN     'W'
#define _BLE_NPL_LOG_LVL_CHR_ERROR    'E'
#define _BLE_NPL_LOG_LVL_CHR_CRITICAL 'C'

/* Same shape as the stock FreeRTOS implementation, but RTT-backed. */
#define BLE_NPL_LOG_IMPL(lvl)                                           \
        static inline void _BLE_NPL_LOG_CAT(BLE_NPL_LOG_MODULE,         \
                _BLE_NPL_LOG_CAT(_, lvl))(const char *fmt, ...)         \
        {                                                               \
            va_list args;                                               \
            va_start(args, fmt);                                        \
            ble_npl_log_rtt_put(_BLE_NPL_LOG_LVL_CHR(lvl), fmt, args);  \
            va_end(args);                                               \
        }

#endif  /* _NIMBLE_NPL_OS_LOG_WCH_H_ */
