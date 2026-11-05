/*
 * main.c - CH32V208 NimBLE bring-up entry.
 *
 * Called from handle_reset (startup_ch32v20x_D8W.S) after SystemInit()
 * and in machine mode with interrupts masked until the scheduler starts.
 *
 * Responsibilities:
 *  - refresh SystemCoreClock, init the UART log backend and print the boot banner
 *  - start the FreeRTOS scheduler with a single "app" task
 *  - app_ble_start() is a weak no-op here; the strong version in
 *    app/app_ble.c brings up NimBLE when the full image
 *    is built.  The 'boot' image links without it on purpose.
 *
 * PFIC enabling of BB/LLE/TIM3 is deliberately NOT done here - that
 * belongs to the drivers that own those interrupts.
 */

#include <stdint.h>

#include "FreeRTOS.h"
#include "task.h"

#include "console/console.h"
#define LOG_TAG  "boot"
#include "elog.h"
#include "wch_log.h"
#include "wch_power.h"
#include "ch32v20x.h"

/* Linker-provided boundaries (ld/Link.ld). */
extern char _heap_start[];
extern char _heap_end[];
extern char _susrstack[];
extern char _eusrstack[];

void app_ble_start( void ) __attribute__( ( weak ) );

/* Guard words surround the static app stack so failure hooks can report
 * writes beyond either end without relying on dynamic allocation. */
#define APP_STACK_WORDS     ( 1024 )
#define APP_TRIPWIRE_WORD   ( 0xC0DEC0DEUL )
static uint32_t g_app_zone[ 8 + APP_STACK_WORDS + 8 ];
static StaticTask_t xAppTaskTCB;
static TaskHandle_t xAppTaskHandle;

/* configSUPPORT_STATIC_ALLOCATION: kernel-owned idle/timer buffers. */
static StaticTask_t xIdleTaskTCB;
static StackType_t xIdleTaskStack[ configMINIMAL_STACK_SIZE ];
static StaticTask_t xTimerTaskTCB;
static StackType_t xTimerTaskStack[ configTIMER_TASK_STACK_DEPTH ];

void vApplicationGetIdleTaskMemory( StaticTask_t ** ppxIdleTaskTCBBuffer,
                                    StackType_t ** ppxIdleTaskStackBuffer,
                                    uint32_t * pulIdleTaskStackSize )
{
    *ppxIdleTaskTCBBuffer = &xIdleTaskTCB;
    *ppxIdleTaskStackBuffer = xIdleTaskStack;
    *pulIdleTaskStackSize = configMINIMAL_STACK_SIZE;
}

void vApplicationGetTimerTaskMemory( StaticTask_t ** ppxTimerTaskTCBBuffer,
                                     StackType_t ** ppxTimerTaskStackBuffer,
                                     uint32_t * pulTimerTaskStackSize )
{
    *ppxTimerTaskTCBBuffer = &xTimerTaskTCB;
    *ppxTimerTaskStackBuffer = xTimerTaskStack;
    *pulTimerTaskStackSize = configTIMER_TASK_STACK_DEPTH;
}

static void app_task( void * pvParameters );

/*********************************************************************
 * Weak default: the boot image has no BLE stack, so starting the app
 * task is a no-op and it just reports its heartbeat.
 *********************************************************************/
void app_ble_start( void )
{
    /* Real implementation: app/app_ble.c */
}

/*********************************************************************
 * FreeRTOS failure hooks (enabled in config/FreeRTOSConfig.h).
 *********************************************************************/
void vApplicationMallocFailedHook( void )
{
    taskDISABLE_INTERRUPTS();
    wch_log_panic( "\r\n[malloc-failed] heap exhausted in task %s\r\n",
                   pcTaskGetName( NULL ) );
    for( ;; )
    {
    }
}

