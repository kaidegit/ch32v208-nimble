# ch32v208-nimble

在 CH32V208 上运行 Apache NimBLE（controller + host 全开源），直接驱动芯片自带蓝牙硬件的验证工程。BB/LLE/RFEND/AES 寄存器级驱动逆向自 WCH 闭源库 `libwchble.a`，**不使用 WCH 闭源协议栈，也不使用 TMOS**。CMake 构建，FreeRTOS + NimBLE NPL(freertos)，probe-rs + WCH-LinkE 烧录与取证，EasyLogger + USART1 串口 DMA 日志。本项目同时为后续 Rust trouble-host + 自研 controller 路线提供寄存器层面的参考。

## 当前状态

固件可烧录后独立复位运行，已完成实机互通与速率验证：

- **macOS 测试客户端**：扫描、连接、服务发现、uptime 读取、echo 写入回读全部通过；1M/2M PHY 均完成显式确认与 64 字节写入回读，同一连接内 1M→2M 切换通过。
- **Android 手机**（nRF Connect）：连接、服务发现、1M/2M 回读、PHY 切换、重连短测通过。
- **LE Coded PHY**（2026-10-31）：复核修正同coded PHY编码不更新、TX编码位残留与RX参数倒置。手机同连接S2↔S8切换与64B回读通过，本端TX寄存器/耗时确认两种编码实际生效；对端S2发送未实测。详情见[复核记录](docs/coded-review-2026-10-31.md)。
- **长时可靠性**：早期镜像通过 30 次复位、100 次重连、1M/2M 各 30 分钟持续连接读写，累计 1 万余轮回读校验，覆盖 cputime 自然回卷。
- **尚未完成**：当前镜像未重做长时测试；coded大流量、部分接收事件预算及此前队列自环仍需验证；仪器级射频指标未验证。暂不宣称BLE已完全稳定。

各阶段实测数据与排查过程见 [docs/](docs/) 索引。

## 已知调试坑：RTT 会暂停 CPU

本板经 probe-rs 读取 RTT 实际会反复暂停 CPU，破坏 BLE 微秒级收发时序，曾表现为射频发送失败累积、手机无法连接。因此本项目：

- 运行测试一律使用串口日志（921600 8N1），不附加 RTT 或 GDB；
- GDB 仅用于停机后的故障取证，取证后正常复位再做独立运行验证。

结论适用范围与完整现场见 [Android 互通记录](docs/android-interop-2026-10-30.md) 和 [串口日志复核](docs/uart-review-2026-10-30.md)。

## 仓库结构

| 路径 | 内容 |
|---|---|
| `app/` | 应用层：启动、FreeRTOS 初始化、NimBLE 外设应用（广播 + GATT 私有服务） |
| `drivers/wch_ble/` | 逆向得到的射频/基带寄存器驱动，以及 NimBLE 的 PHY、HW 适配 |
| `port/` | 平台移植：NimBLE port 入口、定时器、串口日志、加密 shim、newlib 存根 |
| `config/` | NimBLE 配置裁剪 |
| `third_party/` | WCH SDK、FreeRTOS 内核、EasyLogger 等第三方源码 |
| `ld/`、`cmake/` | 链接脚本与 WCH GCC 工具链文件 |
| `scripts/` | 构建、烧录、串口日志、GDB 取证脚本 |
| `tools/macos-ble/` | macOS 侧 BLE 测试客户端（扫描、GATT 读写、速率与可靠性测试） |
| `tools/android-ble/` | 通过 ADB 驱动手机 nRF Connect 的互通测试工具 |
| `docs/` | 各阶段排查与实测记录 |

## 构建与烧录

| 组件 | 要求 |
|---|---|
| WCH GCC | `riscv32-wch-elf-gcc`（MounRiver 发行版），默认路径 `~/DevTools/riscv32-wch-elf-gcc-15`，可用环境变量 `WCH_TOOLCHAIN_DIR` 覆盖 |
| CMake | ≥ 3.20 |
| probe-rs | ≥ 0.32 |
| 调试器 | WCH-LinkE |
| NimBLE 源码 | [apache/mynewt-nimble](https://github.com/apache/mynewt-nimble) 检出到**本仓库同级目录 `../nimble`**（不随本仓库分发，构建时直接编译其中的源码） |

```bash
# 1. 获取 NimBLE 源码（与本仓库并列放置）
git clone https://github.com/apache/mynewt-nimble ../nimble

# 2. Release 构建，产物 build/ch32v208_nimble.{elf,hex,bin}
scripts/build.sh

# 3. 烧录并正常复位，目标独立运行
scripts/run.sh

# 4. 查看串口日志（921600 8N1，PA9 TX / PA10 RX）
scripts/log.sh -p /dev/cu.usbmodemXXX
```

芯片默认 `CH32V208WBU6`（QFN48），其他封装用 `CHIP=CH32V208CBU6`（QFN32）或 `CHIP=CH32V208RBT6`（LQFP64）覆盖，无需改代码。

## 文档索引

| 文档 | 内容 |
|---|---|
| [bringup-2026-10-29.md](docs/bringup-2026-10-29.md) | 启动路径排查与修复（.data 复制源、syscall 链接、ISR 误判） |
| [radio-2026-10-29.md](docs/radio-2026-10-29.md) | 射频寄存器逐项复核与空口实测 |
| [phy-rates-2026-10-30.md](docs/phy-rates-2026-10-30.md) | 1M/2M 速率切换实测 |
| [reliability-2026-10-30.md](docs/reliability-2026-10-30.md) | 复位、重连与长时连接可靠性测试 |
| [android-interop-2026-10-30.md](docs/android-interop-2026-10-30.md) | Android 手机互通排查与 RTT 结论 |
| [uart-review-2026-10-30.md](docs/uart-review-2026-10-30.md) | 串口 DMA 日志复核与手机短测 |
| [uart-easylogger-handoff.md](docs/uart-easylogger-handoff.md) | 日志迁移（EasyLogger + USART1 DMA）交接说明 |
| [coded-phy-2026-10-31.md](docs/coded-phy-2026-10-31.md) | LE Coded S=2/S=8 接入、S=2 锚点漂移根因与手机实测 |
| [coded-review-2026-10-31.md](docs/coded-review-2026-10-31.md) | Coded适配复核、实际编码切换修复、1537硬件错误恢复与错误码勘误 |

## 其他已知限制

- SM/配对未启用：GATT 特征全部无加密要求，地址默认 public。
- RNG 为硬件 LFSR 类随机源，可用于地址生成等一般用途，不可作为配对/加密密钥熵源。
- privacy 关闭；legacy 广播 1M，连接支持 1M/2M/coded(S=2/S=8)，默认偏好 1M，coded 由测试特征值显式请求。

## 第三方组件与许可

| 组件 | 位置 | 许可 |
|---|---|---|
| WCH CH32V20x SDK（HAL/startup） | `third_party/sdk` | WCH 随包许可，原样使用 |
| FreeRTOS 内核 | `third_party/freertos` | MIT |
| EasyLogger | `third_party/easylogger` | MIT |
| SEGGER RTT | `third_party/segger_rtt`（历史源码，当前不编译） | 依其自带许可 |
| Apache NimBLE | 仓库同级检出，不入库 | Apache-2.0 |
