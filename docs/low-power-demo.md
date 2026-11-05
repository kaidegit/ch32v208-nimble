# CH32V208 独立低功耗 demo

`demo/low_power.c` 用于比较芯片 RUN、SLEEP、STOP 及不同中断/事件唤醒方式，不链接 FreeRTOS 或 NimBLE。完整固件的使用、最终功耗截图和 GPIO 迁移要求见 [低功耗说明](low-power.md)。

**所有模式每次复位后都强制等待十秒**，输出倒计时后才进入低功耗。此保护不能通过脚本关闭。

## 构建

```sh
scripts/low-power-demo.sh stop-event --flash
```

省略 `--flash` 只编译。产物位于 `build-lp-模式名/low_power_demo.{elf,bin}`，串口为 **PA9 TX、115200 8N1**，与完整固件的 921600 不同。选项和目标定义位于 `cmake/low-power-demo.cmake`。

| 参数 | 默认值 | 用途 |
|---|---|---|
| `LP_PERIOD` | 5 | 名义休眠秒数，范围 1～100 |
| `LP_LSE` | OFF | 改用 LSE/2，当前板上未验证起振 |
| `LP_PARK_IO` | OFF | 十秒窗口后关闭 SWJ、GPIOA-D 设下拉并停止 UART 输出 |
| `SYSCLK_MHZ` | 96 | HSE/PLL 主频，可选 72/96/144 |

默认使用未校准 LSI/2，按名义 16 kHz 设置周期，只适合功耗与唤醒实验。醒来后运行约 100 ms，再次休眠。初始化会重置 RTC/备份域。

## 模式

| 模式 | 行为 |
|---|---|
| `run` | CPU 忙等 RTC，作为运行电流对照 |
| `sleep` | 普通 WFI，保留主时钟 |
| `stop-irq` | STOP/WFI，中断开放；RTC ISR 首先恢复系统时钟 |
| `stop-masked-wfi` | STOP/WFI，屏蔽 MIE/MPIE；实验模式，尚未验证可唤醒 |
| `stop-event` | STOP/WFE，屏蔽全局响应，以 EXTI17 event 唤醒，在主程序恢复时钟 |
| `stop-pending` | STOP/WFE，屏蔽全局响应，SEVONPEND=1；实验模式，尚未实测 |

`stop-event` 与 `stop-irq` 已实机确认周期唤醒。前者不进入 RTC ISR；后者先进入 ISR，所以必须在任何可能触发调度的服务之前恢复时钟。`stop-event` 是完整 FreeRTOS 移植采用的芯片层路径。

## GPIO 与测量

普通 demo 将未用 GPIOA/B/C 设为模拟输入，保留 UART TX 和调试脚。可选下拉/SWJ 对照档：

```sh
LP_PARK_IO=ON scripts/low-power-demo.sh stop-irq --flash
```

此选项仅用于当前芯片测试板。它会改变引脚状态，外接上拉、通信或其他外设时必须先修改引脚掩码。LSE 档保留 PC14/PC15。实际项目中的数字输入和唤醒引脚应按外部电路配置上下拉，不应统一设为模拟输入或下拉。

已测裸机基线为 **3.3 V MCU 支路、连接线拔掉、休眠平台约 22 µA**，并确认约五秒一次的唤醒脉冲。该平台值不包含完整 BLE 收发平均电流。

烧录后退出调试会话，拔掉 WCH-Link，让目标独立供电；复位并等待十秒窗口结束，再分别观察休眠平台与唤醒脉冲。用户反馈 WCH-Link 连接时可能出现峰间约 350 µA 平台，拔掉后消失，具体机制未验证，暂不继续排查。测量时还需避免 UART 线反向供电，不附加 GDB/RTT。STOP 时调试器可能无法连接，可用 BOOT＋RESET 恢复烧录；正常启动时恢复 BOOT 电平。

## 移植注意

裸机唤醒成功不等于可直接用在 RTOS。STOP 会停止 TIM3 cputime 和 SysTick，完整移植必须补偿经过时间、保留 tick 相位、重设定时截止点，并在恢复系统时钟和时间基准后才允许 ISR/调度运行。仅使用未校准的名义 LSI 频率不能保证 BLE 时序精度。
