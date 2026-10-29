/*
 * main.c - CH32V208 NimBLE bring-up entry.
 *
 * Called from handle_reset (startup_ch32v20x_D8W.S) after SystemInit()
 * and in machine mode with global interrupts enabled (mstatus = 0x1888).
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
#include <stdio.h>

#include "FreeRTOS.h"
#include "task.h"

#include "SEGGER_RTT.h"
#include "ch32v20x.h"
#include "ch32v20x_flash.h"

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

void app_trace( uint32_t tag, void * ra )
{
    uint32_t w = g_trace_wr++ & 127U;

    g_trace_ring[ w * 3U ] = tag;
    g_trace_ring[ w * 3U + 1U ] = ( uint32_t ) ( uintptr_t ) ra;
    g_trace_ring[ w * 3U + 2U ] = g_trace_wr;  /* seq no. - tick read needs kernel.h */
}

#define TRACE( n )  app_trace( ( n ), __builtin_return_address( 0 ) )

/* Bring-up forensics: the SysTick sampler (port.c) validates pxCurrentTCB
 * against these three addresses; the sprayer manifests as pxCurrentTCB
 * pointing somewhere else entirely. */
const void * const g_tcb_addrs[ 3 ] =
{
    &xIdleTaskTCB, &xAppTaskTCB, &xTimerTaskTCB
};

