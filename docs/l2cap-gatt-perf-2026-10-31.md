# macOS L2CAP / GATT 饱和吞吐实测（2026-10-31）

本次增加 LE CoC 服务、GATT bulk 服务和 macOS 饱和测试入口，分别测
Mac→MCU 上行、MCU→Mac 下行。串行回显保留为 `--echo-perf` 延迟基线。

## 实现与测量口径

| 通道 | 上行 Mac→MCU | 下行 MCU→Mac |
| --- | --- | --- |
| GATT | 244 B 无应答写，按 CoreBluetooth 可写状态持续填队列 | MCU 持续排入 244 B Notification，Mac 逐包校验 |
| LE CoC | 连续写入字节流，按 OutputStream 背压暂停/恢复 | MCU 连续发送 512 B SDU，Mac 按连续字节偏移校验 |

- GATT 新增 `…3e05` 控制/统计特征及 `…3e06` 无应答写/通知特征。
  每包前 4 字节是递增序号，后续字节随序号变化；统计载荷为完整 244 字节。
  244 + 3 字节 ATT + 4 字节 L2CAP 正好适合 251 字节 LL 数据。
- CoC 使用 PSM `0x0080`，本端接收 MTU 2048、MPS 247、初始 9 credits；
  本端测试发送 SDU 为 512 字节。Mac 的 Stream 写入粒度不等于实际 SDU，
  接收端按全局字节偏移检查 pattern，兼容部分读写和重分段。
- 上行持续发送指定时长后停止投递，再等待 MCU 已收到字节数与 Mac 已排队
  字节数完全相等。以 MCU **实际收到且校验通过**的字节数计数；Mac 排队数
  单独记录，不能作为吞吐。序号、pattern 或字节总数不一致即失败。
- 下行 MCU 持续填发送队列，1 ms callout 补充数据；GATT/CoC 分别保留至少
  5/9 个 MSYS 块的投递门槛。生产结束后等待 Mac 收齐并校验全部已提交载荷。
  不等待应用层逐包回显，不使用 indication 的逐包确认。
- 上行计时从 MCU 收到 START 到最后收到数据，使用 TIM3 1 MHz cputime，
  不以 RTOS tick 估算高负载时间。下行计时由 Mac 单调时钟，从传输开始
  （若先收到数据则从首个接收回调）到最后一个校验成功的接收回调。
  两者均包含队列尾部排空，排除扫描、连接、PHY/CI 设置及最终统计/断开。
- `payload_bytes_per_second = verified_bytes / transfer_seconds`；
  `payload_kbps = verified_bytes × 8 / transfer_seconds / 1000`。
  这是**单向应用载荷**，不双倍计算；不包括 ATT/L2CAP/LL 头及控制包。
- 每次独立连接，传输前后均读取固件 PHY 状态，要求 TX/RX 匹配 1M 或 2M。
  最终统计还读取实际 DLE TX/RX 长度，二者必须均为 251。
- macOS 写入路径遵循 `canSendWriteWithoutResponse`/ready callback 或 Stream
  可写事件，并计数背压。下行记录生产受阻次数、MSYS 最低余量、LL outstanding
  数据包高水位及 PHY 的 More Data 位计数，检查发送端是否确实有积压。

