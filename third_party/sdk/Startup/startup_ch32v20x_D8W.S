;/********************************** (C) COPYRIGHT *******************************
;* File Name          : startup_ch32v20x_D8W.s
;* Author             : WCH
;* Version            : V1.0.2
;* Date               : 2025/10/21
;* Description        : CH32V208x
;*                      Startup File
;*********************************************************************************
;* Copyright (c) 2021 Nanjing Qinheng Microelectronics Co., Ltd.
;* Attention: This software (modified or not) and binary are used for 
;* microcontroller manufactured by Nanjing Qinheng Microelectronics.
*******************************************************************************/

	.section	.init,"ax",@progbits
	.global	_start
	.align	1
_start:
	j	handle_reset

    .section    .vector,"ax",@progbits
    .align  1
_vector_base:
    .option norvc;
    .word   _start
    .word   wch_exc_trap               /* exc 1 - was 0: any fault here jumped to 0 */
    .word   NMI_Handler                /* NMI */
    .word   wch_exc_trap               /* Hard Fault slot: raw trap while hunting the wild jump */
    .word   wch_exc_trap               /* exc 4: load/store access fault -> mepc captured */
    .word   Ecall_M_Mode_Handler       /* Ecall M Mode */
    .word   wch_exc_trap               /* exc 6 */
    .word   wch_exc_trap               /* exc 7 */
    .word   Ecall_U_Mode_Handler       /* Ecall U Mode */
    .word   Break_Point_Handler        /* Break Point */
    .word   wch_exc_trap               /* exc 10 */
    .word   wch_exc_trap               /* exc 11 */
    .word   SysTick_Handler            /* SysTick */
    .word   wch_exc_trap               /* exc 13 */
    .word   SW_Handler                 /* SW */
    .word   wch_exc_trap               /* exc 15 */
    /* External Interrupts */
    .word   WWDG_IRQHandler            /* Window Watchdog */
    .word   PVD_IRQHandler             /* PVD through EXTI Line detect */
    .word   TAMPER_IRQHandler          /* TAMPER */
    .word   RTC_IRQHandler             /* RTC */
    .word   FLASH_IRQHandler           /* Flash */
    .word   RCC_IRQHandler             /* RCC */
    .word   EXTI0_IRQHandler           /* EXTI Line 0 */
    .word   EXTI1_IRQHandler           /* EXTI Line 1 */
    .word   EXTI2_IRQHandler           /* EXTI Line 2 */
    .word   EXTI3_IRQHandler           /* EXTI Line 3 */
    .word   EXTI4_IRQHandler           /* EXTI Line 4 */
    .word   DMA1_Channel1_IRQHandler   /* DMA1 Channel 1 */
    .word   DMA1_Channel2_IRQHandler   /* DMA1 Channel 2 */
    .word   DMA1_Channel3_IRQHandler   /* DMA1 Channel 3 */
    .word   DMA1_Channel4_IRQHandler   /* DMA1 Channel 4 */
    .word   DMA1_Channel5_IRQHandler   /* DMA1 Channel 5 */
    .word   DMA1_Channel6_IRQHandler   /* DMA1 Channel 6 */
    .word   DMA1_Channel7_IRQHandler   /* DMA1 Channel 7 */
    .word   ADC1_2_IRQHandler          /* ADC1_2 */
    .word   USB_HP_CAN1_TX_IRQHandler  /* USB HP and CAN1 TX */
    .word   USB_LP_CAN1_RX0_IRQHandler /* USB LP and CAN1RX0 */
    .word   CAN1_RX1_IRQHandler        /* CAN1 RX1 */
    .word   CAN1_SCE_IRQHandler        /* CAN1 SCE */
    .word   EXTI9_5_IRQHandler         /* EXTI Line 9..5 */
    .word   TIM1_BRK_IRQHandler        /* TIM1 Break */
    .word   TIM1_UP_IRQHandler         /* TIM1 Update */
    .word   TIM1_TRG_COM_IRQHandler    /* TIM1 Trigger and Commutation */
    .word   TIM1_CC_IRQHandler         /* TIM1 Capture Compare */
    .word   TIM2_IRQHandler            /* TIM2 */
    .word   TIM3_IRQHandler            /* TIM3 */
    .word   TIM4_IRQHandler            /* TIM4 */
    .word   I2C1_EV_IRQHandler         /* I2C1 Event */
    .word   I2C1_ER_IRQHandler         /* I2C1 Error */
    .word   I2C2_EV_IRQHandler         /* I2C2 Event */
    .word   I2C2_ER_IRQHandler         /* I2C2 Error */
    .word   SPI1_IRQHandler            /* SPI1 */
    .word   SPI2_IRQHandler            /* SPI2 */
    .word   USART1_IRQHandler          /* USART1 */
    .word   USART2_IRQHandler          /* USART2 */
    .word   USART3_IRQHandler          /* USART3 */
    .word   EXTI15_10_IRQHandler       /* EXTI Line 15..10 */
    .word   RTCAlarm_IRQHandler        /* RTC Alarm through EXTI Line */
    .word   USBWakeUp_IRQHandler       /* USB Wake up from suspend */
    .word   USBFS_IRQHandler           /* USBFS Break */
    .word   USBFSWakeUp_IRQHandler     /* USBFS Wake up from suspend */
    .word   ETH_IRQHandler             /* ETH global */
    .word   ETHWakeUp_IRQHandler       /* ETH Wake up */
    .word   BB_IRQHandler              /* BLE BB */
    .word   LLE_IRQHandler             /* BLE LLE */
    .word   TIM5_IRQHandler            /* TIM5 */
    .word   UART4_IRQHandler           /* UART4 */
    .word   DMA1_Channel8_IRQHandler   /* DMA1 Channel8 */
    .word   OSC32KCal_IRQHandler       /* OSC32KCal */
    .word   OSCWakeUp_IRQHandler       /* OSC Wake Up */

    .option rvc;
    .section    .text.vector_handler, "ax", @progbits
    .weak   NMI_Handler                /* NMI */
    .weak   HardFault_Handler          /* Hard Fault */
    .weak   Ecall_M_Mode_Handler       /* Ecall M Mode */
    .weak   Ecall_U_Mode_Handler       /* Ecall U Mode */
    .weak   Break_Point_Handler        /* Break Point */
    .weak   SysTick_Handler            /* SysTick */
    .weak   SW_Handler                 /* SW */
    .weak   WWDG_IRQHandler            /* Window Watchdog */
    .weak   PVD_IRQHandler             /* PVD through EXTI Line detect */
    .weak   TAMPER_IRQHandler          /* TAMPER */
    .weak   RTC_IRQHandler             /* RTC */
    .weak   FLASH_IRQHandler           /* Flash */
    .weak   RCC_IRQHandler             /* RCC */
    .weak   EXTI0_IRQHandler           /* EXTI Line 0 */
    .weak   EXTI1_IRQHandler           /* EXTI Line 1 */
    .weak   EXTI2_IRQHandler           /* EXTI Line 2 */
    .weak   EXTI3_IRQHandler           /* EXTI Line 3 */
    .weak   EXTI4_IRQHandler           /* EXTI Line 4 */
    .weak   DMA1_Channel1_IRQHandler   /* DMA1 Channel 1 */
    .weak   DMA1_Channel2_IRQHandler   /* DMA1 Channel 2 */
    .weak   DMA1_Channel3_IRQHandler   /* DMA1 Channel 3 */
    .weak   DMA1_Channel4_IRQHandler   /* DMA1 Channel 4 */
    .weak   DMA1_Channel5_IRQHandler   /* DMA1 Channel 5 */
    .weak   DMA1_Channel6_IRQHandler   /* DMA1 Channel 6 */
    .weak   DMA1_Channel7_IRQHandler   /* DMA1 Channel 7 */
    .weak   ADC1_2_IRQHandler          /* ADC1_2 */
    .weak   USB_HP_CAN1_TX_IRQHandler  /* USB HP and CAN1 TX */
    .weak   USB_LP_CAN1_RX0_IRQHandler /* USB LP and CAN1RX0 */
    .weak   CAN1_RX1_IRQHandler        /* CAN1 RX1 */
    .weak   CAN1_SCE_IRQHandler        /* CAN1 SCE */
    .weak   EXTI9_5_IRQHandler         /* EXTI Line 9..5 */
    .weak   TIM1_BRK_IRQHandler        /* TIM1 Break */
    .weak   TIM1_UP_IRQHandler         /* TIM1 Update */
    .weak   TIM1_TRG_COM_IRQHandler    /* TIM1 Trigger and Commutation */
    .weak   TIM1_CC_IRQHandler         /* TIM1 Capture Compare */
    .weak   TIM2_IRQHandler            /* TIM2 */
    .weak   TIM3_IRQHandler            /* TIM3 */
    .weak   TIM4_IRQHandler            /* TIM4 */
    .weak   I2C1_EV_IRQHandler         /* I2C1 Event */
    .weak   I2C1_ER_IRQHandler         /* I2C1 Error */
    .weak   I2C2_EV_IRQHandler         /* I2C2 Event */
    .weak   I2C2_ER_IRQHandler         /* I2C2 Error */
    .weak   SPI1_IRQHandler            /* SPI1 */
    .weak   SPI2_IRQHandler            /* SPI2 */
    .weak   USART1_IRQHandler          /* USART1 */
    .weak   USART2_IRQHandler          /* USART2 */
    .weak   USART3_IRQHandler          /* USART3 */
    .weak   EXTI15_10_IRQHandler       /* EXTI Line 15..10 */
    .weak   RTCAlarm_IRQHandler        /* RTC Alarm through EXTI Line */
    .weak   USBWakeUp_IRQHandler       /* USB Wakeup from suspend */
    .weak   USBFS_IRQHandler           /* USBFS */
    .weak   USBFSWakeUp_IRQHandler     /* USBFS Wake Up */
    .weak   ETH_IRQHandler             /* ETH global */
    .weak   ETHWakeUp_IRQHandler       /* ETHWakeUp */
    .weak   BB_IRQHandler              /* BLE BB */
    .weak   LLE_IRQHandler             /* BLE LLE */
    .weak   TIM5_IRQHandler            /* TIM5 */
    .weak   UART4_IRQHandler           /* UART4 */
    .weak   DMA1_Channel8_IRQHandler   /* DMA1 Channel8 */
    .weak   OSC32KCal_IRQHandler       /* OSC32 KCal */
    .weak   OSCWakeUp_IRQHandler       /* OSC Wake Up */