void vApplicationStackOverflowHook( TaskHandle_t xTask, char * pcTaskName )
{
    uint32_t sp;
    const uint32_t * lo   = &g_app_zone[ 0 ];
    const uint32_t * base = &g_app_zone[ 8 ];
    const uint32_t * top  = &g_app_zone[ 8 + APP_STACK_WORDS - 8 ];
    const uint32_t * hi   = &g_app_zone[ 8 + APP_STACK_WORDS ];

    __asm__ volatile ( "mv %0, sp" : "=r" ( sp ) );

    taskDISABLE_INTERRUPTS();
    /* hw = words of intact A5 fill from the stack base. */
    wch_log_panic( "\r\n[stack-overflow] task: %s, hw=%uw, heap_free=%u, sp=%x\r\n",
                   pcTaskName,
                   ( unsigned ) uxTaskGetStackHighWaterMark( xTask ),
                   ( unsigned ) xPortGetFreeHeapSize(),
                   ( unsigned ) sp );
    if( xTask == xAppTaskHandle )
    {
        wch_log_panic( "lo  : %x %x %x %x %x %x %x %x\r\n",
                       ( unsigned ) lo[ 0 ], ( unsigned ) lo[ 1 ], ( unsigned ) lo[ 2 ], ( unsigned ) lo[ 3 ],
                       ( unsigned ) lo[ 4 ], ( unsigned ) lo[ 5 ], ( unsigned ) lo[ 6 ], ( unsigned ) lo[ 7 ] );
        wch_log_panic( "base: %x %x %x %x %x %x %x %x\r\n",
                       ( unsigned ) base[ 0 ], ( unsigned ) base[ 1 ], ( unsigned ) base[ 2 ], ( unsigned ) base[ 3 ],
                       ( unsigned ) base[ 4 ], ( unsigned ) base[ 5 ], ( unsigned ) base[ 6 ], ( unsigned ) base[ 7 ] );
        wch_log_panic( "top : %x %x %x %x %x %x %x %x\r\n",
                       ( unsigned ) top[ 0 ], ( unsigned ) top[ 1 ], ( unsigned ) top[ 2 ], ( unsigned ) top[ 3 ],
                       ( unsigned ) top[ 4 ], ( unsigned ) top[ 5 ], ( unsigned ) top[ 6 ], ( unsigned ) top[ 7 ] );
        wch_log_panic( "hi  : %x %x %x %x %x %x %x %x\r\n",
                       ( unsigned ) hi[ 0 ], ( unsigned ) hi[ 1 ], ( unsigned ) hi[ 2 ], ( unsigned ) hi[ 3 ],
                       ( unsigned ) hi[ 4 ], ( unsigned ) hi[ 5 ], ( unsigned ) hi[ 6 ], ( unsigned ) hi[ 7 ] );
    }
    for( ;; )
    {
    }
}

/* Validate the app task pointer and static memory guards during BLE init.
 * Log details only on corruption; the normal path produces no output. */
extern void * volatile pxCurrentTCB;       /* FreeRTOS tasks.c            */
extern uint32_t g_wch_whitelist[];         /* ble_hw_wch.c canary, 64 B   */
/* g_wch_log_canary_lo/hi come from wch_log.h (console_uart.c). */

#define LOG_CANARY_WORD    ( 0x5A5AA5A5UL )

void app_dbg_chk( const char * tag )
{
    volatile uint32_t * slot = ( volatile uint32_t * ) &pxCurrentTCB;
    uint32_t tcb = *slot;
    int wl_bad = 0, ring_bad = 0, zlo_bad = 0, zhi_bad = 0;
    int i;

    for( i = 0; i < 16; i++ )
    {
        if( g_wch_whitelist[ i ] != 0UL )
        {
            wl_bad = 1;
            break;
        }
    }
    for( i = 0; i < 4; i++ )
    {
        if( ( g_wch_log_canary_lo[ i ] != LOG_CANARY_WORD ) ||
            ( g_wch_log_canary_hi[ i ] != LOG_CANARY_WORD ) )
        {
            ring_bad = 1;
            break;
        }
    }
    for( i = 0; i < 8; i++ )
    {
        if( g_app_zone[ i ] != APP_TRIPWIRE_WORD )
        {
            zlo_bad = 1;
            break;
        }
    }
    for( i = 0; i < 8; i++ )
    {
        if( g_app_zone[ 8 + APP_STACK_WORDS + i ] != APP_TRIPWIRE_WORD )
        {
            zhi_bad = 1;
            break;
        }
    }

    if( ( tcb != ( uint32_t ) &xAppTaskTCB ) || wl_bad || ring_bad ||
        zlo_bad || zhi_bad )
    {
        console_printf( "chk[%s]: tcb=%08x wl=%d ring=%d z=%d%d\r\n",
                        tag, (unsigned)tcb, wl_bad, ring_bad, zlo_bad, zhi_bad );
        volatile uint32_t * p = slot - 12;

        for( i = 0; i < 6; i++, p += 4 )
        {
            console_printf( "  %08x: %08x %08x %08x %08x\r\n",
                            ( unsigned ) ( uintptr_t ) p,
                            ( unsigned ) p[ 0 ], ( unsigned ) p[ 1 ],
                            ( unsigned ) p[ 2 ], ( unsigned ) p[ 3 ] );
        }
        console_printf( "  wl : %08x %08x %08x %08x\r\n",
                        ( unsigned ) g_wch_whitelist[ 0 ],
                        ( unsigned ) g_wch_whitelist[ 1 ],
                        ( unsigned ) g_wch_whitelist[ 2 ],
                        ( unsigned ) g_wch_whitelist[ 3 ] );
        console_printf( "  rng: lo=%08x hi=%08x\r\n",
                        ( unsigned ) g_wch_log_canary_lo[ 0 ],
                        ( unsigned ) g_wch_log_canary_hi[ 0 ] );
    }
}

