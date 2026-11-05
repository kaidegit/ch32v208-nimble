/*
 * console_uart.c - console facade + NimBLE NPL log sink + EasyLogger port
 *                  on USART1 (PA9 TX / PA10 RX) with DMA1_CH4 async TX.
 *
 * Replaces port/console_rtt.c: runtime logs leave via the UART, not via
 * probe-rs RTT RAM reads (which halted the CPU on this chip and broke RF).
 *
 * Data flow (runtime logging - never waits for the UART):
 *   caller formats on its own stack (vsnprintf) -> wch_log_write() copies
 *   the bytes into the DMA ring under a short MIE/MPIE-masked window ->
 *   kicks DMA1_CH4 if idle; the TC interrupt advances the tail and chains
 *   the next contiguous ring segment; USART1 shifts bytes out at 921600 8N1.
 *
 * Concurrency rules (explicit, kept at least as strict as the RTT sink):
 *   - Nothing is ever formatted inside a masked window: vsnprintf runs on
 *     the caller stack before wch_log_write() is entered; the masked window
 *     covers only the ring memcpy (<= 256 B, same bound the RTT sink had)
 *     and, at most, the DMA kick register writes. It never waits for the
 *     UART, the DMA, the scheduler or a host.
 *   - Producers serialize per call by saving/masking MIE and MPIE through
 *     GINTENR; that single window excludes other tasks AND
 *     interrupt-context producers by construction.
 *   - The DMA TC ISR only consumes (tail, g_dma_end/busy, descriptor);
 *     producers only produce (head). g_log_head is published with a fence
 *     AFTER the payload copy, so a segment started by the ISR never reads
 *     half-written bytes; a producer seeing g_dma_busy==false can safely
 *     program the channel because the only writer of that flag (the ISR)
 *     cannot run inside the masked window.
 *   - Fragmented stream logs (NimBLE ACL hex: header, per-byte fragments,
 *     final newline arrive as separate calls) keep line_start semantics:
 *     "[D] " prefix once per logical line, no per-fragment prefix/newline,
 *     one masked window per fragmented call so a line stays contiguous.
 *
 * Backpressure: the ring is bounded (UART_LOG_RING_SIZE). A full ring drops
 * the enqueue whole and counts it (g_wch_log_drop_*); nothing blocks on a
 * serial terminal that drains slower than the producers. The heartbeat
 * prints the counters and they are readable via gdb at any time.
 *
 * Emergency paths (configASSERT, HardFault/NMI/WWDG/stub traps, FreeRTOS
 * hooks, _exit) may run with interrupts disabled and cannot rely on the DMA
 * TC interrupt: wch_log_panic() first parks DMA with a BOUNDED poll of the
 * hardware TC flag (DMA completes without CPU help; no IRQ needed), then
 * takes the UART over for polled output with per-byte spin caps. Bounded
 * everywhere: a dead UART degrades to counted loss, never a hang.
 *
 * EasyLogger (third_party/easylogger, MIT, upstream commit 806328e) is the
 * record logger for app code: elog_port_output() lands on the same ring,
 * and its output lock suspends the scheduler (formatting happens inside
 * that lock, so it must NOT be an interrupt mask).
 */

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "wch_irq.h"
#include <stdio.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"

#include "ch32v20x.h"
#include "console/console.h"
#include "elog.h"
#include "wch_log.h"

/* ---------------- UART / DMA hardware configuration ----------------
 *
 * USART1 on PA9 TX / PA10 RX, 921600 8N1, no flow control, default pin
 * mapping. PA10 is configured as the USART RX input only - nothing receives
 * (no shell); RXNE/ORE stay unpolled with their interrupt masked.
 *
 * USART1_TX maps to DMA1 channel 4 (CH32FV2x_V3xRM v2.5, fig. 11-1 "DMA1
 * request mapping"; the same table the SDK EVT USART_DMA example uses for
 * USART2/3, which cross-checks USART1_TX=CH4 by the table's structure).
 * CH32V208 keeps PFIC vector DMA1_Channel4_IRQn (irq 30).
 *
 * PFIC priority 0x80: with interrupt nesting OFF (INTSYSCR=0, see startup)
 * nothing preempts anyway; this only orders the DMA TC ISR behind the radio
 * (LLE 0x20 / BB 0x40 / TIM3 0x40) when both pend simultaneously. The ISR
 * body is index arithmetic + register writes - no logging, no RTOS calls. */
