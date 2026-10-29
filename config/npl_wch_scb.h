/*
 * Map the upstream FreeRTOS NPL's SCB->ICSR test onto the QingKe V4
 * PFIC_GISR.GACTSTA live interrupt status (0xE000E04C, bit 8).
 * mcause is NOT a live ISR flag: it retains the last trap after mret.
 */
#ifndef H_NPL_WCH_SCB_H_
#define H_NPL_WCH_SCB_H_

#include <stdint.h>

typedef struct {
    volatile uint32_t ICSR;      /* PFIC_GISR */
} SCB_WCH_TypeDef;

#define SCB_ICSR_VECTACTIVE_Msk  (1UL << 8) /* PFIC_GISR.GACTSTA */
#define SCB ((SCB_WCH_TypeDef *)0xE000E04CUL)

#endif /* H_NPL_WCH_SCB_H_ */