/*********************************************************************
 * app task: BLE bring-up hook, then a 1 s heartbeat with uptime ticks.
 *********************************************************************/
#undef LOG_TAG
#define LOG_TAG  "app"

static void app_task( void * pvParameters )
{
#if !CH32_LOW_POWER || CH32_POWER_DIAGNOSTICS
    struct wch_log_stats stats;
#endif
    ( void ) pvParameters;

    wch_power_init();
    app_ble_start(); /* no-op while linked against the weak default */

    wch_power_start();
#if CH32_LOW_POWER && !CH32_POWER_DIAGNOSTICS
    log_i("power measurement: periodic app diagnostics disabled");
    vTaskSuspend(NULL);
    for (;;) {} /* this task is not resumed by the application */
#else
    TickType_t heartbeat = xTaskGetTickCount();
    for( ;; )
    {
        vTaskDelayUntil(&heartbeat, pdMS_TO_TICKS(1000));
        wch_log_get_stats( &stats );
        log_i("pm: RTC=%u mHz stop=%u us=%u abort=%u overrun=%u cal=%u/%u restore_max=%uus short=%u",
              (unsigned)g_wch_power.rtc_millihz, (unsigned)g_wch_power.sleeps,
              (unsigned)g_wch_power.slept_us, (unsigned)g_wch_power.aborts,
              (unsigned)g_wch_power.overruns, (unsigned)g_wch_power.calibrations,
              (unsigned)g_wch_power.calibration_errors,
              (unsigned)g_wch_power.recovery_max_us, (unsigned)g_wch_power.short_sleeps);
        log_i( "alive, uptime=%u ticks, sysclk=%u, stk_hw=%uw, "
               "log drop=%u/%uB dma_err=%u dead=%u panic=%u",
               ( unsigned ) xTaskGetTickCount(),
               ( unsigned ) SystemCoreClock,
               ( unsigned ) uxTaskGetStackHighWaterMark( NULL ),
               ( unsigned ) stats.drop_msgs, ( unsigned ) stats.drop_bytes,
               ( unsigned ) stats.dma_errors, ( unsigned ) stats.dma_dead,
               ( unsigned ) stats.panic_mode );
    }
#endif
}

/* Back to the boot tag for main()'s banner (macros use the definition
 * visible at each call site). */
#undef LOG_TAG
#define LOG_TAG  "boot"

