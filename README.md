# ch32v208-nimble

最新实机排查见 [2026-10-29 取证与修复记录](docs/bringup-2026-10-29.md)。已确认：
`.farcode` 导致 CRT 的 `.data` 复制源错误；放在静态库中的 syscall overrides 未被链接；
`mcause` 残留值把任务误判成 ISR，空事件回调又跳回 `_start`。当前已改为使用
`LOADADDR(.data)`、直接链接 syscalls 和 `PFIC_GISR.GACTSTA`。
下方调试历史中的 HPE/异步写内存/EHMOD 等旧推断应对照此记录阅读。

射频修正和空口实测另见 [射频复核记录](docs/radio-2026-10-29.md)。
当前固件可独立复位运行，Mac已完成扫描、服务发现、uptime读取和echo写入回读实测。
1M/2M均已完成显式PHY确认和64字节写入回读，同连接1M→2M通过；
本机Mac对2M→1M请求回复no-change。详见[速率测试记录](docs/phy-rates-2026-10-30.md)。
可靠性复测已完成：30次复位、100次重连，1M/2M各30分钟及额外2M20分钟均通过；
累计10291轮回读校验，覆盖约71.6分钟的cputime回卷。复位VCO超时误报已修复。
详见[可靠性测试记录](docs/reliability-2026-10-30.md)。coded PHY和仪器级时序仍未验证。
macOS 测试客户端见 [tools/macos-ble](tools/macos-ble/README.md)。
Android 手机互通排查及 RX→TX 软件等待调整见
[手机互通记录](docs/android-interop-2026-10-30.md)；
[ADB 手机工具](tools/android-ble/README.md)直接操作现有 nRF Connect，保留用户过滤器。
旧逆向笔记和下方历史表包含已推翻的寄存器解释，新增驱动修改必须核对原厂指令。

**项目定位**：用 Apache NimBLE（controller + host 全开源）直接驱动 CH32V208 的蓝牙硬件，验证/测试该芯片的 BLE 功能。RF/BB/LLE 寄存器级驱动逆向自 WCH 闭源库 `libwchble.a`（见 [WCH_BLE_LIB_RE_FINDINGS.md](../../WCH_BLE_LIB_RE_FINDINGS.md)），**不用 WCH 闭源协议栈、不用 TMOS**。CMake 构建，FreeRTOS + NimBLE NPL(freertos)，probe-rs + WCH-LinkE 烧录/停机取证，EasyLogger + USART1 DMA 日志。

## 架构

```
        app/main.c  app/app_ble.c            应用层: GATT 私有服务 + 广播
              │  (app_ble_start 强符号)
              ▼
   ┌── NimBLE host (nimble/host) ──┐   GAP/GATT/ATT/L2CAP/SM, 事件队列驱动
   │            ▲ native transport │   ble_transport_to_ll_*/_to_hs_* (进程内 HCI)
   └────────────┼──────────────────┘
   ┌── NimBLE controller/LL ───────┐   广播/连接状态机、调度、CSA#1 跳频
   │   ble_ll_*.c + porting/npl    │
   └────────────┬──────────────────┘
                ▼
   ble_phy_wch.c / ble_hw_wch.c          PHY 时序与 HW 适配 (本工程)
                ▼
   drivers/wch_ble/wch_rf.c              BB/LLE/RFEND/AES 寄存器级驱动
                ▼
   ┌────────────┴───────────────────┐
   │ BB   0x40024100   基带数字域    │
   │ LLE  0x40024200   空口时序引擎  │      （逆向所得基址，见 RE findings §3）
   │ RFEND 0x40025000  射频模拟前端  │
   │ AES  0x40024300   硬件 AES-128 │
   └────────────────────────────────┘
   旁路: FreeRTOS + NPL(freertos)｜USART1 DMA 日志｜TIM3 → os_cputime(1 MHz)
```

## 目录结构

