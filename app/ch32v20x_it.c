/********************************** (C) COPYRIGHT *******************************
 * File Name          : ch32v20x_it.c
 * Description        : Interrupt handlers for the CH32V208 NimBLE bring-up.
 *
 * Handler ownership:
 *  - SysTick_Handler / SW_Handler are NOT defined here: the WCH FreeRTOS
 *    RISC-V port owns them (port.c defines SysTick_Handler around
 *    xTaskIncrementTick(); portASM.S defines SW_Handler for context
 *    switch).  The startup vector table references them weakly, so the
 *    strong port definitions win.
 *  - TIM3_IRQHandler  -> hal_timer_wch_irq()  (port/hal_timer_wch.c)
 *  - BB_IRQHandler    -> wch_rf_bb_isr()      (drivers/wch_ble/wch_rf.c)
 *  - LLE_IRQHandler   -> wch_rf_lle_isr()     (drivers/wch_ble/wch_rf.c)
 *    The three callees are weak references so the boot image (which does
 *    not link the wch_ble library) still links; the branch is skipped
 *    while the callee address is NULL.
 *  - Everything else falls through to the startup file's weak
 *    spin-loop stubs.
 *
 * PFIC priority notes:
 *  - The QingKe V4 PFIC has a flat 4-bit priority field per IRQ
 *    (NVIC_SetPriority(IRQn, 0..15); numerically lower = more urgent,
 *    no Cortex-M style priority-group split).
 *  - The FreeRTOS port pins Software and SysTick at priority 0xF
 *    (lowest) so every peripheral/BLE ISR can preempt the kernel's
 *    tick and yield interrupts.
 *  - Enabling of BB/LLE/TIM3 in the PFIC is done by their drivers, not
 *    here; global interrupts (mstatus.MIE) are already enabled by the
 *    startup code (csrw mstatus, 0x1888) before main().
 *******************************************************************************/

#include <stdint.h>
#include "ch32v20x.h"
#include "wch_log.h"

/* Plain interrupt() attribute: the compiler emits a full software
 * prologue/epilogue.  The "WCH-Interrupt-fast" variant must only be used
 * with HPE enabled (CSR 0x804 INTSYSCR.HWSTKEN), which QingKe V4 leaves
 * OFF at reset and nothing in this project (or the WCH EVT examples)
 * enables - a fast handler with HPE off clobbers the interrupted
 * context's registers and corrupts RAM (observed as garbage writes into
 * FreeRTOS globals during bring-up).  Plain interrupt() is correct under
 * either HPE state (merely redundant if HPE ever gets turned on). */
void NMI_Handler( void ) __attribute__( ( interrupt ) );
void HardFault_Handler( void ) __attribute__( ( interrupt ) );

/* Peripheral/BLE vectors (owned by the drivers, weak-called from here). */
void TIM3_IRQHandler( void ) __attribute__( ( interrupt ) );
void BB_IRQHandler( void ) __attribute__( ( interrupt ) );
void LLE_IRQHandler( void ) __attribute__( ( interrupt ) );
void DMA1_Channel4_IRQHandler( void ) __attribute__( ( interrupt ) );

/* Bring-up forensics: nothing in this project enables the WWDG, yet the
 * core was found parked in its weak spin stub.  A strong handler turns
 * the silent spin into a full report: mcause bit31+code16 proves a real
 * WWDG interrupt (vs. a wild jump into the stub), and the PFIC status
 * words show which interrupt lines are asserted/active. */
void WWDG_IRQHandler( void ) __attribute__( ( interrupt ) );

/* Weak callees: real implementations arrive with the wch_ble library. */
extern void hal_timer_wch_irq( void ) __attribute__( ( weak ) );
extern void wch_rf_bb_isr( void ) __attribute__( ( weak ) );
extern void wch_rf_lle_isr( void ) __attribute__( ( weak ) );

/* Log ring DMA completion (port/console_uart.c; logger lib is linked into
 * both images, so a direct strong reference is fine). */
extern void wch_log_dma_irq( void );

/*********************************************************************
 * Shared reporter for the startup's unused-vector stubs.  The .S shim
 * stashes mcause/mepc/mtval + entry ra into g_stub_trap_words FIRST
 * (fixed RAM - survives a wild sp), then this best-effort print runs
 * and the core parks.  Read g_stub_trap_words via gdb when the print
 * did not make it out.
 *********************************************************************/
volatile uint32_t g_stub_trap_words[ 8 ];
volatile uint32_t g_exc_trap_words[ 40 ];    /* written by wch_exc_trap (.S) */
volatile uint32_t g_config_assert_line;