GATT 无应答写入的背压语义见
[Apple 文档](https://developer.apple.com/documentation/corebluetooth/cbperipheral/cansendwritewithoutresponse)；
CoC 使用 [CBL2CAPChannel](https://developer.apple.com/documentation/corebluetooth/cbl2capchannel)
的两个流。本测试不依赖应用回显来驱动下一包。

## 发现并解除的限制

1. 最初 `--perf` 是串行写/读回显，只有一个未完成请求；现已移到
   `--echo-perf`。当时 GATT 64 B 约 507 B/s，CoC 回显约 3.4～4.4 kB/s，
   这些值不能代表饱和吞吐。
2. Controller 的 `BLE_LL_CONN_INIT_MAX_TX_BYTES` 原为 27。即使支持最大
   251，自动 DLE 也不会主动扩到 251；下行 244 B 通知会被拆成多个 LL 包。
   改成 251，正式用控制器 `eff_max_tx/rx_octets` 核对协商结果。
3. CoC 接收 MTU 512 只提供 3 个初始 credits。调整为 MTU 2048、9 credits，
   为连续发送提供更大窗口，同时保持 MSYS 内存预算。
4. 默认十个逻辑 HCI TX credits 调整为 24。native/native 不另建 ACL pool，
   实际内存仍受共享 MSYS 池及生产者预留门槛约束。
5. 原 Mac 连接间隔为 30 ms。测试准备阶段请求 7.5～15 ms，最终是否变更
   由中心端决定。紧接 PHY 过程请求曾返回 554，即 HCI `0x2a`
   `Different Transaction Collision`；客户端增加 1 秒准备间隔再请求，随后
   成功协商到 15 ms，传输完成时再记录实际间隔。

逐项短测（各 5 秒，不能与后文重复测量混作同一组）：

| 配置 | 2M GATT 下行载荷 kB/s（十进制） |
| --- | ---: |
| 饱和发送、初始 TX 27 B、CI 30 ms | 8.94 |
| DLE 实际 251、HCI credits 10、CI 30 ms | 30.35 |
| DLE 251、HCI credits 24、CI 30 ms | 32.80 |
| DLE 251、HCI credits 24、协商 CI 15 ms | 60.75 |

前两步与 CI 调整有明确提升；十到二十四个 credits 的短测差异较小，
不据此单独宣称稳定收益。More Data/背压证明队列受压，不证明已达到芯片
的物理极限；结果还包含当前 Mac、连接事件调度、驱动和协议流控的影响。

## 正式重复测量

2026-10-31 17:41:34～17:46:54（Asia/Shanghai），16/16 项通过。
每组合两次、每次连续发送/生产约 15 秒，再排空并核对接收端计数。
下表单位 kB/s 为十进制 1000 B/s，均值为两次独立结果的算术平均。

| 通道 | PHY | 方向 | 第一次 kB/s | 第二次 kB/s | 均值 kB/s | 均值 kbps |
| --- | --- | --- | ---: | ---: | ---: | ---: |
| GATT | 1M | 上行 | 28.45 | 28.51 | 28.48 | 227.85 |
| GATT | 1M | 下行 | 28.29 | 27.87 | 28.08 | 224.62 |
| GATT | 2M | 上行 | 53.40 | 58.72 | 56.06 | 448.48 |
| GATT | 2M | 下行 | 49.64 | 54.82 | 52.23 | 417.82 |
| L2CAP | 1M | 上行 | 20.90 | 20.01 | 20.45 | 163.63 |
| L2CAP | 1M | 下行 | 29.39 | 30.07 | 29.73 | 237.83 |
| L2CAP | 2M | 上行 | 30.64 | 30.99 | 30.82 | 246.54 |
| L2CAP | 2M | 下行 | 50.68 | 50.87 | 50.78 | 406.21 |

累计校验 8,970,200 字节（各单向案例相加）。
全部 DLE TX/RX=251、CI=15 ms，PHY 前后均匹配目标；校验错误、TX 失败、
TX 迟到、无缓冲及硬件错误均为 0。每个上行都观察到主机背压；每个下行都
观察到生产受阻和 TX More Data，发送端持续有积压。
下行生产者采样的 MSYS 最低余量为 3 个块。

这组结果测的是当前 Mac/固件配置下的饱和单向吞吐。2M 的重复值有波动，
不只报告最好的一次，也不宣称其等于芯片的射频极限。

正式证据目录：[`perf-20261031T094134208337Z`](../tools/macos-ble/.build/perf-20261031T094134208337Z/report.json)。
固件 SHA256：`0a53fd17771343b031b085d290f33cc2ffe896597d2cefee3f82c3b705112828`；
客户端 SHA256：`e1ca73e1b96d361e02cb39e07666f88ee43649c91332031476ae33113580bde7`；
runner SHA256：`7f90189d9b5ad9b723a8ee167a02c376005eeda18598d17c244b439e850670ba`。


## 固件、环境与证据

- CH32V208WBU6，96 MHz，Release，FreeRTOS + NimBLE host/controller。
- macOS 15.7.5（24G624），arm64，Mac14,12，系统 CoreBluetooth。
- `CH32_BLE_TEST_IDENTITY=ON`，本次 GATT schema 使用 XOR `0x5c` 的静态
  随机测试身份，避免旧 `0x5b` 身份缓存。原三个测试特征的句柄未移动。
- `CH32_BLE_PACKET_LOGS=OFF`，关闭 NimBLE DEBUG/INFO 包记录和逐次 GATT
  载荷打印，保留 WARN/ERROR、连接、PHY、perf 汇总及资源日志。
  USART1 921600 8N1 DMA 只读采集，传输期间不访问 probe/RTT/GDB。
- MSYS 24×292 B；FreeRTOS 堆 16 KiB（原 20 KiB，前期测到约 13.8 KiB
  空闲后重新预算）；newlib 预留堆 15 KiB（原 16 KiB）；启动栈 2 KiB。
  最终 Flash 144128 B，RAM 64776 B（含固定堆/栈预留），构建器报 98.84%。
- 仓库基于 `ad8b849` 加本次改动；NimBLE
  `a86045acb7635cabac0e53d7bdee44a99359a887`，依赖仓库工作区无改动。

```sh
cmake -S . -B build -DCH32_SYSCLK_MHZ=96 \
  -DCH32_BLE_TEST_IDENTITY=ON -DCH32_BLE_PACKET_LOGS=OFF
scripts/build.sh
scripts/run.sh --verify

# 独立终端采集 UART；传输期间不使用 probe。
scripts/log.sh -p /dev/cu.usbmodemF3F38F0678B02 -o build/l2cap-uart.log
python3 tools/macos-ble/perf.py --duration 15 --repeats 2

# 单项默认 30 秒，up/down 均以 Mac 为方向参照。
tools/macos-ble/ble.sh probe --perf --phy 2m --direction up
tools/macos-ble/ble.sh l2cap --perf --phy 2m --direction down
```

完整参数见 [工具说明](../tools/macos-ble/README.md)。runner 保留 ELF、客户端/
runner 源码快照、源码/固件 SHA256、Git diff/revision、依赖版本、系统版本、
构建选项及每个案例的 JSONL/stderr；失败即停，不重试失败的测量。
原始证据在忽略的 `.build/`/`build/` 下，本文件引用本机保留路径。

## 验证范围

功能回显覆盖 1、2、23、127、128、129、247、251、255、511、512、513、1024、
4096 字节；错误 PSM 返回失败；工具自测覆盖参数、分段比较、序号/小端编码。
最终镜像另通过 1M/2M 各 14 长度回显、原 GATT 1M→2M 写读及 2 秒旧式 soak
回归。`check-image.py` 检查 boot/full image 的 .data LMA 和 syscall 链接。
正式运行及回归后的断开资源记录均为 MSYS 空闲 22 块、FreeRTOS 最低空闲
9648 B；UART 的 drop/DMA error/panic 均为 0。相同证据目录另保留
`regression-report.json`、`uart.log`、`build.log` 和 `flash.log`。

CoC 上行仍受当前 MTU 2048 / 9 credits 配置与接收完成后补充 credits 的策略
影响；本次没有遍历所有 MTU/credit 参数来寻找最优值。

UART 中的 `ble_att_tx_with_conn: wakeup but nothing in the queue` 是上游在空
ATT 队列唤醒时输出的 ERROR，该分支返回 0；仍保留原文，不当作链路或校验失败。
PHY CRC 错误、RX 超时可能非零，会在报告中保留；不把它们换算成空口 PER。
本次未测 coded、大距离、长时可靠性或仪器级射频峰值。
