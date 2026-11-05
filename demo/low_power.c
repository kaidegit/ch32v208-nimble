/* Standalone silicon experiment; no RTOS, BLE, DMA or periodic SysTick IRQ.
 * See docs/low-power-demo.md. All modes wait 10 s after EVERY reset.
 * Reference: WCH BLE/HAL/{SLEEP,RTC}.c and QingKe V4 manual ch.6.
 */
#include "ch32v20x.h"
#include "ch32v20x_pwr.h"
#include "ch32v20x_rtc.h"
#include "ch32v20x_exti.h"
#include "wch_irq.h"

#ifndef LP_PARK_IO
#define LP_PARK_IO 0
#endif

#ifndef LP_MODE
#define LP_MODE 4
#endif
#ifndef LP_LSE
#define LP_LSE 0
#endif
#ifndef LP_PERIOD_SECONDS
#define LP_PERIOD_SECONDS 5
#endif

/* These numeric values are also the public CH32_LP_MODE CMake values. */
enum low_power_mode {
    LP_RUN = 0,
    LP_SLEEP,
    LP_STOP_IRQ,
    LP_STOP_MASKED_WFI,
    LP_STOP_EVENT,
    LP_STOP_PENDING,
};

/* The LSI value is nominal only; this standalone demo does not measure it. */
#define LP_RTC_HZ (LP_LSE ? 16384u : 16000u)

volatile uint32_t g_stub_trap_words[8], g_exc_trap_words[40];
volatile uint32_t lp_wakes, lp_irq_count, lp_irq_before_restore;
volatile uint32_t lp_return_cfgr, lp_isr_cfgr, lp_sleeping;
static volatile uint32_t clock_ready;
static const char *const modes[] = {"run",        "sleep",       "stop-irq", "stop-masked-wfi",
                                    "stop-event", "stop-pending"};

static void putc_uart(char c)
{
    while (!(USART1->STATR & USART_STATR_TXE)) {
    }
    USART1->DATAR = (uint8_t)c;
}
static void puts_uart(const char *s)
{
    while (*s) {
        putc_uart(*s++);
    }
}
static void number(uint32_t v)
{
    char s[10];
    unsigned n = 0;
    do {
        s[n++] = '0' + v % 10;
        v /= 10;
    } while (v);
    while (n) {
        putc_uart(s[--n]);
    }
}
static void hex(uint32_t v)
{
    for (int i = 28; i >= 0; i -= 4) {
        putc_uart("0123456789abcdef"[(v >> i) & 15]);
    }
}
static void flush_uart(void)
{
    while (!(USART1->STATR & USART_STATR_TC)) {
    }
}
static void delay_ms(uint32_t ms)
{
    /* Polled core counter; never enable SysTick interrupt. */
    SysTick->CTLR = 0;
    SysTick->CNT = 0;
    SysTick->CMP = ~(uint64_t)0;
    SysTick->SR = 0;
    SysTick->CTLR = 5;
    while (SysTick->CNT < (uint64_t)SystemCoreClock / 1000 * ms) {
    }
    SysTick->CTLR = 0;
}
void wch_stub_trap_report(uint32_t cause, uint32_t pc, uint32_t val)
{
    puts_uart("TRAP ");
    hex(cause);
    putc_uart(' ');
    hex(pc);
    putc_uart(' ');
    hex(val);
    for (;;) {
    }
}

void RTCAlarm_IRQHandler(void) __attribute__((interrupt));
void RTCAlarm_IRQHandler(void)
{
    lp_isr_cfgr = RCC->CFGR0;
    if (lp_sleeping && !clock_ready) {
        ++lp_irq_before_restore;
        /* Deliberately test the vendor IRQ-first path, but restore BEFORE
         * any future RTOS API. Masked/event paths restore in main instead. */
        SystemInit();
        clock_ready = 1;
    }
    ++lp_irq_count;
    EXTI_ClearITPendingBit(EXTI_Line17);
    RTC_ClearITPendingBit(RTC_IT_ALR);
    RTC_WaitForLastTask();
}

/* WCH PWR/Stop_Mode_RtcWakeUp GPIO_ALL_IPD reference. Deliberately
 * opt-in: disables the debug port AFTER the mandatory boot window. */