NMI_Handler:
HardFault_Handler:
Ecall_M_Mode_Handler:
Ecall_U_Mode_Handler:
Break_Point_Handler:
SysTick_Handler:
SW_Handler:
WWDG_IRQHandler:
PVD_IRQHandler:
TAMPER_IRQHandler:
RTC_IRQHandler:
FLASH_IRQHandler:
RCC_IRQHandler:
EXTI0_IRQHandler:
EXTI1_IRQHandler:
EXTI2_IRQHandler:
EXTI3_IRQHandler:
EXTI4_IRQHandler:
DMA1_Channel1_IRQHandler:
DMA1_Channel2_IRQHandler:
DMA1_Channel3_IRQHandler:
DMA1_Channel4_IRQHandler:
DMA1_Channel5_IRQHandler:
DMA1_Channel6_IRQHandler:
DMA1_Channel7_IRQHandler:
ADC1_2_IRQHandler:
USB_HP_CAN1_TX_IRQHandler:
USB_LP_CAN1_RX0_IRQHandler:
CAN1_RX1_IRQHandler:
CAN1_SCE_IRQHandler:
EXTI9_5_IRQHandler:
TIM1_BRK_IRQHandler:
TIM1_UP_IRQHandler:
TIM1_TRG_COM_IRQHandler:
TIM1_CC_IRQHandler:
TIM2_IRQHandler:
TIM3_IRQHandler:
TIM4_IRQHandler:
I2C1_EV_IRQHandler:
I2C1_ER_IRQHandler:
I2C2_EV_IRQHandler:
I2C2_ER_IRQHandler:
SPI1_IRQHandler:
SPI2_IRQHandler:
USART1_IRQHandler:
USART2_IRQHandler:
USART3_IRQHandler:
EXTI15_10_IRQHandler:
RTCAlarm_IRQHandler:
USBWakeUp_IRQHandler:
USBFS_IRQHandler:
USBFSWakeUp_IRQHandler:
ETH_IRQHandler:
ETHWakeUp_IRQHandler:
BB_IRQHandler:
LLE_IRQHandler:
TIM5_IRQHandler:
UART4_IRQHandler:
DMA1_Channel8_IRQHandler:
OSC32KCal_IRQHandler:
OSCWakeUp_IRQHandler:
1:
	/* Forensic trap shared by all unused-vector stubs: stash the machine
	 * state into fixed RAM (survives a wild sp - printf may not work) and
	 * park.  jal sets ra to the address INSIDE this shim's caller region,
	 * identifying which vector landed.  Read g_stub_trap_words via gdb. */
	csrr t0, mcause
	csrr t1, mepc
	csrr t2, mtval
	la   t3, g_stub_trap_words
	sw   t0, 0(t3)
	sw   t1, 4(t3)
	sw   t2, 8(t3)
	sw   ra, 12(t3)
	mv   t0, sp
	sw   t0, 16(t3)
	mv   t0, gp
	sw   t0, 20(t3)
	lw   a0, 0(t3)
	lw   a1, 4(t3)
	lw   a2, 8(t3)
	tail wch_stub_trap_report
