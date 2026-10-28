/*
 * nimble_npl.h - CH32V208 shim for the stock nimble_npl.h.
 *
 * porting/npl/freertos/src/npl_os_freertos.c probes ISR context with the ARM
 * CMSIS expression "SCB->ICSR" inside its static in_isr().  That file is the
 * canonical place for the platform to inject its SCB definition:
 *
 *     #ifdef NIMBLE_NPL_OS_EXTRA_INCLUDE
 *     #include NIMBLE_NPL_OS_EXTRA_INCLUDE
 *     #endif
 *
 * Nothing in the CMake build defines that macro (CMakeLists.txt is not to be
 * modified), so this shim - which shadows the real header because config/ is
 * the first -I directory - defines it and then defers to the stock header
 * with #include_next.  config/npl_wch_scb.h provides the SCB compile shim.
 *
 * The actual ISR-safe NPL behaviour is implemented in the shadowed
 * config/nimble/nimble_npl_os.h (see npl_wch_*() helpers there).
 */
#ifndef _NIMBLE_NPL_H_WCH_SHIM_
#define _NIMBLE_NPL_H_WCH_SHIM_

#define NIMBLE_NPL_OS_EXTRA_INCLUDE "npl_wch_scb.h"

#include_next <nimble/nimble_npl.h>

#endif /* _NIMBLE_NPL_H_WCH_SHIM_ */