static void park_io(void)
{
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOA | RCC_APB2Periph_GPIOB | RCC_APB2Periph_GPIOC |
                               RCC_APB2Periph_GPIOD | RCC_APB2Periph_AFIO,
                           ENABLE);
    GPIO_PinRemapConfig(GPIO_Remap_SWJ_Disable, ENABLE);
    GPIO_InitTypeDef gpio = {0};
    gpio.GPIO_Pin = GPIO_Pin_All;
    gpio.GPIO_Mode = GPIO_Mode_IPD;
    gpio.GPIO_Speed = GPIO_Speed_2MHz;
    GPIO_Init(GPIOA, &gpio);
    GPIO_Init(GPIOB, &gpio);
#if LP_LSE
    gpio.GPIO_Pin &= ~(GPIO_Pin_14 | GPIO_Pin_15); /* crystal pins */
#endif
    GPIO_Init(GPIOC, &gpio);
    gpio.GPIO_Pin = GPIO_Pin_All;
    GPIO_Init(GPIOD, &gpio);
}

static void rtc_init(void)
{
    RCC_APB1PeriphClockCmd(RCC_APB1Periph_PWR | RCC_APB1Periph_BKP, ENABLE);
    PWR_BackupAccessCmd(ENABLE);
    /* Demo owns RTC. Reset backup domain to avoid a stale clock selection.
     * This clears RTC / backup registers; never use on a timekeeping image. */
    RCC_BackupResetCmd(ENABLE);
    RCC_BackupResetCmd(DISABLE);
#if LP_LSE
    RCC_LSEConfig(RCC_LSE_ON);
    for (unsigned i = 0; !(RCC->BDCTLR & RCC_LSERDY); ++i) {
        if (i == 500) {
            puts_uart("LSE failed; staying RUN\r\n");
            for (;;) {
            }
        }
        delay_ms(10);
    }
    RCC_LSICmd(DISABLE);
    RCC_RTCCLKConfig(RCC_RTCCLKSource_LSE);
#else
    RCC_LSICmd(ENABLE);
    while (!(RCC->RSTSCKR & RCC_LSIRDY)) {
    }
    RCC_RTCCLKConfig(RCC_RTCCLKSource_LSI);
#endif
    RCC_RTCCLKCmd(ENABLE);
    RTC_WaitForSynchro();
    RTC_WaitForLastTask();
    RTC_SetPrescaler(1); /* same divide-by-two as WCH BLE HAL */
    RTC_WaitForLastTask();
    RTC_SetCounter(0);
    RTC_WaitForLastTask();
    RTC_ITConfig(RTC_IT_ALR, ENABLE);
    RTC_WaitForLastTask();
    EXTI->RTENR |= EXTI_Line17;
    EXTI->FTENR &= ~EXTI_Line17;
    if (LP_MODE == LP_STOP_EVENT) {
        EXTI->EVENR |= EXTI_Line17; /* explicit event; no ISR required */
        EXTI->INTENR &= ~EXTI_Line17;
        NVIC_DisableIRQ(RTCAlarm_IRQn);
    } else {
        EXTI->INTENR |= EXTI_Line17;
        EXTI->EVENR &= ~EXTI_Line17;
        NVIC_SetPriority(RTCAlarm_IRQn, 0x20);
        NVIC_EnableIRQ(RTCAlarm_IRQn);
    }
}