2:	j 2b

/* Raw exception trap for the reserved exception slots: stash mcause/
 * mepc/mtval/sp/gp into g_exc_trap_words (fixed RAM) and spin.  No C
 * calls - sp may be wild.  Read g_exc_trap_words via gdb: mepc is the
 * address of the faulting instruction. */
	.section	.text.vector_handler, "ax", @progbits
	.globl	wch_exc_trap
wch_exc_trap:
	/* Full machine stash: g_exc_trap_words layout
	 *  0 mcause  1 mepc   2 mtval  3 mstatus  4 mscratch
	 *  5..35 x1..x31 (index = regnum+4; x0 skipped)
	 * t0/t1/t2/t3 are scratch registers clobbered by this shim;
	 * their slots contain shim values, not interrupted values.
	 * ra(x1) is the return address at trap time; the s-registers
	 * (x8,x9,x18..x27) fingerprint the function that was executing. */
	csrr t0, mcause
	csrr t1, mepc
	csrr t2, mtval
	la   t3, g_exc_trap_words
	sw   t0, 0(t3)
	sw   t1, 4(t3)
	sw   t2, 8(t3)
	csrr t0, mstatus
	sw   t0, 12(t3)
	csrr t0, mscratch
	sw   t0, 16(t3)
	mv   t0, sp
	sw   t0, 4*4+2*4(t3)
	mv   t0, gp
	sw   t0, 4*4+3*4(t3)
	mv   t0, tp
	sw   t0, 4*4+4*4(t3)
	sw   x1, 4*4+1*4(t3)
	sw   x5, 4*4+5*4(t3)
	sw   x6, 4*4+6*4(t3)
	sw   x7, 4*4+7*4(t3)
	sw   x8, 4*4+8*4(t3)
	sw   x9, 4*4+9*4(t3)
	sw   x10, 4*4+10*4(t3)
	sw   x11, 4*4+11*4(t3)
	sw   x12, 4*4+12*4(t3)
	sw   x13, 4*4+13*4(t3)
	sw   x14, 4*4+14*4(t3)
	sw   x15, 4*4+15*4(t3)
	sw   x16, 4*4+16*4(t3)
	sw   x17, 4*4+17*4(t3)
	sw   x18, 4*4+18*4(t3)
	sw   x19, 4*4+19*4(t3)
	sw   x20, 4*4+20*4(t3)
	sw   x21, 4*4+21*4(t3)
	sw   x22, 4*4+22*4(t3)
	sw   x23, 4*4+23*4(t3)
	sw   x24, 4*4+24*4(t3)
	sw   x25, 4*4+25*4(t3)
	sw   x26, 4*4+26*4(t3)
	sw   x27, 4*4+27*4(t3)
	sw   x28, 4*4+28*4(t3)
	sw   x29, 4*4+29*4(t3)
	sw   x30, 4*4+30*4(t3)
	sw   x31, 4*4+31*4(t3)
