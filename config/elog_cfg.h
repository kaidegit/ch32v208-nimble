/*
 * elog_cfg.h - EasyLogger configuration for CH32V208 (RAM-lean).
 *
 * This header shadows third_party/easylogger/inc/elog_cfg.h: elog.h pulls
 * in <elog_cfg.h> and config/ sits ahead of easylogger/inc on every include
 * path in CMakeLists.txt. Upstream defaults (1024-byte line buffer, colors,
 * pthread async, buffered mode) are deliberately NOT used:
 *
 *   - async/buffered modes are off: this project already decouples producers
 *     from the UART with its own DMA ring (port/console_uart.c); stacking
 *     EasyLogger's async ring on top would double the RAM and add a consumer
 *     thread for nothing;
 *   - colors are off (raw serial capture, ANSI noise costs bytes);
 *   - the line buffer is sized for "[<L>][<tag>] " + app records; the long
 *     NimBLE hex-dump lines never pass through elog (the NPL sink streams
 *     them straight into the DMA ring);
 *   - filter arrays trimmed: runtime tag/kw filtering is compiled in but
 *     small (default filter accepts everything; tags here are short).
 */
#ifndef _ELOG_CFG_H_
#define _ELOG_CFG_H_

/* enable log output. */
#define ELOG_OUTPUT_ENABLE
/* static output log level: keep DEBUG records (e.g. PHY traces). */
#define ELOG_OUTPUT_LVL                          ELOG_LVL_DEBUG
/* assert checking is done by FreeRTOS configASSERT + the panic writer. */
/* #undef ELOG_ASSERT_ENABLE */
/* buffer size for every line's log */
#define ELOG_LINE_BUF_SIZE                       192
/* output line number max length */
#define ELOG_LINE_NUM_MAX_LEN                    5
/* output filter's tag max length */
#define ELOG_FILTER_TAG_MAX_LEN                  16
/* output filter's keyword max length */
#define ELOG_FILTER_KW_MAX_LEN                   8
/* output filter's tag level max num */
#define ELOG_FILTER_TAG_LVL_MAX_NUM              4
/* output newline sign */
#define ELOG_NEWLINE_SIGN                        "\n"

/* no log color */
/* #undef ELOG_COLOR_ENABLE */

/* log format: level + tag only; set per-level at runtime in wch_log_init()
 * (ELOG_FMT_LVL | ELOG_FMT_TAG). Time/dir/func/line are off to keep records
 * short at 921600 baud; __FILE__ on this toolchain is a long absolute path. */

/* no asynchronous output mode (own DMA ring instead) */
/* #undef ELOG_ASYNC_OUTPUT_ENABLE */

/* no buffered output mode */
/* #undef ELOG_BUF_OUTPUT_ENABLE */

#endif /* _ELOG_CFG_H_ */
