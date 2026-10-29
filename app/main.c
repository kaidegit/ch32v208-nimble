/*
 * main.c - CH32V208 NimBLE bring-up entry.
 *
 * Called from handle_reset (startup_ch32v20x_D8W.S) after SystemInit()
 * and in machine mode with interrupts masked until the scheduler starts.
 *
 * Responsibilities:
 *  - refresh SystemCoreClock, init SEGGER RTT and print the boot banner
 *  - start the FreeRTOS scheduler with a single "app" task
 *  - app_ble_start() is a weak no-op here; the strong version in
 *    app/app_ble.c (other agent) brings up NimBLE when the full image
 *    is built.  The 'boot' image links without it on purpose.
 *
 * PFIC enabling of BB/LLE/TIM3 is deliberately NOT done here - that
 * belongs to the drivers that own those interrupts.
 */

#include <stdint.h>

#include "FreeRTOS.h"
#include "task.h"

#include "SEGGER_RTT.h"
#include "ch32v20x.h"

/* Linker-provided boundaries (ld/Link.ld). */
extern char _heap_start[];
extern char _heap_end[];
extern char _susrstack[];
extern char _eusrstack[];

void app_ble_start( void ) __attribute__( ( weak ) );

/* Forensic zone for the app task: one array holding 8 tripwire words, the
 * 1024-word task stack, and 8 more tripwires - adjacency guaranteed by the
 * single object.  The bring-up saw the A5 fill pattern fully destroyed
 * (hw=0) far below any legitimate depth (-fstack-usage max frame 320 B),
 * i.e. RAM is being written by something; vApplicationStackOverflowHook
 * dumps these words to fingerprint the writer. */
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

/* Bring-up forensics: in-RAM flight recorder.  trace(tag, ra) writes
 * (tag, ra, tick) into a fixed ring - no printing, no RTT, ~3 stores.
 * After a silent death, read g_trace_ring via gdb: the last entry's ra
 * pins the exact call site the bomb hit from. */
volatile uint32_t g_trace_ring[ 3 * 128 ];
volatile uint32_t g_trace_wr;
volatile uint32_t g_tick_sample_pc;

void app_trace( uint32_t tag, void * ra )
{
    uint32_t w = g_trace_wr++ & 127U;

    g_trace_ring[ w * 3U ] = tag;
    g_trace_ring[ w * 3U + 1U ] = ( uint32_t ) ( uintptr_t ) ra;
    g_trace_ring[ w * 3U + 2U ] = g_trace_wr;  /* seq no. - tick read needs kernel.h */
}

#define TRACE( n )  app_trace( ( n ), __builtin_return_address( 0 ) )

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
    SEGGER_RTT_printf( 0, "\r\n[malloc-failed] heap exhausted in task %s\r\n",
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
    SEGGER_RTT_printf( 0, "\r\n[stack-overflow] task: %s, hw=%uw, heap_free=%u, sp=%x\r\n",
                       pcTaskName,
                       ( unsigned ) uxTaskGetStackHighWaterMark( xTask ),
                       ( unsigned ) xPortGetFreeHeapSize(),
                       ( unsigned ) sp );
    if( xTask == xAppTaskHandle )
    {
        SEGGER_RTT_printf( 0, "lo  : %x %x %x %x %x %x %x %x\r\n",
                           lo[ 0 ], lo[ 1 ], lo[ 2 ], lo[ 3 ],
                           lo[ 4 ], lo[ 5 ], lo[ 6 ], lo[ 7 ] );
        SEGGER_RTT_printf( 0, "base: %x %x %x %x %x %x %x %x\r\n",
                           base[ 0 ], base[ 1 ], base[ 2 ], base[ 3 ],
                           base[ 4 ], base[ 5 ], base[ 6 ], base[ 7 ] );
        SEGGER_RTT_printf( 0, "top : %x %x %x %x %x %x %x %x\r\n",
                           top[ 0 ], top[ 1 ], top[ 2 ], top[ 3 ],
                           top[ 4 ], top[ 5 ], top[ 6 ], top[ 7 ] );
        SEGGER_RTT_printf( 0, "hi  : %x %x %x %x %x %x %x %x\r\n",
                           hi[ 0 ], hi[ 1 ], hi[ 2 ], hi[ 3 ],
                           hi[ 4 ], hi[ 5 ], hi[ 6 ], hi[ 7 ] );
    }
    for( ;; )
    {
    }
}