| 路径 | 内容 |
|---|---|
| `app/` | 应用层：`main.c`（启动+FreeRTOS+weak `app_ble_start`）、`app_ble.c/h`（NimBLE 外设应用）、`system_ch32v20x.c`（时钟）、`ch32v20x_it.c`（中断向量） |
| `drivers/wch_ble/` | `wch_rf.c`（BB/LLE/RFEND/AES 寄存器驱动）、`ble_phy_wch.c`（NimBLE PHY 适配）、`ble_hw_wch.c`（NimBLE HW 适配+私有地址） |
| `port/` | 平台移植：`nimble_port_wch.c`（`nimble_port_init` 替代实现）、`hal_timer_wch.c`（TIM3→os_cputime 1 MHz）、`console_uart.c`（console/NPL/EasyLogger→USART1 DMA）、`wch_log.h`、`syscalls.c`（newlib 存根） |
| `config/` | 顶层配置：NimBLE `syscfg.h` 裁剪（优先于栈内默认值） |
| `third_party/` | vendored 依赖（见下方"第三方组件与许可"） |
| `cmake/` | `riscv32-wch.toolchain.cmake`（WCH GCC 工具链） |
| `ld/Link.ld` | 链接脚本（FLASH 0x08000000 / RAM，ISR 敏感代码段 `.highcode`） |
| `scripts/` | `build.sh` / `run.sh` / `log.sh` / `serial_log.py` / `gdb.sh` |

## 构建与调试

工具链要求：

| 组件 | 要求 |
|---|---|
| WCH GCC | `riscv32-wch-elf-gcc` 15（MounRiver 发行版），默认路径 `~/DevTools/riscv32-wch-elf-gcc-15`，可用 `WCH_TOOLCHAIN_DIR` 环境变量覆盖 |
| CMake | ≥ 3.20 |
| probe-rs | ≥ 0.32（下载、复位和停机取证） |
| 调试器 | WCH-LinkE |
| 目标芯片 | CH32V208WBU6（默认）/ CBU6 / RBT6，芯片名可换 |

命令：

```bash
scripts/build.sh                 # Release 构建 → build/ch32v208_nimble.{elf,hex,bin}
scripts/build.sh --clean         # 清理后重建
scripts/run.sh                   # 烧录 + 正常复位，目标独立运行
scripts/log.sh --list             # 列出USB串口（依赖pyserial）
scripts/log.sh -p /dev/cu.usbmodemXXX  # PA9 TX / PA10 RX，921600 8N1，无流控
scripts/attach.sh                # log.sh兼容入口，不再打开探针或RTT
scripts/gdb.sh                   # 起 GDB server :3333，另开终端连 riscv32-wch-elf-gdb
```

