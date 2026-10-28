/********************************** (C) COPYRIGHT *******************************
 * File Name          : system_ch32v20x.h
 * Description        : CH32V20x system/clock declarations.
 *
 * Lives in config/ (which is on every target's include path) because
 * third_party/sdk/Peripheral/inc/ch32v20x.h does
 *   #include "system_ch32v20x.h"
 * and the freertos_kernel library is built without the SDK include dirs.
 *********************************************************************************
 * Based on the WCH EVT BLE/RF_PHY example header.
 *******************************************************************************/

#ifndef __SYSTEM_CH32V20x_H
#define __SYSTEM_CH32V20x_H

#ifdef __cplusplus
extern "C" {
#endif

extern uint32_t SystemCoreClock; /* System Clock Frequency (Core Clock) */

/* System_Exported_Functions */
extern void SystemInit(void);
extern void SystemCoreClockUpdate(void);

#ifdef __cplusplus
}
#endif

#endif /*__SYSTEM_CH32V20x_H */