1:	j 1b

	.section	.text.handle_reset,"ax",@progbits
	.weak	handle_reset
	.align	1
handle_reset:
	/* Keep IRQs masked until the scheduler has valid task/ISR stacks.
	 * Debugger restarts can leave SysTick/PFIC state from the last run. */
	li t0, 0x1800
	csrw mstatus, t0
	li t0, 0xe000f000
	sw zero, 0(t0)
	sw zero, 4(t0)
.option push 
.option	norelax 
	la gp, __global_pointer$
.option	pop 

	la sp, _eusrstack 

/* Load data section from flash to RAM */
	la a0, _data_lma
	la a1, _data_vma
	la a2, _edata
	bgeu a1, a2, 2f
1:
	lw t0, (a0)
	sw t0, (a1)
	addi a0, a0, 4
	addi a1, a1, 4
	bltu a1, a2, 1b
2:
/* Clear bss section */
	la a0, _sbss
	la a1, _ebss
	bgeu a0, a1, 2f
1:
	sw zero, (a0)
	addi a0, a0, 4
	bltu a0, a1, 1b
2:
/* Configure pipelining and instruction prediction.  Bring-up A/B test:
 * the EVT default 0x1f (all prediction features on) coincides with the
 * wild-jump crash (mcause=0/mepc=0/ra=8/sp=0/gp=0x101 at tick 12-56);
 * 0x0 disables all of them.  If the crash disappears, this is a silicon
 * workaround candidate. */
    li t0, 0x0
    csrw 0xbc0, t0
/* INTSYSCR(0x804): 0x0 = interrupt nesting OFF + HPE hardware prologue
 * OFF.  The EVT default was 0x3 (both on).  Bring-up forensics found the
 * 2 KB ISR stack overflowing once multiple IRQ levels nested on the
 * mscratch-swapped ISR stack, spraying register frames into adjacent RAM
 * (heap/kernel-variable corruption - the entire "async writer" saga).
 * The FreeRTOS port serializes handlers fine without nesting (every
 * handler saves its own registers), so both features stay off. */
	li t0, 0x0
	csrw 0x804, t0
/* Return to main with IRQs masked; xPortStartFirstTask enables them. */
    li t0, 0x1800
   	csrw mstatus, t0
/* Configure the interrupt vector table recognition mode and entry address mode */
 	la t0, _vector_base
    ori t0, t0, 3           
	csrw mtvec, t0

    jal  SystemInit
	la t0, main
	csrw mepc, t0
	mret