#define UART_LOG_BAUD           921600
#define UART_LOG_RING_SIZE      WCH_LOG_RING_SIZE   /* power of two */
#define UART_LOG_RING_MASK      ( UART_LOG_RING_SIZE - 1u )
#define UART_LOG_LINE_MAX       256     /* console/NPL fragment stack buffer */

#define UART_LOG_DMA_PRIO       0x80
#define UART_LOG_TE_LIMIT       8u      /* consecutive TE before parking DMA */

/* Monotonic 32-bit ring indices (mask on dereference only): used/free
 * arithmetic is wrap-proof in u32 and stays valid across rollover. */
#define WCH_LOG_CANARY_WORD     0x5A5AA5A5UL

struct wch_log_storage g_wch_log_storage;
#define g_log_ring (g_wch_log_storage.ring)

static volatile uint32_t g_log_head;    /* producers publish AFTER copy+fence */
static volatile uint32_t g_log_tail;    /* only the DMA TC ISR advances       */
static volatile uint32_t g_dma_end;     /* ring index past the in-flight seg  */
static volatile bool     g_dma_busy;    /* segment programmed in CH4          */
static volatile bool     g_dma_dead;    /* parked after repeated TE           */
static volatile bool     g_panic_mode;  /* polled output owns the UART        */

/* gdb-readable counters (struct copy in wch_log_get_stats). */
uint32_t g_wch_log_drop_bytes;
uint32_t g_wch_log_drop_msgs;
uint32_t g_wch_log_dma_errors;
uint32_t g_wch_log_panic_lost;
static uint32_t s_te_streak;

static void wch_log_panic_write( const char *p, size_t n );

/* ---------------------------------------------------------------- */
/* Producer-side interrupt masking (mirrors the old SEGGER_RTT_LOCK) */
/* ---------------------------------------------------------------- */

static inline uint32_t log_irq_save( void )
{
    return wch_irq_save();
}

static inline void log_irq_restore( uint32_t mie )
{
    wch_irq_restore(mie);
}

/* ---------------------------------------------------------------- */
/* DMA ring core.  ring_put()/dma_start() run either inside a producer */
/* masked window or from the DMA ISR - both exclude every other caller */
/* by construction, so they take no locks of their own.               */
/* ---------------------------------------------------------------- */

static void dma_start( void )
{
    uint32_t tail = g_log_tail;
    uint32_t len = g_log_head - tail;
    uint32_t start;

    if( len == 0 )
    {
        g_dma_busy = false;
        return;
    }
    /* One contiguous segment per transfer: clip at the physical end of the
     * buffer; the TC ISR chains the wrapped remainder. */
    start = tail & UART_LOG_RING_MASK;
    if( len > UART_LOG_RING_SIZE - start )
    {
        len = UART_LOG_RING_SIZE - start;
    }
    g_dma_end = tail + len;
    g_dma_busy = true;

    /* Make the payload stores globally visible before the DMA sees them. */
    __asm__ volatile ( "fence rw, rw" ::: "memory" );

    /* Channel is idle here (programmed only when not busy), so registers
     * can be rewritten right after disabling EN. PADDR stays fixed. */
    DMA1_Channel4->CFGR &= ~DMA_CFGR1_EN;
    DMA1_Channel4->CNTR = len;
    DMA1_Channel4->MADDR = ( uint32_t ) ( uintptr_t ) &g_log_ring[ start ];
    DMA1_Channel4->CFGR |= DMA_CFGR1_EN;
}

static void ring_put( const char *p, size_t len )
{
    uint32_t head = g_log_head;
    uint32_t tail = g_log_tail;
    uint32_t pos, first;

    if( len == 0 )
    {
        return;
    }
    if( g_dma_dead || g_panic_mode ||
        ( len > UART_LOG_RING_SIZE - ( head - tail ) ) )
    {
        g_wch_log_drop_bytes += ( uint32_t ) len;
        g_wch_log_drop_msgs++;
        return;
    }
    pos = head & UART_LOG_RING_MASK;
    first = UART_LOG_RING_SIZE - pos;
    if( first > len )
    {
        first = ( uint32_t ) len;
    }
    memcpy( &g_log_ring[ pos ], p, first );
    memcpy( &g_log_ring[ 0 ], p + first, len - first );

    __asm__ volatile ( "fence rw, rw" ::: "memory" );
    g_log_head = head + len;    /* publish AFTER the payload is complete */

    if( !g_dma_busy )
    {
        dma_start();            /* masked window: no ISR can interleave */
    }
}