void wch_stub_trap_report( uint32_t mcause, uint32_t mepc, uint32_t mtval )
{
    g_stub_trap_words[ 0 ] = mcause;
    g_stub_trap_words[ 1 ] = mepc;
    g_stub_trap_words[ 2 ] = mtval;
    /* words 3-5 hold ra/sp/gp from the .S shim - do not clobber */
    g_stub_trap_words[ 6 ] = PFIC->IACTR[ 0 ];
    g_stub_trap_words[ 7 ] = PFIC->GISR;

    wch_log_panic( "\r\n[STUB-TRAP] mcause=%08x mepc=%08x mtval=%08x\r\n",
                   ( unsigned ) mcause, ( unsigned ) mepc, ( unsigned ) mtval );
    wch_log_panic( "[STUB-TRAP] iactr0=%08x gisr=%08x\r\n",
                   ( unsigned ) g_stub_trap_words[ 6 ], ( unsigned ) g_stub_trap_words[ 7 ] );
    for( ;; )
    {
    }
}

/*********************************************************************
 * NMI_Handler
 *********************************************************************/
void NMI_Handler( void )
{
    wch_log_panic( "\r\n[NMI] NMI taken, halting.\r\n" );
    for( ;; )
    {
    }
}

/*********************************************************************
 * HardFault_Handler: report mcause/mepc/mtval + the kernel-variable
 * neighborhood (pxCurrentTCB slot is the word the tick handler faults
 * on), then park.
 *********************************************************************/
extern void * volatile pxCurrentTCB;

void HardFault_Handler( void )
{
    uint32_t mcause, mepc, mtval;
    volatile uint32_t * p = ( volatile uint32_t * ) &pxCurrentTCB - 12;
    int i;

    __asm volatile( "csrr %0, mcause" : "=r"( mcause ) );
    __asm volatile( "csrr %0, mepc"   : "=r"( mepc ) );
    __asm volatile( "csrr %0, mtval"  : "=r"( mtval ) );

    wch_log_panic( "\r\n[HardFault] mcause=0x%08x mepc=0x%08x mtval=0x%08x\r\n",
                   ( unsigned ) mcause, ( unsigned ) mepc, ( unsigned ) mtval );
    wch_log_panic( "[HardFault] tcb@%08x=%08x\r\n",
                   ( unsigned ) ( uintptr_t ) &pxCurrentTCB,
                   ( unsigned ) ( uintptr_t ) pxCurrentTCB );
    for( i = 0; i < 7; i++, p += 4 )
    {
        wch_log_panic( "  %08x: %08x %08x %08x %08x\r\n",
                       ( unsigned ) ( uintptr_t ) p,
                       ( unsigned ) p[ 0 ], ( unsigned ) p[ 1 ],
                       ( unsigned ) p[ 2 ], ( unsigned ) p[ 3 ] );
    }
    for( ;; )
    {
    }
}

/*********************************************************************
 * TIM3: NimBLE hal timer (cputime).  Dispatch to the port layer.
 *********************************************************************/
void TIM3_IRQHandler( void )
{
    if( hal_timer_wch_irq )
    {
        hal_timer_wch_irq();
    }
}

/*********************************************************************
 * WWDG: never enabled by this project - report and park.
 *********************************************************************/
void WWDG_IRQHandler( void )
{
    uint32_t mcause, mepc, mtval;

    __asm volatile( "csrr %0, mcause" : "=r"( mcause ) );
    __asm volatile( "csrr %0, mepc"   : "=r"( mepc ) );
    __asm volatile( "csrr %0, mtval"  : "=r"( mtval ) );

    wch_log_panic( "\r\n[WWDG] mcause=%08x mepc=%08x mtval=%08x\r\n",
                   ( unsigned ) mcause, ( unsigned ) mepc, ( unsigned ) mtval );
    wch_log_panic( "[WWDG] isr=%08x %08x %08x\r\n",
                   ( unsigned ) PFIC->ISR[ 0 ], ( unsigned ) PFIC->ISR[ 1 ], ( unsigned ) PFIC->ISR[ 2 ] );
    wch_log_panic( "[WWDG] iactr=%08x %08x %08x gisr=%08x cfgr=%08x\r\n",
                   ( unsigned ) PFIC->IACTR[ 0 ], ( unsigned ) PFIC->IACTR[ 1 ], ( unsigned ) PFIC->IACTR[ 2 ],
                   ( unsigned ) PFIC->GISR, ( unsigned ) PFIC->CFGR );
    for( ;; )
    {
    }
}

/*********************************************************************
 * BLE baseband / link-layer engine interrupts.
 *********************************************************************/
void BB_IRQHandler( void )
{
    if( wch_rf_bb_isr )
    {
        wch_rf_bb_isr();
    }
}

void LLE_IRQHandler( void )
{
    if( wch_rf_lle_isr )
    {
        wch_rf_lle_isr();
    }
}

/*********************************************************************
 * DMA1 channel 4: log-ring drain (USART1_TX).  Body in
 * port/console_uart.c; the ISR only advances ring indices and chains
 * the next contiguous segment - it never logs or blocks.
 *********************************************************************/
void DMA1_Channel4_IRQHandler( void )
{
    wch_log_dma_irq();
}
