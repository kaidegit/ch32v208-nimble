# 2026-10-30 UART DMA 适配复核与短测

实现提交为 `27416a7`。运行时日志已改为 EasyLogger/console/NPL/newlib ->
1KB复制环形队列 -> DMA1通道4 -> USART1，PA9 TX / PA10 RX，921600、8N1、无流控。
`log.sh`和`attach.sh`现在读取串口，不打开调试探针；固件不再链接RTT。

## 复核修正

- 入队关中断锁补`fence.i`，并保留编译器memory屏障。反汇编确认屏障位于
  清MIE之后、访问环形队列之前，避免重新引入之前遗漏的关中断流水线屏障。
- 金丝雀和DMA缓冲区改成同一结构体，保证前后相邻。原来三个独立变量在本次
  ELF中顺序实际相反，不能据此宣称布局由源代码保证。
- 显式关闭USART1引脚重映射，固定PA9/PA10；补`stdio.h`及日志格式参数类型，
  清理本次改动引入的隐式函数声明和格式检查警告。
- EasyLogger全部8个vendor文件（含LICENSE）逐文件对照上游，内容一致；
  VENDORED.md的完整commit误写，已修正为
  `806328e131836662fa83dd43364a53f699fd76ac`。原样保留上游空白和许可证。
- panic接管DMA原来等待GIF总标志，它包含半传输标志，即使HTIE关闭也可能置位。
  已改为等待TC或TE，避免在半传输时关闭通道、截断在途数据。
- 失败取证脚本新增UART丢弃、DMA状态/索引和金丝雀读数，仍校验完整64KB RAM。

两个目标构建和镜像检查通过；7项ADB工具离线检查通过。正常镜像RAM占用
65204/65536B（已包含保留的heap和栈），没有压缩任务栈。静态队列满会计数丢弃，
不会等待终端。无流控UART在终端关闭后仍发送，不能用板端零丢弃计数推断电脑
没有漏收；这里另外用GATT回读验证板端运行。

## 手机与串口同时运行

测试手机OPPO PEDM00、Android14、nRF Connect4.29.1，ADB serial为80836814。
USB-UART实际端口为`/dev/cu.usbmodemF3F38F0678B02`，pyserial通过macOS
IOSSIOSPEED设置921600，实际收到可解码banner和数据。未连接GDB/RTT进行BLE短测。

第一版复核镜像SHA256：
`22d02f4f7afaf74169b22eecc3ae68ebc3efa9cdd84648a8a4c932d6f6c2e4c6`。

- 手机新扫描收到53条primary、51条scan response，成功连接并发现GAP、GATT和测试服务。
- 在同一连接默认1M下回读64字节`00..3f`及空值；显式协商2M后回读64字节异或模式。
- PHY状态分别为`01 02 02 00 00 02`和切回1M的`01 01 01 00 00 01`，不是根据耗时推断PHY。
- 随后3轮主动断开/关闭目标tab、重新扫描连接并回读4/0/64字节，全部通过。
  `short-report.json`包含16个逐项成功结果，没有自动重试。
- 关闭串口读取进程后，手机仍能新建连接并读uptime342秒；重开读取程序后同一
  连接读到413秒，并回读`a5 00 ff 7f 80 5a`，再正常断开，没有重启目标。
- 首段连续串口捕获中的73条ACL TX和79条ACL RX，逐行按HCI声明长度核对，
  没有不完整或跨行帧。心跳中的丢弃计数、DMA错误/dead/panic均为0。
- 用户过滤器一直为`CH, -75 dBm`，以及原有厂商排除项；前后未改动。

原始证据在忽略目录`tools/android-ble/.build/uart-review/`：`session.json`记录ELF、
实现提交和串口源文件起始偏移；`uart.log`、`uart-audit.json`、`short-report.json`
及各手机步骤文件保留原始证据。已有reader继续运行时直接取其日志，避免双reader
争抢串口；关闭/重新打开reader另有独立步骤报告和串口捕获。

## panic在途传输验证

临时boot固件先在关中断状态下启动768字节连续DMA，再立即进入panic输出。
串口实际收到连续768个`Z`，随后收到`[PANIC-DMA-TEST] complete`，证明接管没有
在384字节半传输点发生。该测试不依赖DMA完成中断，证据在
`tools/android-ble/.build/panic-review/{report.json,uart.log,firmware.elf}`。

临时代码已移除，正常NimBLE镜像已重建、verify烧录和复位。最终ELF SHA256：
`f3efee51f5c98baf94c09c208d7fa30029ff2b43f286439c6d6ffb9d9f9952af`。
最终镜像再次完成连接、1M四字节回读、切换2M并回读64字节、切回1M、正常断开，
6项全部成功。记录在`tools/android-ble/.build/uart-review/final/report.json`。

## 结论边界

本轮是UART适配和手机短场景验证，没有执行新的持续连接长测。此前独立运行的
队列自环问题尚无充分证据证明根因消失，临界区/重复入队断言仍保留。
不能把本轮通过扩大为全部断连问题已经修复；之前108us版本的84分钟记录也不能
替代当前120us版本验证。HSE保持启用，日志实读LSE=0/0、LSI=0/0。
