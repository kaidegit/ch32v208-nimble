/*
 * ch32v20x_conf.h - trimmed peripheral-library configuration.
 *
 * Only the blocks this project actually uses (RCC/GPIO/TIM/MISC/FLASH/
 * USART/DMA); all *_it / debug (SDI printf) usage removed.  Headers are
 * pulled with paths relative to config/ so the file also resolves inside
 * library targets (freertos_kernel/logger) that are built without the SDK
 * include directories.
 *
 * Interrupt handlers live in app/ch32v20x_it.c (NOT declared here).
 */

#ifndef __CH32V20x_CONF_H
#define __CH32V20x_CONF_H

#include "../third_party/sdk/Peripheral/inc/ch32v20x_rcc.h"
#include "../third_party/sdk/Peripheral/inc/ch32v20x_gpio.h"
#include "../third_party/sdk/Peripheral/inc/ch32v20x_tim.h"
#include "../third_party/sdk/Peripheral/inc/ch32v20x_misc.h"
#include "../third_party/sdk/Peripheral/inc/ch32v20x_flash.h"
#include "../third_party/sdk/Peripheral/inc/ch32v20x_usart.h"
#include "../third_party/sdk/Peripheral/inc/ch32v20x_dma.h"

#endif /* __CH32V20x_CONF_H */
