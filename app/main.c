/*
 * main.c - CH32V208 NimBLE bring-up entry.
 *
 * Called from handle_reset (startup_ch32v20x_D8W.S) after SystemInit()
 * and with global interrupts already enabled (mstatus = 0x88).
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

/* Linker-provided boundaries (ld/Link.ld). */
extern char _heap_start[];
extern char _heap_end[];
extern char _susrstack[];
extern char _eusrstack[];

void app_ble_start( void ) __attribute__( ( weak ) );

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
    ( void ) xTask;
    taskDISABLE_INTERRUPTS();
    SEGGER_RTT_printf( 0, "\r\n[stack-overflow] task: %s\r\n", pcTaskName );
    for( ;; )
    {
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
        SEGGER_RTT_printf( 0, "[app] alive, uptime=%u ticks, sysclk=%u\r\n",
                           ( unsigned ) xTaskGetTickCount(),
                           ( unsigned ) SystemCoreClock );
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
    SEGGER_RTT_WriteString( 0, "==============================\r\n" );

    if( xTaskCreate( app_task, "app", 512, NULL, tskIDLE_PRIORITY + 2, NULL ) != pdPASS )
    {
        SEGGER_RTT_WriteString( 0, "[main] xTaskCreate(app) failed\r\n" );
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