/* Bring-up forensics: called by tasks.c before each stack fill. */
void prv_dbg_fill_log( void * dst, uint32_t size )
{
    SEGGER_RTT_printf( 0, "fill: dst=%08x size=%u\r\n",
                       ( unsigned ) ( uintptr_t ) dst, ( unsigned ) size );
}

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

    SEGGER_RTT_printf( 0, "chk[%s]: tcb=%08x%s wl=%d rtt=%d z=%d%d\r\n",
                       tag, ( unsigned ) tcb,
                       ( tcb == ( uint32_t ) &xAppTaskTCB ) ? "" : "!",
                       wl_bad, rtt_bad, zlo_bad, zhi_bad );

    if( ( tcb != ( uint32_t ) &xAppTaskTCB ) || wl_bad || rtt_bad ||
        zlo_bad || zhi_bad )
    {
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

    /* Bring-up forensics: trigger-module store watchpoint helpers (used by
 * main.c and the npl port; must live in a file both targets link). */
/* Program a QingKe trigger-module store watchpoint on 'addr'.  Any store
 * to it raises a breakpoint exception (HardFault), whose mepc names the
 * writing instruction.  Used on pool_evt's block-size word: written once
 * by os_mempool_init, never again by legitimate code. */
void wch_watch_store(uint32_t addr)
{
    /* tselect=0x7a0, tdata1=0x7a1, tdata2=0x7a2 (RV32 trigger module) */
    __asm__ volatile ("csrw 0x7a0, zero");            /* trigger 0 */
    /* mcontrol: type=2, M=1, store=1, load=0, action=0 (exception) */
    __asm__ volatile ("li t0, 0x20000042; csrw 0x7a1, t0" ::: "t0");
    __asm__ volatile ("csrw 0x7a2, %0" :: "r" (addr));
}

/* Execute-breakpoint at addr: fires when the PC matches.  With action=0
 * this raises a breakpoint exception (vector slot 3 -> wch_exc_trap). */
void wch_watch_exec(uint32_t addr)
{
    /* mcontrol (type 2): EXECUTE = bit2, M = bit5, action 0 (exception) */
    __asm__ volatile ("csrw 0x7a0, zero");
    __asm__ volatile ("li t0, 0x24; csrw 0x7a1, t0" ::: "t0");
    __asm__ volatile ("csrw 0x7a2, %0" :: "r" (addr));
}

/* Calibration: does the trigger module exist and hold writes?  Prints
 * tinfo and a write/readback of tdata1 with a disabled type. */
void wch_watch_probe(void)
{
    uint32_t info = 0, rb = 0, sel = 0;

    __asm__ volatile ("csrr %0, 0x7a4" : "=r" (info));
    __asm__ volatile ("csrw 0x7a0, zero");
    __asm__ volatile ("li t0, 0x60000000; csrw 0x7a1, t0" ::: "t0");
    __asm__ volatile ("csrr %0, 0x7a1" : "=r" (rb));
    __asm__ volatile ("csrr %0, 0x7a0" : "=r" (sel));
    SEGGER_RTT_printf( 0, "trg: tinfo=%08x sel=%08x tdata1rb=%08x\r\n",
                       ( unsigned ) info, ( unsigned ) sel, ( unsigned ) rb );
}

/* Bring-up forensics: executes from beyond the 128K zero-wait window. */
__attribute__( ( section( ".farcode" ), noinline ) )
int far_add( int a, int b )
{
    volatile int t = a * 3;
    return t + b;
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
     * startup_ch32v20x_D8W.S now writes 0x2 (nesting on, HPE off). */
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

/* Bring-up forensics: trigger-module calibration + execute-breakpoint
     * at PC=0x0.  When the phantom wild jump lands, slot-3 vectors into the
     * raw exc trap which stashes ra = the culprit call site. */
    {
        extern void wch_watch_probe(void);
        extern void wch_watch_exec(uint32_t);
        wch_watch_probe();
        /* The wild jump lands at PC=0 (then re-runs the CRT through the
         * 0x0 alias).  An execute-trigger at 0 fires BEFORE any
         * instruction there runs, so the trap stash captures the caller's
         * ra intact - the null/garbage function pointer call site. */
        wch_watch_exec( 0x0UL );
        SEGGER_RTT_WriteString( 0, "trg: exec-watch @0 armed\r\n" );
    }

    /* Bring-up forensics: varargs battery.  The silent deaths all landed on
     * the FIRST argument-carrying console_printf (vsnprintf %p path) and
     * once on SEGGER_RTT_printf (%x) from an ISR - print each primitive in
     * isolation with a breadcrumb before it. */
    SEGGER_RTT_WriteString( 0, "vat: segger-x\n" );
    SEGGER_RTT_printf( 0, "vat: segger %08x done\r\n", 0xDEADBEEFUL );
    {
        char tbuf[ 64 ];

        SEGGER_RTT_WriteString( 0, "vat: vsnprintf-d\n" );
        ( void ) snprintf( tbuf, sizeof( tbuf ), "vat: d=%d done\n", 42 );
        SEGGER_RTT_WriteString( 0, tbuf );
        SEGGER_RTT_WriteString( 0, "vat: vsnprintf-x\n" );
        ( void ) snprintf( tbuf, sizeof( tbuf ), "vat: x=%08x done\n", 0xCAFEBABEUL );
        SEGGER_RTT_WriteString( 0, tbuf );
        SEGGER_RTT_WriteString( 0, "vat: vsnprintf-p\n" );
        ( void ) snprintf( tbuf, sizeof( tbuf ), "vat: p=%p done\n", ( void * ) 0x12345678UL );
        SEGGER_RTT_WriteString( 0, tbuf );
    }

    /* Option-byte repair, only if the partition actually deviates.
     * RAM_CODE_MOD (OBR[9:7]) is a bit PATTERN, not an enum: 00x (values
     * 0-1) = CODE-128K + RAM-64K (factory default - value 1 is the SAME
     * partition as 0!), 01x (2-3) = 144K+48K, 1xx (4-7) = 160K+32K.
     * Mode 1 was initially misread as non-default and an unnecessary
     * reprogram was attempted (and faulted mid-way) - hence the pattern
     * test below and the RAM-valid scratch stack the repair runs on.
     * ld/Link.ld requires the 00x family (64K SRAM). */
    {
        uint32_t mode = ( *( volatile uint32_t * ) 0x4002201CUL >> 7 ) & 0x7UL;

        if( ( mode & 0x6UL ) != 0UL )
        {
            /* Valid RAM top for the CURRENT mode: 00x=64K, 01x=48K,
             * 1xx=32K.  main() runs on the main stack at 0x2000F800+,
             * which is nonexistent under a non-default mode; the SDK's
             * FLASH_ProgramOptionByteData reads all option halfwords
             * into a stack array and rewrites them - run it from a
             * 4K scratch stack just below the valid top (inside the
             * .heap NOLOAD region, unused before malloc). */
            uint32_t ram_top = ( mode & 0x4UL ) ? 0x20008000UL
                               : ( ( mode & 0x2UL ) ? 0x2000C000UL
                                                    : 0x20010000UL );
            uint8_t user = *( volatile uint8_t * ) 0x1FFFF802UL;
            uint8_t new_user = ( uint8_t ) ( user & 0x1FU ); /* [7:5]=000 */
            FLASH_Status st;

            SEGGER_RTT_printf( 0,
                               "memcfg  : RAM_CODE_MOD=%lu non-default, "
                               "restoring 000 (USER %02x->%02x)\r\n",
                               ( unsigned long ) mode, user, new_user );

            __asm__ volatile ( "mv sp, %0" : : "r"( ram_top - 0x1000UL )
                                                : "memory" );

            FLASH_Unlock();
            st = FLASH_ProgramOptionByteData( 0x1FFFF802UL, new_user );

            if( ( st == FLASH_COMPLETE ) &&
                ( ( *( volatile uint8_t * ) 0x1FFFF802UL & 0xE0U ) == 0U ) )
            {
                SEGGER_RTT_WriteString( 0,
                    "memcfg  : option byte restored, rebooting\r\n" );
            }
            else
            {
                SEGGER_RTT_printf( 0,
                    "memcfg  : repair FAILED st=%d user=%02x, "
                    "fix option bytes with WCH tools\r\n",
                    ( int ) st,
                    ( unsigned ) *( volatile uint8_t * ) 0x1FFFF802UL );
            }
            NVIC_SystemReset();
            for( ;; )
            {
            }
        }
    }

    /* RM 32.3: code in the flash region BEYOND the zero-wait window
     * (RAM_CODE_MOD 00x -> 128K) only executes correctly with the FLASH
     * enhanced-read mode enabled.  The image is ~134K and grows; without
     * EHMOD every fetch past 0x08020000 returned garbage - the wild jumps
     * (mcause=0/mepc=0/ra=8/sp=0/gp=0x101) whose death site wandered
     * between np:5/np:8/rng_seed as the layout shifted between builds.
     * The WCH closed BLE lib enables this internally; the register-level
     * driver must do it itself.  FLASH_CTLR(0x40022010).EHMOD = bit24;
     * FLASH_STATR(0x4002200C).EHMODS = bit7 confirms.  NOTES: exit before
     * any flash erase/program (option-byte repair above runs BEFORE this
     * on purpose) and before stop mode; hardware auto-exits on reset.
     * EHMOD is a "normal+fast unlock" bit: FLASH_CTLR comes up LOCKed
     * (bit7=1) and silently ignores the write, so run the dual key
     * sequence (FLASH_Unlock_Fast) first. */
    {
        uint32_t ctlr = *( volatile uint32_t * ) 0x40022010UL;

        if( ( ctlr & 0x01000000UL ) == 0UL )
        {
            FLASH_Unlock_Fast();
            *( volatile uint32_t * ) 0x40022010UL =
                ( *( volatile uint32_t * ) 0x40022010UL ) | 0x01000000UL;
        }
        SEGGER_RTT_printf( 0, "ehmod   : CTLR=%08x EHMODS=%u\r\n",
                           ( unsigned ) *( volatile uint32_t * ) 0x40022010UL,
                           ( unsigned ) ( ( *( volatile uint32_t * ) 0x4002200CUL >> 7 ) & 1UL ) );

        /* CPU data read beyond the CODE window: if this returns the real
         * instruction bytes the data path works and only fetch is broken;
         * 0xFFFFFFFF means the whole controller path is closed. */
        {
            uint32_t v = *( volatile uint32_t * ) 0x08021000UL;
            SEGGER_RTT_printf( 0, "farpeek : %08x\r\n", ( unsigned ) v );
        }
    }

    /* Out-of-window execution canary: far_add lives at 0x08020100. */
    {
        int r = far_add( 10, 20 );
        SEGGER_RTT_printf( 0, "farcode : far_add(10,20)=%d %s\r\n", r,
                           ( r == 50 ) ? "OK" : "WRONG" );
    }

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