int main(void)
{
    SystemCoreClockUpdate();
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOA | RCC_APB2Periph_GPIOB | RCC_APB2Periph_GPIOC |
                               RCC_APB2Periph_USART1,
                           ENABLE);
    GPIO_InitTypeDef gpio = {0};
    /* Minimise floating-input consumption; preserve PA13/PA14 debug pins. */
    gpio.GPIO_Pin = GPIO_Pin_All & ~(GPIO_Pin_9 | GPIO_Pin_10 | GPIO_Pin_13 | GPIO_Pin_14);
    gpio.GPIO_Mode = GPIO_Mode_AIN;
    GPIO_Init(GPIOA, &gpio);
    gpio.GPIO_Pin = GPIO_Pin_All;
    GPIO_Init(GPIOB, &gpio);
    GPIO_Init(GPIOC, &gpio);
    gpio.GPIO_Pin = GPIO_Pin_9;
    gpio.GPIO_Mode = GPIO_Mode_AF_PP;
    gpio.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(GPIOA, &gpio);
    gpio.GPIO_Pin = GPIO_Pin_10;
    gpio.GPIO_Mode = GPIO_Mode_AIN;
    GPIO_Init(GPIOA, &gpio);
    USART_InitTypeDef uart = {0};
    uart.USART_BaudRate = 115200;
    uart.USART_WordLength = USART_WordLength_8b;
    uart.USART_StopBits = USART_StopBits_1;
    uart.USART_Mode = USART_Mode_Tx;
    USART_Init(USART1, &uart);
    USART_Cmd(USART1, ENABLE);
    puts_uart("\r\nLP DEMO mode=");
    puts_uart(modes[LP_MODE]);
    puts_uart(" sysclk=");
    number(SystemCoreClock);
    puts_uart(" chip=");
    hex(*(volatile uint32_t *)0x1ffff704);
    puts_uart(" rtc=");
    puts_uart(LP_LSE ? "LSE/2" : "LSI/2 (uncalibrated)");
    puts_uart(" park_io=");
    number(LP_PARK_IO);
    puts_uart("\r\n");
    for (unsigned i = 10; i; --i) {
        puts_uart("reflash window ");
        number(i);
        puts_uart("s\r\n");
        flush_uart();
        delay_ms(1000);
    }
    rtc_init();
    if (LP_PARK_IO) {
        puts_uart("parking GPIOA-D + disabling SWJ; UART becomes silent\r\n");
        flush_uart();
        park_io();
    }
    wch_irq_restore(0x88);
    for (;;) {
        if (!LP_PARK_IO) {
            puts_uart("enter ");
            puts_uart(modes[LP_MODE]);
            puts_uart(" cycle=");
            number(lp_wakes);
            puts_uart("\r\n");
            flush_uart();
        }
        uint32_t mask = wch_irq_save();
        RTC_ClearITPendingBit(RTC_IT_ALR);
        RTC_WaitForLastTask();
        EXTI_ClearITPendingBit(EXTI_Line17);
        NVIC_ClearPendingIRQ(RTCAlarm_IRQn);
        RTC_SetAlarm(RTC_GetCounter() + LP_RTC_HZ * LP_PERIOD_SECONDS);
        RTC_WaitForLastTask();
        uint32_t start = RTC_GetCounter();
        lp_sleeping = 1;
        clock_ready = (LP_MODE < LP_STOP_IRQ);
        uint32_t sctlr = NVIC->SCTLR;
        NVIC->SCTLR &= ~((1u << 1) | (1u << 2) | (1u << 3) | (1u << 4));
        if (LP_MODE < LP_STOP_MASKED_WFI) {
            wch_irq_restore(mask);
        }
        if (LP_MODE == LP_RUN) {
            uint32_t target = start + LP_RTC_HZ * LP_PERIOD_SECONDS;
            while ((int32_t)(RTC_GetCounter() - target) < 0) {
            }
        } else if (LP_MODE == LP_SLEEP) {
            __WFI();
        } else {
            if (LP_MODE == LP_STOP_PENDING) {
                NVIC->SCTLR |= 1u << 4; /* SEVONPEND */
            }
            PWR_EnterSTOPMode_RAM_LV(PWR_Regulator_LowPower, LP_MODE >= LP_STOP_EVENT
                                                                 ? PWR_STOPEntry_WFE
                                                                 : PWR_STOPEntry_WFI);
        }
        /* Keep IRQs masked for clock restoration on event/masked paths. */
        uint32_t after = wch_irq_save();
        lp_return_cfgr = RCC->CFGR0;
        if (!clock_ready) {
            SystemInit();
            clock_ready = 1;
        }
        lp_sleeping = 0;
        NVIC->SCTLR = sctlr;
        uint32_t elapsed = RTC_GetCounter() - start;
        if (LP_MODE == LP_STOP_EVENT) {
            RTC_ClearITPendingBit(RTC_IT_ALR);
            RTC_WaitForLastTask();
            EXTI_ClearITPendingBit(EXTI_Line17);
        }
        wch_irq_restore(LP_MODE >= LP_STOP_MASKED_WFI ? mask : after);
        ++lp_wakes;
        if (!LP_PARK_IO) {
            puts_uart("wake ticks=");
            number(elapsed);
            puts_uart(" irq=");
            number(lp_irq_count);
            puts_uart(" irq_before_clock=");
            number(lp_irq_before_restore);
            puts_uart(" return_cfgr=");
            hex(lp_return_cfgr);
            puts_uart(" isr_cfgr=");
            hex(lp_isr_cfgr);
            puts_uart(" pwr=");
            hex(PWR->CTLR);
            puts_uart(" restored_cfgr=");
            hex(RCC->CFGR0);
            puts_uart("\r\n");
            flush_uart();
        }
        delay_ms(100); /* visible, short awake interval for current traces */
    }
}
