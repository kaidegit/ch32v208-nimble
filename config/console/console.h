/*
 * console/console.h - minimal console facade for NimBLE on CH32V208.
 *
 * Lives in config/ so that BOTH nimble_core (which includes this header from
 * nimble/host/src/ble_gatts_lcl.c) and wch_ble see it through the shared
 * config/ include path.  The implementation is port/console_rtt.c
 * (SEGGER RTT channel 0).
 *
 * API surface kept to what the stack actually uses (grep across
 * nimble/host/src + nimble/controller/src): only console_printf()
 * (ble_gatts_lcl.c; ble_ll_ctrl.c uses it under an #undef'd debug flag).
 * console_write()/console_flush() are provided for app convenience -
 * 宁少勿多, so nothing else is invented.
 */
#ifndef H_CONSOLE_WCH_
#define H_CONSOLE_WCH_

#include <stdarg.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Backend sink implemented in port/console_rtt.c.  Exposed so the NPL log
 * shim (config/nimble/nimble_npl_os_log.h) can share the same RTT path. */
void console_write(const void *data, size_t len);
int  console_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* No-ops kept for source compatibility with app code using the mynewt
 * console API; RTT needs neither setup nor flushing. */
int  console_init(void);
void console_flush(void);

#ifdef __cplusplus
}
#endif

#endif /* H_CONSOLE_WCH_ */