int main( void )
{
    /* SystemInit() already ran from the reset handler; refresh the
     * variable from the programmed HSE/PLL and AHB divider. */
    SystemCoreClockUpdate();
    /* All active BLE timing comes from HSE/PLL -> TIM3 and SysTick.
     * Do not inherit an enabled LSI from a previous firmware. */
    RCC->RSTSCKR &= ~RCC_LSION;

    /* USART1 + DMA1_CH4 + EasyLogger (port/console_uart.c). */
    wch_log_init();

    log_i( "==============================" );
    log_i( "CH32V208 NimBLE bring-up" );
    log_i( "build   : %s %s", __DATE__, __TIME__ );
    log_i( "sysclk  : %u Hz", ( unsigned ) SystemCoreClock );
    log_i( "osc     : HSE(on/ready)=%u/%u LSE=%u/%u LSI=%u/%u",
           !!( RCC->CTLR & RCC_HSEON ), !!( RCC->CTLR & RCC_HSERDY ),
           !!( RCC->BDCTLR & RCC_LSEON ), !!( RCC->BDCTLR & RCC_LSERDY ),
           !!( RCC->RSTSCKR & RCC_LSION ), !!( RCC->RSTSCKR & RCC_LSIRDY ) );
    log_i( "kernel  : FreeRTOS %s, tick %d Hz",
           tskKERNEL_VERSION_NUMBER, ( int ) configTICK_RATE_HZ );
    log_i( "heap    : FreeRTOS %u B + newlib [%p..%p] (%u B)",
           ( unsigned ) configTOTAL_HEAP_SIZE,
           ( void * ) _heap_start, ( void * ) _heap_end,
           ( unsigned ) ( _heap_end - _heap_start ) );
    log_i( "stack   : %p..%p",
           ( void * ) _susrstack, ( void * ) _eusrstack );
    /* Memory split actually loaded by the chip (RM ch.32): FLASH_OBR[9:7]
     * RAM_CODE_MOD - 00x=CODE-128K+RAM-64K, 01x=144K+48K, 1xx=160K+32K;
     * ESIG F_SIZE @0x1FFFF7E0 = usable code flash in KB.  The ld assumes
     * 128K zero-wait code window + 64K SRAM - verify, don't trust. */
    log_i( "memcfg  : OBR=%08x ram_code_mod=%u fsize=%uK",
           ( unsigned ) *( volatile uint32_t * ) 0x4002201CUL,
           ( unsigned ) ( ( *( volatile uint32_t * ) 0x4002201CUL >> 7 ) & 0x7UL ),
           ( unsigned ) *( volatile uint16_t * ) 0x1FFFF7E0UL );
    /* INTSYSCR (CSR 0x804): confirm the startup's HPE/nesting configuration -
     * startup_ch32v20x_D8W.S writes 0 (nesting and HPE off). */
    {
        uint32_t intsyscr;

        __asm__ volatile ( "csrr %0, 0x804" : "=r"( intsyscr ) );
        log_i( "intcfg  : INTSYSCR=%08x nest=%u hpe=%u",
               ( unsigned ) intsyscr,
               ( unsigned ) ( ( intsyscr >> 1 ) & 1UL ),
               ( unsigned ) ( intsyscr & 1UL ) );
    }
    /* 96-bit factory UID - provenance data for the deterministic garbage
     * values written into RAM (is the junk derived from chip-fixed data?). */
    log_i( "uid    : %08x %08x %08x",
           ( unsigned ) *( volatile uint32_t * ) 0x1FFFF7E8UL,
           ( unsigned ) *( volatile uint32_t * ) 0x1FFFF7ECUL,
           ( unsigned ) *( volatile uint32_t * ) 0x1FFFF7F0UL );

    log_i( "==============================" );

    /* Static task inside g_app_zone: stack corruption forensics (see the
     * zone comment above; depth itself is measured adequate - max frame in
     * the bring-up chain is console_printf's 320 B). */
    for( size_t i = 0; i < 8; i++ )
    {
        g_app_zone[ i ] = APP_TRIPWIRE_WORD;
        g_app_zone[ 8 + APP_STACK_WORDS + i ] = APP_TRIPWIRE_WORD;
    }

    xAppTaskHandle = xTaskCreateStatic( app_task, "app", APP_STACK_WORDS, NULL,
                                        tskIDLE_PRIORITY + 2,
                                        ( StackType_t * ) &g_app_zone[ 8 ],
                                        &xAppTaskTCB );
    if( xAppTaskHandle == NULL )
    {
        log_e( "xTaskCreateStatic(app) failed" );
        for( ;; )
        {
        }
    }

    vTaskStartScheduler();

    /* Only returns if the kernel ran out of heap before the first task
     * could be scheduled. */
    log_e( "vTaskStartScheduler returned" );
    for( ;; )
    {
    }
}