手动等价命令：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_TOOLCHAIN_FILE=cmake/riscv32-wch.toolchain.cmake
cmake --build build -j$(sysctl -n hw.ncpu)
probe-rs download --chip CH32V208WBU6 build/ch32v208_nimble.elf
probe-rs reset --chip CH32V208WBU6
```

**probe-rs 芯片名**：与 WCH-LinkE 目标封装对应——`CH32V208WBU6`（QFN48）、`CH32V208CBU6`（QFN32）、`CH32V208RBT6`（LQFP64）。任一脚本均可用 `CHIP=xxx scripts/xxx.sh` 覆盖，无需改代码。启动代码已通过绝对跳转处理`0x0`复位别名，`run.sh`使用正常复位，不自动附加RTT。日志已迁移为USART1（PA9 TX、PA10 RX）、921600、8N1、无流控，由DMA1通道4发送；`log.sh`只读串口。普通日志有界入队，满时计数丢弃；异常采用有界轮询输出。本板probe-rs RTT使用ProgramBuffer暂停CPU读RAM，会干扰BLE，因此当前固件不链接RTT。`gdb.sh`也会暂停CPU，仅用于取证。HSE保留，LSI显式关闭。

## 上板验证计划（S0–S5）

| 阶段 | 内容 | 怎么算过 |
|---|---|---|
| **S0** | boot 路径：startup→时钟→FreeRTOS→RTT banner（`boot` 目标） | `scripts/run.sh` 能看到 RTT 输出 startup/时钟/任务启动 banner，无 HardFault；gdb attach 可停在 main |
| **S1** | RF 基础：`wch_rf_init`、RFEND TXCtune 三频点校准日志、空闲 RSSI 读数（BB+0x30 bits[22:15]） | 校准日志给出低/中/高三频点 tCtune 值且逐次上电一致；屏蔽罩下读 RSSI 为饱和负值，靠近 2.4G 干扰源数值变化 |
| **S2** | DTM 发射：HCI_LE_Transmitter_Test（经 host HCI 或直接调 RF 原语定频发射） | nRF Connect/频谱仪在指定信道看到稳定载波/调制包；功率与频偏在 CH32V208 规格内（数据手册典型值） |
| **S3** | legacy 广播：本工程固件上电广播 "CH32V208-NIMBLE" | 手机 nRF Connect 扫描到设备名与厂商字段（0xFF, 'W','C'）；`ble_gap_adv_start` 无错误返回；可被连接 |
| **S4** | GATT + MTU：连接后交换 MTU，读 uptime 特征、写 echo 特征 | 连接日志打印 conn itvl/latency/timeout；MTU 事件打印（≥23）；读回 4 字节 uptime 递增；写任意 hex 后 RTT 打印 hex dump 且回读一致；断开后自动恢复广播 |
| **S5** | 吞吐/稳定性：长时间连接 + notify/写压测，周期校准验证 | ≥30 min 连接不掉线、无 host reset 回调；周期校准定时器日志按时触发且 RSSI 无漂移异常；丢包率符合预期 |

## 寄存器速查（BB/LLE/RFEND/AES）

以下为本轮核对原厂指令的字节偏移；完整依据见射频复核记录。

| 寄存器 | 已核实含义 |
| --- | --- |
| BB+0x00 bits13:12 | 1M=0x1000，2M=0；bits5:0为BLE信道 |
| BB+0x2c bits30:25 | TX功率枚举，0dBm=9 |
| BB+0x30 | RSSI bits22:15 |
| BB+0x38 | 状态；bit5为CRC错误；bit4/7不能作为RX超时判断 |
| LLE+0x04 bit0 | basic/auto sequencer选择，完成状态含义随模式变化 |
| LLE+0x08 / +0x0c | W1C状态 / 中断屏蔽；basic RX done=1，TX done=2 |
| LLE+0x14 | 初始化0x8c；模式字位于+0x50 |
| LLE+0x50 | TX=0x5a、RX=0x59、校准=0x5d |
| LLE+0x64 | 半微秒窗口倒计时；归零不能证明TX成功 |
| LLE+0x70 / +0x74 | 独立TX/RX DMA直接地址；RX需272字节对齐缓冲 |
| RFEND+0x2c bit1 | BLE路径清零，使用自动信道合成 |

## 上板调试记录（2026-10-28 bring-up）

### 中断属性与 HPE（历史排查）

以下为2026-10-28的排查历史，含已推翻的假设。当前startup使用`INTSYSCR=0x0`，
关闭嵌套与HPE；BB/LLE IRQ已启用，收发完成由中断处理。以顶部最新记录为准。

- `interrupt("WCH-Interrupt-fast")` 告诉编译器“硬件会保存寄存器”，依赖 **HPE（硬件压栈）**：CSR `0x804`（INTSYSCR）bit0 `HWSTKEN`。RM 9.5.3.1 的复位值确为 0，但**本项目 startup（`startup_ch32v20x_D8W.S`）原来显式写 `0x3`（HPE+嵌套都开）**——早期“HPE 默认关、fast 属性缺保护现场”的推断前提不成立。
- **崩溃事实链（三轮一致）**：每轮都死在 `hw: rng_seed` 打印后 ~1 ms 内，`mcause=4`、`mtval=0x7B21AF17`；反汇编定位在 `xTaskIncrementTick`（tasks.c:2853）读 `pxCurrentTCB->uxPriority`——即 `pxCurrentTCB`（0x2000036C）被写成 `0x7B21AEEB`（= mtval−44，TCB 内 uxPriority 偏移）。该值跨构建、跨运行**完全确定**，且不在 flash 镜像里。rng_seed 阶段纯软件部分（xorshift、UID/MAC 只读）无任何野写路径，SDK ADC 调用只写外设寄存器 ⇒ 写入者是**异步的**。
- **当前主嫌疑：HPE × mscratch 交换式 FreeRTOS port**。WCH 的 port 在 `SW_Handler`/`xPortStartFirstTask`（portASM.S）里**软件**搬运 sp/mscratch 跨上下文切换；HPE 开着时，中断入口的硬件压栈/出栈与这种 sp 移动交错，可能把寄存器帧落到错误位置（值确定：帧内容就是被打断上下文的寄存器）。portASM.S 里 WCH 自己留了一行被注释的 `csrs 0x804, 0x20 /* Disable HPE */`，说明他们知道有相互作用。
- **处置（2026-10-28）**：startup 改写 `0x2`——只关 HPE（bit0），保留嵌套（bit1）。安全性已核对：portASM.S `SW_Handler` 软件保存全部 x1/x5–x31+mstatus，port.c `SysTick_Handler` 是普通 `interrupt()` + 对称的 mscratch 交换，本工程全部 handler 也是普通 `interrupt()`，均不依赖 HPE 入口帧。banner 新增 `intcfg` 行实读 INTSYSCR 自证。**若关 HPE 后崩溃消失即定案；若仍在，下一步在 rng_seed 各子步骤间布 `pxCurrentTCB`/canary 打点取证。**
- 教训（仍然有效）：fast 属性的正确性依赖 HPE 状态与 port 的配合，本项目一律用普通 `interrupt()`；改成普通属性本身无害（该修复后崩溃依旧，说明它只是必要非充分）。
### 根因（2026-10-28 23:46 破案）：RNG 种子无限递归炸栈

`ble_hw_rng_seed()`（原始版）结尾先做 4 次 `ble_hw_rng_read()`、**之后**才置 `g_rng_seeded=1`；而 `ble_hw_rng_read()` 在 `!g_rng_seeded` 时回调 `ble_hw_rng_seed()` —— seed→read→seed→read **无限递归**。每层 ~150–350 B（ADC 采样帧 + UID/MAC 读取 + 64 位种子运算 + printf 320 B 帧），把 app 任务 4 KB 静态栈吃穿后继续向下碾进 .bss：`g_app_zone` 哨兵、白名单走廊、FreeRTOS 内核变量（`pxCurrentTCB` @0x2000036C）、`ucHeap`、直到 `_SEGGER_RTT` @0x2000A230 —— 与全部观测足迹吻合（"栈溢出 hw=0"、RTT 通道描述符损坏、heap_4.c:292 free 断言、mcause=4 读 `pxCurrentTCB` 崩溃、崩溃点永远在 `hw: rng_seed` 打印后 ~1 ms）。

- **神秘确定值 0x7B21AF17 的来历**：banner 实测 `uid0 = 0x7B21AEEB`（0x1FFFF7E8）。HPE 开着那几轮 `pxCurrentTCB` 被写的正是 uid0——`FLASH_GetMACAddress` 每层递归都把 uid0 装进局部 `value`，递归栈以固定布局下降，某一深度的 `value` 槽位恰好落在 0x2000036C，于是同一值每轮稳定复现；代码布局一变（如关 HPE/加插桩后），那个槽位装的是别的中间值（0x34D9BBAD）。所谓"跨构建确定的垃圾值"其实是"每布局确定的栈内容"。
- **教训**：`-fstack-usage` 只能证单帧深度，排除不了**无界递归**；"hw=0（A5 全毁）"从一开始就在说真话（真·栈耗尽），被"最大帧仅 320 B"带偏了方向。
- **修复**：`g_rng_seeded = 1` 移到 discard 读之前（`drivers/wch_ble/ble_hw_wch.c`）。
- **HPE 调查结论保留**：startup 原写 `INTSYSCR=0x3`（HPE+嵌套开）确为真实风险面（与 mscratch 交换式 port 的相互作用未验证），已改 `0x2` 关闭且全链路核对安全；但它不是本次崩溃的原因（hpe=0 时崩溃依旧）。中断属性一律普通 `interrupt()` 的规矩不变。

### Flash / SRAM 配置与 ld 的对齐（依据：CH32FV2x_V3xRM V2.5、CH32V208DS0）

- **总量 480 KB**（RM 表 32-4"总共 480K 字节的主存储器"），地址 `0x08000000..0x08077FFF`（1920 页 × 256 B）。`ld/Link.ld` 的 448 K 是 EVT BLE 工程的软件约定：顶部 32 K（`0x08070000+`）预留给 BLE NV/绑定数据，不是硬件边界。
- **零等待窗口与划分**：480 K = 零等待应用区 + 非零等待数据区（DS 表 2-1 注 1），分界由**用户选择字 USER[7:5]（RAM_CODE_MOD）**决定（RM 表 32-4，仅 D8/D8W 系列）：`00x = CODE-128K + RAM-64K`（**出厂默认**）、`01x = CODE-144K + RAM-48K`、`1xx = CODE-160K + RAM-32K`。复位后从 `0x1FFFF800` 选字区装载，运行时只读 `FLASH_OBR`（`0x4002201C`）bits[9:7] 即当前模式。
- **ld 假设 = 出厂默认**（128K 零等待 + 64K SRAM）。若选字被改过（如 160K+32K），ld 仍按 64K 布局，高地址全局变量落在不存在的 RAM 上——上板 banner 已加 `memcfg` 行实读 OBR/`ram_code_mod`/ESIG `F_SIZE`（`0x1FFFF7E0`，KB 单位）自证。
- **非零等待区可执行**（RM 32.3"FLASH 增强读模式"，用户代码可超出 CODE 窗口运行），但慢；跨区后建议置 `FLASH_CTLR.EHMOD`（`0x40022010` bit24），擦写/进停止模式前必须退出。**当前镜像 ~130 KB，距 128 K 零等待窗口余量已不足 1 KB**，继续增长即跨区。
- **Flash 访问时钟 ≤ 60 MHz**（RM 32.1 注 2 / `FLASH_CTLR` bit25 `SCKMOD`，复位值 0 = SYSCLK/2）：96 MHz 主频下默认即 48 MHz，合规；不要置 SCKMOD=1。
- **SRAM**：`0x20000000..0x20010000` 连续 64 KB，无分段、无需使能（RCC_AHBPCENR.SRAMEN 复位 1，仅管睡眠时钟）。坑：**待机模式默认不保 RAM**（PWR_CTLR R30KSTY/R2KSTY 复位 0，睡前显式置位最多保持 32 K）。
- **ESIG**（RM 第 31 章）：`0x1FFFF7E0` = F_SIZE（可用代码区 KB 数），`0x1FFFF7E8/EC/F0` = 96 位 UID（`FLASH_GetMACAddress` 即取自这里）；选字区 `0x1FFFF800`：byte0 RDPR（出厂 0xA5）、byte2 USER。
- **实测（2026-10-28）**：本板 OBR=0x03FFFCFC，RAM_CODE_MOD=0b001——属 **00x 组 = CODE-128K + RAM-64K，与出厂默认同划分**（值 1 与 0 等价，[2:1] 位才是模式位）；`fsize=160K` 是该料零等待容量标识，不改变划分。**结论：划分与 ld 一致，无需修复。** 曾误按 `mode != 0` 触发过一次不必要的选项字节重编程并中断于半途；现改为按模式位 `(mode & 0x6) != 0` 判断，且修复路径先切到当前模式有效 RAM 顶部的临时栈（SDK 的 `FLASH_ProgramOptionByteData` 从栈上 pbuf 回写全部半字，不能跑在无效栈上）。

### bring-up 修复清单（前两轮）

| 问题 | 现象 | 修复 |
|---|---|---|
| BLE IP AHB 时钟未使能 + cputime 初始化晚于 RF 驱动 | 卡死在 `ble: bringing up`（校准 poll 无超时死循环：TIM3 未配时 `os_cputime_get32()` 恒 0） | `wch_rf_init()` 首行 `RCC_AHBPeriphClockCmd(RCC_AHBPeriph_BLE_CRC, ENABLE)`；`nimble_port_init()` 把 cputime/TIM3 初始化提前到 RF 硬件之前 |
| app 任务栈 512 words 过小 | 首轮真·栈溢出（实测 init 链 ~1.5 KB） | 改静态栈 1024 words（`g_app_zone` 哨兵取证区），`configSUPPORT_STATIC_ALLOCATION=1` |
| BB/LLE NVIC 使能（按项目决策保持关闭） | 中断关闭后"溢出"仍复现，证明写手另有其人（见上节 HPE 主嫌疑，验证中） | `wch_rf_init()` 不再 `NVIC_EnableIRQ(BB/LLE)`；TX 完成走轮询，RX 事件通路留待后续 |
| `WCH-Interrupt-fast` 属性（当时误判 HPE 为关） | RAM 野写（`pxCurrentTCB` 被写残值）→ 栈溢出假象 / mcause=4 HardFault；改后崩溃依旧 | 全部 handler 改普通 `interrupt()`——无害且必要，但非根因（见上节） |
| startup 开 HPE（`INTSYSCR=0x3`）× mscratch 交换式 port | 同上崩溃，三轮确定值 0x7B21AEEB，异步写入者 | startup 改 `0x2`（关 HPE、留嵌套），banner `intcfg` 行自证——验证中 |
| **RNG 种子无限递归（seed→read→seed，`g_rng_seeded` 置位过晚）——根因** | 三轮 mcause=4 + "栈溢出 hw=0" + RTT CB/堆损伤；`pxCurrentTCB` 值=uid0（0x7B21AEEB） | `g_rng_seeded=1` 提前到 discard 读之前；`app_dbg_chk()` 五路 canary 插桩保留观察 |

## 与 trouble(Rust) 路线的关系

同一验证目标（CH32V208 BLE 硬件 + 全开源栈）的 C 快速路径：本工程用 NimBLE 先把硬件调通，寄存器事实与驱动可直接迁移给 [TROUBLE_CH32V208_PORTING.md](../../TROUBLE_CH32V208_PORTING.md) 描述的 Rust trouble-host + 自研 controller 路线。

## 第三方组件与许可

| 组件 | 位置 | 来源 | 许可 |
|---|---|---|---|
| WCH CH32V20x SDK（HAL/startup） | `third_party/sdk` | WCH EVT（MounRiver 发行包） | WCH 随包许可，原样使用 |
| FreeRTOS 内核 | `third_party/freertos` | upstream FreeRTOS-Kernel | MIT |
| EasyLogger | `third_party/easylogger` | armink/EasyLogger `806328e131836662fa83dd43364a53f699fd76ac` | MIT，见随附LICENSE和VENDORED.md |
| SEGGER RTT（历史源码，当前不编译） | `third_party/segger_rtt` | SEGGER RTT 发行包 | 依其自带许可 |
| Apache NimBLE | `../nimble`（工作区外检出，不入库） | apache/mynewt-nimble | Apache-2.0 |

## 已知限制与待上板校准项

- **镜像逼近 128K 零等待窗口**：RAM_CODE_MOD=000（出厂默认）时 CODE 区仅 128 K，当前镜像 ~130 K、余量 <1 K；继续增长将跨入非零等待区（可执行但慢），届时按 RM 32.3 开 `FLASH_CTLR.EHMOD`，擦写/停止模式前退出（详见"Flash / SRAM 配置"节）。
- **LLE[0x1C]/[0x64] 单位未实测**：超时/自旋计数按逆向推断的 LLE tick 使用，上板后需用逻辑分析仪/RTT 计时校准（RE findings §3.2、§4.3）。
- **auto-ack 未启用**：v1 收发走手动模式字序列（BB[0x2C] arm 位留待 S3 后开启自动应答）。
- **RNG 非安全熵源**：控制器随机数来自硬件 LFSR 类源，可用于地址生成，**不可**作为配对/加密密钥熵源（SM v1 未启用，见下）。
- **privacy / coded PHY 关闭**：legacy广播使用1M；连接支持1M/2M，默认偏好1M，可通过测试服务切换。地址默认public；`CH32_BLE_TEST_IDENTITY=ON`可使用独立静态随机测试地址以重新发现GATT数据库。
- **SM/配对 v1 不配**：GATT 特征全部无加密要求；`ble_hs_cfg.store_status_cb` 用 NimBLE 标准 `ble_store_util_status_rr` 策略（绑定/白名单写入空操作，状态事件按 rr 策略回复）。
- host `util`（ble_hs_util_ensure_addr）与 NimBLE 自带 `services/*` 不在编译列表内——应用自带 GATT 服务注册，地址处理见 `app/app_ble.c`。

## 集成审查记录（v1 冻结）

四个子任务（SDK/FreeRTOS/RTT 平台、NimBLE NPL 移植、WCH RF 寄存器驱动、应用层）合入后，主控逐文件对照逆向文档复核，最终全量链接通过：`ch32v208_nimble` FLASH 126.9 KB / 448 KB（27.7%），RAM 53.8 KB / 64 KB（82.1%，含 20K FreeRTOS 堆 + 16K newlib 堆 + 2K 栈，bss 末尾 `0x20008a10`，heap→`0x2000ca10`，栈顶 `0x20010000`，余 11.7K）；`boot` 引导镜像同步回归通过。合入时的主要修正：

| 修正 | 位置 | 说明 |
|---|---|---|
| 白名单 AdvA/TargetA 偏移与 TxAdd 掩码 | `ble_hw_wch.c` | AdvA 在 `pdu+2`（PDU 头 2 字节后），非 `pdu+6`；TargetA `pdu+8`；TxAdd 需 `(pdu[0]>>6)&1` |
| CRC 判定默认信任硬件 | `ble_phy_wch.c` | RE 文档中 CRC 经独立指针（DAT_20000334）存放、未必在缓冲区尾；软件 CRC24 校验保留为 `s_crc_sw_verify` 调试开关，默认 0 |
| `ble_phy_mode_set` 改无条件编译 | `ble_phy_wch.c` | 上游 ble_ll_adv/conn/scan 调用点不受特性宏保护；v1 内部钳位 1M |
| mbedtls 软件 AES-128+CMAC shim | `port/crypto_mbedtls_shim.c` | `ble_ll_crypto.c`/`ble_sm_alg.c` 无条件包含 `<mbedtls/cmac.h>`；shim 过 FIPS-197 C.1 与 pycryptodome CMAC 参考向量（后续可换硬件 AES） |
| NPL ISR 路由 | `config/nimble/nimble_npl_os.h` + `port/nimble_port_wch.c` | 上游 `in_isr()` 读 ARM SCB 在 RISC-V 恒假；eventq/sem/callout_reset 五入口经 `npl_wch_*` 以 `mcause` bit31 分流到 `*FromISR()` |
| 链接组 + heap 封顶 | `CMakeLists.txt`、`ld/Link.ld` | wch_ble↔nimble_core 互引需 `-Wl,--start-group`；newlib 堆固定 16K（NimBLE `os_malloc`→malloc→`_sbrk` 消耗），不再吞掉全部 RAM 余量 |

构建产物：`build/ch32v208_nimble.{elf,hex,bin,map}`。上板入口见上文 S0–S5 计划与 `scripts/run.sh`。