void wch_log_write( const void *data, size_t len )
{
    uint32_t mie;

    if( data == NULL || len == 0 )
    {
        return;
    }
    if( g_panic_mode )
    {
        /* After takeover everything is polled; see panic_write(). */
        wch_log_panic_write( data, len );
        return;
    }
    mie = log_irq_save();
    ring_put( data, len );
    log_irq_restore( mie );
}

/* ---------------------------------------------------------------- */
/* DMA1_CH4 completion interrupt                                    */
/* ---------------------------------------------------------------- */

void wch_log_dma_irq( void )
{
    uint32_t intfr = DMA1->INTFR;

    if( intfr & DMA_GIF4 )
    {
        DMA1->INTFCR = DMA_CGIF4 | DMA_CTCIF4 | DMA_CTEIF4;

        if( intfr & DMA_TEIF4 )
        {
            /* Bus error: salvage is not attempted - drop the failed
             * segment, count it, and retry. After repeated failures park
             * the channel for good (logs then drop, nothing hangs). */
            g_wch_log_dma_errors++;
            g_log_tail = g_dma_end;
            if( ++s_te_streak >= UART_LOG_TE_LIMIT )
            {
                g_dma_dead = true;
                g_dma_busy = false;
                DMA1_Channel4->CFGR &= ~( DMA_CFGR1_EN | DMA_CFGR1_TCIE |
                                          DMA_CFGR1_TEIE );
                return;
            }
        }
        else
        {
            s_te_streak = 0;
            g_log_tail = g_dma_end;
        }

        if( g_log_head != g_log_tail )
        {
            dma_start();        /* append-while-sending: chain next segment */
        }
        else
        {
            g_dma_busy = false;
        }
    }
}

/* ---------------------------------------------------------------- */
/* Emergency (polled, bounded) output                               */
/* ---------------------------------------------------------------- */

/* ~15 ms worst case at ~30 cycles/iteration of 96 MHz - comfortably above
 * one full ring (1024 B @ 921600 8N1 ~= 11.1 ms) so a healthy UART never
 * hits the cap, while a dead one cannot spin forever. */
#define PANIC_SPIN_MAX  ( 500000u )

static void panic_enter( void )
{
    if( g_panic_mode )
    {
        return;
    }
    g_panic_mode = true;

    /* Stop further DMA interrupts; the flag polling below needs no IRQ. */
    DMA1_Channel4->CFGR &= ~( DMA_CFGR1_TCIE | DMA_CFGR1_TEIE );

    if( g_dma_busy && !g_dma_dead )
    {
        uint32_t spin = PANIC_SPIN_MAX;

        /* GIF also includes the half-transfer flag, even with HTIE off.
         * Only completion or transfer error permits taking the channel. */
        while( ( ( DMA1->INTFR & ( DMA_TCIF4 | DMA_TEIF4 ) ) == 0 ) && --spin )
        {
        }
        /* Whether or not it completed, the segment is abandoned; skip it
         * so ring indices stay consistent if anyone inspects them later. */
        g_log_tail = g_dma_end;
        g_dma_busy = false;
    }
    DMA1_Channel4->CFGR &= ~DMA_CFGR1_EN;
    /* CPU writes to DATAR work regardless of the USART DMAT gate. */
}

void wch_log_panic_write( const char *p, size_t n )
{
    size_t i;

    panic_enter();
    for( i = 0; i < n; i++ )
    {
        uint32_t spin = 20000;    /* ~ >10 byte times: bounded per byte */

        while( ( ( USART1->STATR & USART_FLAG_TXE ) == 0 ) && --spin )
        {
        }
        if( spin == 0 )
        {
            /* UART TXE not progressing (e.g. a disabled peripheral clock): report
             * the loss in the counters instead of hanging the fault path. */
            g_wch_log_panic_lost += ( uint32_t ) ( n - i );
            return;
        }
        USART1->DATAR = ( uint8_t ) p[ i ];
    }
}

void wch_log_panic( const char *fmt, ... )
{
    char buf[ 192 ];
    va_list args;
    int n;

    va_start( args, fmt );
    n = vsnprintf( buf, sizeof( buf ), fmt, args );
    va_end( args );
    if( n <= 0 )
    {
        return;
    }
    if( ( size_t ) n >= sizeof( buf ) )
    {
        n = ( int ) sizeof( buf ) - 1;
    }
    wch_log_panic_write( buf, ( size_t ) n );
}

