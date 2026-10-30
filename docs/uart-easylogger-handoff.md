# EasyLogger + USART1 DMA 日志迁移 prompt

请直接在 `/Volumes/aigo_1t/GitRepo/wch-trouble-workspace/ch32v208-nimble` 完成日志迁移。
先看项目说明、适用的 AGENTS.md 与当前 Git 变更，保留其他人的工作；不要只写方案。
用户允许自行用 probe-rs 烧录、调试和通过 ADB 操作已连接手机。不要创建 Android APK。

## 目标和边界

- 使用 EasyLogger，串口为 **USART1、PA9 TX / PA10 RX、921600 baud、8N1、无流控、默认引脚映射**。
- 使用 **DMA异步发送**。运行时日志不等待UART完成，不在关中断区格式化或等待DMA；不使用阻塞SDI。
- 不再用 probe-rs RTT 作为 BLE 运行时日志。可保留 RAM 异常现场/飞行记录仪供停机后GDB读取。
- **HSE保留，LSI不得启用，LSE可以不用**。当前96MHz主频和TIM3 1MHz计时均来自HSE。
- 手机 nRF Connect 的过滤器必须保持用户原样：`CH, -75 dBm`，厂商排除项
  `Apple, Microsoft, Samsung, Google, Exposure Notification Service`。不要打开、清空或调整过滤器。
- 用户已要求暂停长时间测试；只做必要的短测，不自行启动20分钟/长时间可靠性循环。
- 不顺手更改RF时序、BLE寄存器、PHY协商或任务优先级。现有寄存器注释不一定准确，
  DMA映射、时钟、清标志方式等应核对本地SDK/芯片手册。

## 当前结构与必须覆盖的日志入口

目前 `port/console_rtt.c` 提供 `console_write/console_printf` 和 NimBLE日志sink，
`config/nimble/nimble_npl_os_log.h` 是适配头。`port/syscalls.c` 的 `_write/_write_r`
仍输出RTT，`app/main.c`、`app/ch32v20x_it.c`、`config/FreeRTOSConfig.h` 等还直接调用
`SEGGER_RTT_printf/WriteString`。请统一迁移这些入口，覆盖banner、应用、NimBLE、
newlib printf和异常/assert日志；不要只迁移console_printf。保留已修复的newlib `_r`
系统调用覆盖，不能重新链接到Linux ecall桩。

NimBLE会把ACL头、各个hex字节、最后的换行拆成多个日志调用。当前sink已经修正为
遵循调用方换行，不能再给每个片段补时间戳/级别/换行。请在EasyLogger适配层保证：

```
[D] ble_hs_hci_acl_tx(): 0x01 0x00 0x07 ...
```

整包一行，前缀一次。长dump不得静默截断、越界，也不能每字节一行。普通日志和
hex输出的并发规则要明确；尽量在本项目适配层处理，不修改同级共享的`../nimble`源码。

EasyLogger应固定上游版本/commit，保留许可证，纳入可重复的CMake构建。

## DMA与实时性要求

- 日志入队时复制数据。DMA不得引用函数栈、复用中的格式化buffer或已释放的mbuf。
- 正确处理环形缓冲区回卷、连续DMA段、发送过程中追加数据、空队列启动和完成中断竞态。
- 缓冲区满采用有界丢弃并保留可读丢弃计数；不能无限等锁、等DMA、等电脑或等串口终端。
- DMA错误应清标志并有诊断，不能永久卡busy；ISR内不递归打日志，不调用会阻塞的RTOS API。
- 当前启动配置HPE/中断嵌套关闭。中断属性按现有软件保存上下文的约定，不直接套用
  依赖HPE的`WCH-Interrupt-fast`模板；DMA优先级不要干扰BB/LLE/TIM3实时路径。
- assert/异常可能已经关中断，应有明确的有界紧急输出策略，避免等不到DMA中断而永久等待。
- 当前RAM已用约64992/65536B，包含固定16KB newlib heap和20KB FreeRTOS heap。
  设计DMA buffer前核算静态RAM；删除RTT后可回收其缓冲区/控制块。不要悄悄压缩任务栈，
  不要只看链接成功就忽略新增格式化栈占用。
- PA10配置成串口RX即可；本任务不要求增加命令shell或远程控制功能。

## 主机脚本与验证

`cmake --build build`构建，`python3 scripts/check-image.py build/ch32v208_nimble.elf build/boot.elf`
检查两个ELF。probe-rs下载完整镜像约65–70秒，无输出时不要提前中断。

`run.sh`目前只烧录和正常reset，**不自动附加RTT**；`attach.sh`仍是明确标注侵入性的RTT
取证工具。请将日常日志脚本改为串口读取，明确选择端口、921600、8N1，支持Ctrl-C关闭。
macOS上若需自定义波特率ioctl，应实际设置成功，不能静默回退为115200。不得随意打开其他
设备串口。可列举端口；若没有已接到PA9/PA10的USB-UART，请说明物理接线验证缺口，不能
把烧录或寄存器检查当作串口实收通过。

最少验证：两个固件目标能构建；端口/波特率/引脚和DMA配置核对；实际串口banner与ACL
hex行可读；断开/停止串口接收不影响板端继续运行；日志突发不会死锁。若接线可用，用
已连接手机做短轮扫描、连接、发现三个服务、echo和主动断开重连，验证日志开启时也能工作。
若更改phone_ble工具，保留其目标地址校验、过滤器保护和失败不自动重试的行为。

手机工具：`tools/android-ble/phone_ble.py`，ADB serial `80836814`，当前测试地址
`D0:54:7B:21:AE:B1`，名称`CH32V208-NIMBLE`。它操作现有nRF Connect，不安装APK。
7项离线检查：`python3 -m unittest discover -s tools/android-ble -p 'test_*.py'`。

## 已知问题，不要误报“全部修复”

详见`docs/android-interop-2026-10-30.md`。当前手机能发现Generic Access(0x1800)、
Generic Attribute(0x1801)和测试服务，1M/2M短轮回读与重连已有通过记录。

本机probe-rs0.32跟踪已证明此芯片`sbcs=0`，RTT读RAM选择`ProgramBuffer`并通过
`halted_access`反复暂停/恢复CPU。开RTT时已复现RF TX持续失败、CPU心跳继续，
即使指定`no-block-skip`也存在。改串口是消除该运行时取日志路径对CPU的干扰，
不能据此宣称所有独立运行问题已解决。

另有一次独立运行在`vListInsert`卡住，LL任务事件节点重复入队形成自环。移植层
关中断屏障、切换前临界区断言、事件节点重复入队断言已加入，尚无充分验证证明根因
完全修复；不要移除这些取证点。TX失败清理也已有故障注入验证，但不能恢复所有
RTT附加后出现的硬件挂起。测试注入代码已移除，当前正常镜像不含它。

请完成实现、短测并汇报修改、实测证据和剩余限制，不启动长测，不push。
