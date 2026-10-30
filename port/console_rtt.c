/*
 * console_rtt.c - console facade + NPL log sink on SEGGER RTT channel 0.
 *
 * Two consumers share this file:
 *   1. the "console/console.h" facade required by the NimBLE host
 *      (nimble/host/src/ble_gatts_lcl.c: console_printf),
 *   2. the NPL log backend: config/nimble/nimble_npl_os_log.h generates
 *      BLE_HS_LOG_INFO() & co. as static inline wrappers around
 *      ble_npl_log_rtt_put(); each logical line gets one "[I] " prefix.
 *      Chain: BLE_HS_LOG(lvl,...) -> BLE_HS_LOG_<lvl>() -> RTT.
 *
 * Formatting goes through newlib-nano vsnprintf() (pure formatting, no
 * syscall layer involved) and the composed buffer is pushed with
 * SEGGER_RTT_WriteString, so this file only needs SEGGER_RTT.h/c.
 */

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "SEGGER_RTT.h"
#include "console/console.h"

#define CONSOLE_RTT_CH          (0)
#define CONSOLE_LINE_MAX        (256)

/* console/console.h */

void
console_write(const void *data, size_t len)
{
    if (data && len) {
        (void)SEGGER_RTT_Write(CONSOLE_RTT_CH, data, len);
    }
}

int
console_printf(const char *fmt, ...)
{
    char buf[CONSOLE_LINE_MAX];
    va_list args;
    int n;

    va_start(args, fmt);
    n = vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    if (n > 0) {
        if ((size_t)n >= sizeof(buf)) {
            n = sizeof(buf) - 1;    /* truncated; buf is NUL-terminated */
        }
        (void)SEGGER_RTT_Write(CONSOLE_RTT_CH, buf, (unsigned)n);
    }
    return n;
}

int
console_init(void)
{
    SEGGER_RTT_Init();
    return 0;
}

void
console_flush(void)
{
    /* RTT is memory-read by the debugger; nothing to flush. */
}

/* NPL log backend (config/nimble/nimble_npl_os_log.h) */

void
ble_npl_log_rtt_put(char lvl, const char *fmt, va_list args)
{
    char buf[CONSOLE_LINE_MAX];
    static bool line_start = true;
    int n;

    /* NimBLE emits ACL headers, individual hex bytes and the final newline
     * in separate calls. Respect that stream instead of terminating every
     * fragment. Long hex dumps can span many calls without extra prefixes. */
    n = vsnprintf(buf, sizeof(buf), fmt, args);
    if (n <= 0) {
        return;
    }
    if ((size_t)n >= sizeof(buf)) {
        n = sizeof(buf) - 1;
    }
    SEGGER_RTT_LOCK();
    for (int pos = 0; pos < n;) {
        if (line_start) {
            const char prefix[] = {'[', lvl, ']', ' '};
            (void)SEGGER_RTT_WriteNoLock(CONSOLE_RTT_CH, prefix, sizeof prefix);
        }
        int end = pos;
        while (end < n && buf[end++] != '\n') {
        }
        line_start = buf[end - 1] == '\n';
        (void)SEGGER_RTT_WriteNoLock(CONSOLE_RTT_CH, buf + pos, (unsigned)(end - pos));
        pos = end;
    }
    SEGGER_RTT_UNLOCK();
}