/*********************************************************************
 * Bring-up forensics: async-writer tripwire checker.
 *
 * Every bring-up step calls app_dbg_chk(tag).  It fingerprints five
 * canaries at once: pxCurrentTCB (kernel slot @0x2000036C, the word the
 * tick handler dereferences), the unused WCH whitelist (64 B of .bss
 * that nothing writes before a whitelist op), the RTT control-block ID
 * ("SEGGER RTT" - probe-rs saw channel descriptors damaged), and the
 * g_app_zone tripwires.  On any anomaly it dumps the kernel-variable
 * neighborhood around pxCurrentTCB so the spray shape is visible.
 */
extern void * volatile pxCurrentTCB;       /* FreeRTOS tasks.c            */
extern uint32_t g_wch_whitelist[];         /* ble_hw_wch.c canary, 64 B   */
/* _SEGGER_RTT comes from SEGGER_RTT.h (already included): ID string is
 * the 16-byte sName[] at offset 0. */

void app_dbg_chk( const char * tag )
{
    volatile uint32_t * slot = ( volatile uint32_t * ) &pxCurrentTCB;
    uint32_t tcb = *slot;
    static const char rtt_id[] = "SEGGER RTT";
    int wl_bad = 0, rtt_bad = 0, zlo_bad = 0, zhi_bad = 0;
    int i;

    for( i = 0; i < 16; i++ )
    {
        if( g_wch_whitelist[ i ] != 0UL )
        {
            wl_bad = 1;
            break;
        }
    }
    for( i = 0; i < 10; i++ )
    {
        if( _SEGGER_RTT.acID[ i ] != rtt_id[ i ] )
        {
            rtt_bad = 1;
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

    if( ( tcb != ( uint32_t ) &xAppTaskTCB ) || wl_bad || rtt_bad ||
        zlo_bad || zhi_bad )
    {
        SEGGER_RTT_printf( 0, "chk[%s]: tcb=%08x wl=%d rtt=%d z=%d%d\r\n",
                           tag, (unsigned)tcb, wl_bad, rtt_bad, zlo_bad, zhi_bad );
        volatile uint32_t * p = slot - 12;

        for( i = 0; i < 6; i++, p += 4 )
        {
            SEGGER_RTT_printf( 0, "  %08x: %08x %08x %08x %08x\r\n",
                               ( unsigned ) ( uintptr_t ) p,
                               ( unsigned ) p[ 0 ], ( unsigned ) p[ 1 ],
                               ( unsigned ) p[ 2 ], ( unsigned ) p[ 3 ] );
        }
        SEGGER_RTT_printf( 0, "  wl : %08x %08x %08x %08x\r\n",
                           ( unsigned ) g_wch_whitelist[ 0 ],
                           ( unsigned ) g_wch_whitelist[ 1 ],
                           ( unsigned ) g_wch_whitelist[ 2 ],
                           ( unsigned ) g_wch_whitelist[ 3 ] );
        SEGGER_RTT_printf( 0, "  rtt: %08x %08x %08x %08x\r\n",
                           ( unsigned ) *( volatile uint32_t * ) &_SEGGER_RTT.acID[ 0 ],
                           ( unsigned ) *( volatile uint32_t * ) &_SEGGER_RTT.acID[ 4 ],
                           ( unsigned ) *( volatile uint32_t * ) &_SEGGER_RTT.acID[ 8 ],
                           ( unsigned ) *( volatile uint32_t * ) &_SEGGER_RTT.acID[ 12 ] );
    }
}

/*********************************************************************
 * app task: BLE bring-up hook, then a 1 s heartbeat with uptime ticks.
 *********************************************************************/
static void app_task( void * pvParameters )
{
    ( void ) pvParameters;

    app_ble_start(); /* no-op while linked against the weak default */

    for( ;; )
    {
        vTaskDelay( pdMS_TO_TICKS( 1000 ) );
        SEGGER_RTT_printf( 0, "[app] alive, uptime=%u ticks, sysclk=%u, stk_hw=%uw\r\n",
                           ( unsigned ) xTaskGetTickCount(),
                           ( unsigned ) SystemCoreClock,
                           ( unsigned ) uxTaskGetStackHighWaterMark( NULL ) );
    }
}

int main( void )
{
    /* SystemInit() already ran from the reset handler; refresh the
     * variable from the programmed PLL (96 MHz, 32 MHz HSE x3). */
    SystemCoreClockUpdate();

    SEGGER_RTT_Init();

    SEGGER_RTT_WriteString( 0, "\r\n==============================\r\n" );
    SEGGER_RTT_printf( 0, "CH32V208 NimBLE bring-up\r\n" );
    SEGGER_RTT_printf( 0, "build   : %s %s\r\n", __DATE__, __TIME__ );
    SEGGER_RTT_printf( 0, "sysclk  : %u Hz\r\n", ( unsigned ) SystemCoreClock );
    SEGGER_RTT_printf( 0, "kernel  : FreeRTOS %s, tick %d Hz\r\n",
                       tskKERNEL_VERSION_NUMBER, ( int ) configTICK_RATE_HZ );
    SEGGER_RTT_printf( 0, "heap    : FreeRTOS %u B + newlib [%p..%p] (%u B)\r\n",
                       ( unsigned ) configTOTAL_HEAP_SIZE,
                       ( void * ) _heap_start, ( void * ) _heap_end,
                       ( unsigned ) ( _heap_end - _heap_start ) );
    SEGGER_RTT_printf( 0, "stack   : %p..%p\r\n",
                       ( void * ) _susrstack, ( void * ) _eusrstack );
    /* Memory split actually loaded by the chip (RM ch.32): FLASH_OBR[9:7]
     * RAM_CODE_MOD - 000=CODE-128K+RAM-64K, 01x=144K+48K, 1xx=160K+32K;
     * ESIG F_SIZE @0x1FFFF7E0 = usable code flash in KB.  The ld assumes
     * 128K zero-wait code window + 64K SRAM - verify, don't trust. */
    SEGGER_RTT_printf( 0, "memcfg  : OBR=%08x ram_code_mod=%u fsize=%uK\r\n",
                       ( unsigned ) *( volatile uint32_t * ) 0x4002201CUL,
                       ( unsigned ) ( ( *( volatile uint32_t * ) 0x4002201CUL >> 7 ) & 0x7UL ),
                       ( unsigned ) *( volatile uint16_t * ) 0x1FFFF7E0UL );
    /* INTSYSCR (CSR 0x804): confirm the startup's HPE/nesting configuration -
     * startup_ch32v20x_D8W.S writes 0 (nesting and HPE off). */
    {
        uint32_t intsyscr;

        __asm__ volatile ( "csrr %0, 0x804" : "=r"( intsyscr ) );
        SEGGER_RTT_printf( 0, "intcfg  : INTSYSCR=%08x nest=%u hpe=%u\r\n",
                           ( unsigned ) intsyscr,
                           ( unsigned ) ( ( intsyscr >> 1 ) & 1UL ),
                           ( unsigned ) ( intsyscr & 1UL ) );
    }
    /* 96-bit factory UID - provenance data for the deterministic garbage
     * values written into RAM (is the junk derived from chip-fixed data?). */
    SEGGER_RTT_printf( 0, "uid    : %08x %08x %08x\r\n",
                       ( unsigned ) *( volatile uint32_t * ) 0x1FFFF7E8UL,
                       ( unsigned ) *( volatile uint32_t * ) 0x1FFFF7ECUL,
                       ( unsigned ) *( volatile uint32_t * ) 0x1FFFF7F0UL );

    SEGGER_RTT_WriteString( 0, "==============================\r\n" );

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
        SEGGER_RTT_WriteString( 0, "[main] xTaskCreateStatic(app) failed\r\n" );
        for( ;; )
        {
        }
    }

    vTaskStartScheduler();

    /* Only returns if the kernel ran out of heap before the first task
     * could be scheduled. */
    SEGGER_RTT_WriteString( 0, "[main] vTaskStartScheduler returned\r\n" );
    for( ;; )
    {
    }
}