/* ---------------------------------------------------------------- */
/* EasyLogger port (see third_party/easylogger/port/elog_port.c)    */
/* ---------------------------------------------------------------- */

ElogErrCode elog_port_init( void )
{
    /* The UART/DMA hardware is brought up by wch_log_init(); EasyLogger
     * itself has no per-platform state beyond this port. */
    return ELOG_NO_ERR;
}

void elog_port_deinit( void )
{
}

void elog_port_output( const char *log, size_t size )
{
    wch_log_write( log, size );
}

/* elog.c formats each record (vsnprintf into its static buffer) INSIDE
 * this lock, so the lock must not mask interrupts - suspend the scheduler
 * instead. Other producers (NPL sink/console) mask IRQs per call and can
 * interleave between records; that matches the old RTT behaviour. */
void elog_port_output_lock( void )
{
    vTaskSuspendAll();
}

void elog_port_output_unlock( void )
{
    xTaskResumeAll();
}

const char *elog_port_get_time( void )
{
    return "";    /* ELOG_FMT_TIME disabled in config/elog_cfg.h */
}

const char *elog_port_get_p_info( void )
{
    return "";    /* single process */
}

const char *elog_port_get_t_info( void )
{
    return "";    /* ELOG_FMT_T_INFO disabled */
}

/* ---------------------------------------------------------------- */
/* NimBLE NPL log sink (called from config/nimble/nimble_npl_os_log.h) */
/* ---------------------------------------------------------------- */

/* NimBLE emits ACL headers, individual hex bytes and the final newline in
 * separate calls. Respect the caller's stream: "[D] " prefix once per
 * logical line, fragments pass through untouched, long dumps neither get
 * truncated nor prefixed per byte. One masked window per call keeps the
 * fragments of a single call contiguous in the ring. */
void ble_npl_log_uart_put( char lvl, const char *fmt, va_list args )
{
    char buf[ UART_LOG_LINE_MAX ];
    static bool line_start = true;
    uint32_t mie;
    int n;

    n = vsnprintf( buf, sizeof( buf ), fmt, args );
    if( n <= 0 )
    {
        return;
    }
    if( ( size_t ) n >= sizeof( buf ) )
    {
        n = ( int ) sizeof( buf ) - 1;
    }

    mie = log_irq_save();
    for( int pos = 0; pos < n; )
    {
        int end = pos;

        if( line_start )
        {
            const char prefix[] = { '[', lvl, ']', ' ' };
            ring_put( prefix, sizeof prefix );
        }
        while( end < n && buf[ end++ ] != '\n' )
        {
        }
        line_start = buf[ end - 1 ] == '\n';
        ring_put( buf + pos, ( size_t ) ( end - pos ) );
        pos = end;
    }
    log_irq_restore( mie );
}

/* ---------------------------------------------------------------- */
/* console facade (config/console/console.h, used by NimBLE host and app) */
/* ---------------------------------------------------------------- */

void console_write( const void *data, size_t len )
{
    wch_log_write( data, len );
}

int console_printf( const char *fmt, ... )
{
    char buf[ UART_LOG_LINE_MAX ];
    va_list args;
    int n;

    va_start( args, fmt );
    n = vsnprintf( buf, sizeof( buf ), fmt, args );
    va_end( args );

    if( n > 0 )
    {
        if( ( size_t ) n >= sizeof( buf ) )
        {
            n = ( int ) sizeof( buf ) - 1;    /* truncated; NUL-terminated */
        }
        wch_log_write( buf, ( size_t ) n );
    }
    return n;
}

int console_init( void )
{
    wch_log_init();
    return 0;
}

void console_flush( void )
{
    /* Bounded drain for diagnostics: wait until the ring empties (~11 ms
     * worst case) or the cap expires; never blocks the callers above it. */
    uint32_t spin = PANIC_SPIN_MAX;

    while( ( g_log_head != g_log_tail ) && --spin )
    {
    }
}

/* ---------------------------------------------------------------- */
/* Init                                                              */
/* ---------------------------------------------------------------- */

void wch_log_get_stats( struct wch_log_stats *out )
{
    if( out == NULL )
    {
        return;
    }
    out->drop_bytes = g_wch_log_drop_bytes;
    out->drop_msgs = g_wch_log_drop_msgs;
    out->dma_errors = g_wch_log_dma_errors;
    out->dma_dead = g_dma_dead;
    out->panic_mode = g_panic_mode;
    out->panic_lost = g_wch_log_panic_lost;
}

