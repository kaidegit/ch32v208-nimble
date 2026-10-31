/*
 * FreeRTOSConfig.h for CH32V208 (QingKe V4C, RV32IMAC) + the WCH-adapted
 * FreeRTOS V10.4.6 RISC-V port in third_party/freertos.
 *
 * Port contract notes (verified against portmacro.h/port.c/portASM.S):
 *  - configMTIME_BASE_ADDRESS / configMTIMECMP_BASE_ADDRESS must be 0:
 *    the QingKe V4 has no CLINT/mtime; port.c then programs the core
 *    SysTick (STK @0xE000F000) in vPortSetupTimerInterrupt() and defines
 *    SysTick_Handler itself (app/ch32v20x_it.c must NOT define it).
 *  - configISR_STACK_SIZE_WORDS must be defined (or the linker script
 *    must provide __freertos_irq_stack_top, which ld/Link.ld does not):
 *    port.c then allocates the ISR stack statically and portASM.S's
 *    xPortStartFirstTask points mscratch at it.
 *  - configMAX_PRIORITIES <= 32 (bitmap task selection).
 *  - portasmHANDLE_INTERRUPT is not needed: this WCH portASM.S variant
 *    has no generic trap handler, the PFIC runs in vector mode
 *    (mtvec = _vector_base | 3, set by the startup file) and every
 *    interrupt has its own vector-table entry.
 */

#ifndef FREERTOS_CONFIG_H
#define FREERTOS_CONFIG_H

/* Bring in the chip header (SysTick/NVIC inline helpers, IRQn_Type,
 * SystemCoreClock).  Relative path so this also compiles inside the
 * freertos_kernel library, whose include path has no SDK dirs.
 * Define the die select before the header: freertos_kernel is built
 * without sdk_periph's -DCH32V20x_D8W. */
#ifndef CH32V20x_D8W
#define CH32V20x_D8W
#endif
#include "../third_party/sdk/Peripheral/inc/ch32v20x.h"

#ifdef __cplusplus
extern "C" {
#endif
/* Declared by app/system_ch32v20x.c; kept here so configCPU_CLOCK_HZ
 * works in any translation unit (core_riscv.h does not declare it). */
extern uint32_t SystemCoreClock;
#ifdef __cplusplus
}
#endif

/* See https://www.freertos.org/Using-FreeRTOS-on-RISC-V.html */

/* No machine-timer/CLINT on QingKe V4: the tick comes from the core
 * SysTick peripheral (see port.c vPortSetupTimerInterrupt). */
#define configMTIME_BASE_ADDRESS        ( 0 )
#define configMTIMECMP_BASE_ADDRESS     ( 0 )

/* Statically allocated interrupt/ISR stack used by portASM.S
 * (xISRStackTop). Reserve 4 KB for interrupt handling. */
#define configISR_STACK_SIZE_WORDS      ( 4096 / sizeof( StackType_t ) )

#define configUSE_PREEMPTION            1
#define configUSE_IDLE_HOOK             0
#define configUSE_TICK_HOOK             0
#define configCPU_CLOCK_HZ              ( SystemCoreClock )   /* actual HCLK at scheduler startup */
#define configTICK_RATE_HZ              ( ( TickType_t ) 1000 )
#define configMAX_PRIORITIES            ( 15 )
#define configMINIMAL_STACK_SIZE        ( ( unsigned short ) 256 )
#define configTOTAL_HEAP_SIZE           ( ( size_t ) ( 16 * 1024 ) )
#define configMAX_TASK_NAME_LEN         ( 16 )
#define configUSE_16_BIT_TICKS          0
#define configIDLE_SHOULD_YIELD         1
#define configUSE_MUTEXES               1
#define configQUEUE_REGISTRY_SIZE       8
#define configCHECK_FOR_STACK_OVERFLOW  2
#define configSUPPORT_STATIC_ALLOCATION 1
#define configUSE_RECURSIVE_MUTEXES     1
#define configUSE_MALLOC_FAILED_HOOK    1
#define configUSE_APPLICATION_TASK_TAG  0
#define configUSE_COUNTING_SEMAPHORES   1
#define configUSE_TRACE_FACILITY        0
#define configGENERATE_RUN_TIME_STATS   0

/* Co-routine definitions. */
#define configUSE_CO_ROUTINES           0
#define configMAX_CO_ROUTINE_PRIORITIES ( 2 )

/* Software timer service (NimBLE NPL uses FreeRTOS timers). */
#define configUSE_TIMERS                1
#define configTIMER_TASK_PRIORITY       ( configMAX_PRIORITIES - 1 )
#define configTIMER_QUEUE_LENGTH        8
#define configTIMER_TASK_STACK_DEPTH    ( configMINIMAL_STACK_SIZE )

/* Set the following definitions to 1 to include the API function. */
#define INCLUDE_vTaskPrioritySet            1
#define INCLUDE_uxTaskPriorityGet           1
#define INCLUDE_vTaskDelete                 1
#define INCLUDE_vTaskSuspend                1
#define INCLUDE_vTaskDelayUntil             1
#define INCLUDE_vTaskDelay                  1
#define INCLUDE_xTaskGetSchedulerState      1
#define INCLUDE_xTaskGetCurrentTaskHandle   1
#define INCLUDE_eTaskGetState               0
#define INCLUDE_xTimerPendFunctionCall      1
#define INCLUDE_xTaskAbortDelay             0
#define INCLUDE_xTaskGetHandle              0
#define INCLUDE_xSemaphoreGetMutexHolder    1
#define INCLUDE_uxTaskGetStackHighWaterMark 1

/* vApplicationMallocFailedHook() / vApplicationStackOverflowHook() are
 * provided by app/main.c (hooks enabled above; no prototypes here since
 * this header is included before the FreeRTOS types exist). */

/* Assert: freeze with interrupts masked and report over the UART
 * (wch_log_panic - integer-only formatting, polled output that is safe
 * with interrupts disabled).  The line number is also stashed into fixed
 * RAM before printing: bring-up saw the print lost when the assert fired
 * with a broken stack (app/ch32v20x_it.c reads these back via gdb). */
extern volatile uint32_t g_config_assert_line;

#define configASSERT( x ) \
    do { \
        if( ( x ) == 0 ) \
        { \
            taskDISABLE_INTERRUPTS(); \
            g_config_assert_line = __LINE__; \
            wch_log_panic( "configASSERT failed: %s:%d\r\n", __FILE__, __LINE__ ); \
            for( ;; ) { } \
        } \
    } while( 0 )

#include "../port/wch_log.h"

#endif /* FREERTOS_CONFIG_H */