void wch_log_init( void )
{
    GPIO_InitTypeDef gpio = { 0 };
    USART_InitTypeDef usart = { 0 };
    DMA_InitTypeDef dma = { 0 };

    for( int i = 0; i < 4; i++ )
    {
        g_wch_log_canary_lo[ i ] = WCH_LOG_CANARY_WORD;
        g_wch_log_canary_hi[ i ] = WCH_LOG_CANARY_WORD;
    }

    /* PA9 = USART1 TX (AF push-pull), PA10 = USART1 RX (floating input,
     * receive logic idle - no shell, RX interrupt never enabled). */
    RCC_APB2PeriphClockCmd( RCC_APB2Periph_GPIOA | RCC_APB2Periph_USART1 |
                            RCC_APB2Periph_AFIO, ENABLE );
    AFIO->PCFR1 &= ~AFIO_PCFR1_USART1_REMAP;
    gpio.GPIO_Pin = GPIO_Pin_9;
    gpio.GPIO_Speed = GPIO_Speed_50MHz;
    gpio.GPIO_Mode = GPIO_Mode_AF_PP;
    GPIO_Init( GPIOA, &gpio );
    gpio.GPIO_Pin = GPIO_Pin_10;
    gpio.GPIO_Mode = GPIO_Mode_IN_FLOATING;
    GPIO_Init( GPIOA, &gpio );

    usart.USART_BaudRate = UART_LOG_BAUD;
    usart.USART_WordLength = USART_WordLength_8b;
    usart.USART_StopBits = USART_StopBits_1;
    usart.USART_Parity = USART_Parity_No;
    usart.USART_HardwareFlowControl = USART_HardwareFlowControl_None;
    usart.USART_Mode = USART_Mode_Tx | USART_Mode_Rx;
    USART_Init( USART1, &usart );
    USART_Cmd( USART1, ENABLE );

    /* DMA1_CH4: USART1_TX, memory->peripheral, bytes, MINC, normal mode.
     * Segments are reprogrammed per transfer (dma_start/ISR); PADDR fixed. */
    RCC_AHBPeriphClockCmd( RCC_AHBPeriph_DMA1, ENABLE );
    DMA_DeInit( DMA1_Channel4 );
    dma.DMA_PeripheralBaseAddr = ( uint32_t ) ( uintptr_t ) &USART1->DATAR;
    dma.DMA_MemoryBaseAddr = ( uint32_t ) ( uintptr_t ) g_log_ring;
    dma.DMA_DIR = DMA_DIR_PeripheralDST;
    dma.DMA_BufferSize = 1;    /* real length set per segment */
    dma.DMA_PeripheralInc = DMA_PeripheralInc_Disable;
    dma.DMA_MemoryInc = DMA_MemoryInc_Enable;
    dma.DMA_PeripheralDataSize = DMA_PeripheralDataSize_Byte;
    dma.DMA_MemoryDataSize = DMA_MemoryDataSize_Byte;
    dma.DMA_Mode = DMA_Mode_Normal;
    dma.DMA_Priority = DMA_Priority_Low;    /* log traffic yields to radio DMA */
    dma.DMA_M2M = DMA_M2M_Disable;
    DMA_Init( DMA1_Channel4, &dma );
    DMA1_Channel4->CFGR |= DMA_CFGR1_TCIE | DMA_CFGR1_TEIE;
    USART_DMACmd( USART1, USART_DMAReq_Tx, ENABLE );

    NVIC_SetPriority( DMA1_Channel4_IRQn, UART_LOG_DMA_PRIO );
    NVIC_EnableIRQ( DMA1_Channel4_IRQn );

    /* EasyLogger: minimal record format "[<L>][<tag>] msg\n". */
    elog_init();
    for( int lvl = 0; lvl < ELOG_LVL_TOTAL_NUM; lvl++ )
    {
        elog_set_fmt( ( uint8_t ) lvl, ELOG_FMT_LVL | ELOG_FMT_TAG );
    }
    elog_start();
}

/* Called with IRQs masked by the tickless port. TC alone is insufficient:
 * DMA may have another segment queued while the UART shifts the last byte. */
int wch_log_can_sleep(void)
{
    return !g_panic_mode && !g_dma_busy && g_log_head == g_log_tail &&
           (USART1->STATR & USART_STATR_TC);
}
