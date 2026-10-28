# ch32v208-nimble

**项目定位**：用 Apache NimBLE（controller + host 全开源）直接驱动 CH32V208 的蓝牙硬件，验证/测试该芯片的 BLE 功能。RF/BB/LLE 寄存器级驱动逆向自 WCH 闭源库 `libwchble.a`（见 [WCH_BLE_LIB_RE_FINDINGS.md](../../WCH_BLE_LIB_RE_FINDINGS.md)），**不用 WCH 闭源协议栈、不用 TMOS**。CMake 构建，FreeRTOS + NimBLE NPL(freertos)，probe-rs + WCH-LinkE + SEGGER RTT 日志。

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
   旁路: FreeRTOS + NPL(freertos)｜SEGGER RTT 日志｜TIM3 → os_cputime(1 MHz)
```

## 目录结构

| 路径 | 内容 |
|---|---|
| `app/` | 应用层：`main.c`（启动+FreeRTOS+weak `app_ble_start`）、`app_ble.c/h`（NimBLE 外设应用）、`system_ch32v20x.c`（时钟）、`ch32v20x_it.c`（中断向量） |
| `drivers/wch_ble/` | `wch_rf.c`（BB/LLE/RFEND/AES 寄存器驱动）、`ble_phy_wch.c`（NimBLE PHY 适配）、`ble_hw_wch.c`（NimBLE HW 适配+私有地址） |
| `port/` | 平台移植：`nimble_port_wch.c`（`nimble_port_init` 替代实现）、`hal_timer_wch.c`（TIM3→os_cputime 1 MHz）、`console_rtt.c`（console→RTT）、`syscalls.c`（newlib 存根）、`console/`、`segger_rtt/` |
| `config/` | 顶层配置：NimBLE `syscfg.h` 裁剪（优先于栈内默认值） |
| `third_party/` | vendored 依赖（见下方"第三方组件与许可"） |
| `cmake/` | `riscv32-wch.toolchain.cmake`（WCH GCC 工具链） |
| `ld/Link.ld` | 链接脚本（FLASH 0x08000000 / RAM，ISR 敏感代码段 `.highcode`） |
| `scripts/` | `build.sh` / `run.sh` / `attach.sh` / `gdb.sh` |

## 构建与调试

工具链要求：

| 组件 | 要求 |
|---|---|
| WCH GCC | `riscv32-wch-elf-gcc` 15（MounRiver 发行版），默认路径 `~/DevTools/riscv32-wch-elf-gcc-15`，可用 `WCH_TOOLCHAIN_DIR` 环境变量覆盖 |
| CMake | ≥ 3.20 |
| probe-rs | ≥ 0.27（CH32 支持更早即有；RTT 自动发现建议 0.32+） |
| 调试器 | WCH-LinkE |
| 目标芯片 | CH32V208WBU6（默认）/ CBU6 / RBT6，芯片名可换 |

命令：

```bash
scripts/build.sh                 # Release 构建 → build/ch32v208_nimble.{elf,hex,bin}
scripts/build.sh --clean         # 清理后重建
scripts/run.sh                   # 烧录 + 复位 + RTT 日志直通（Ctrl-C 退出）
scripts/attach.sh                # 只附加运行中的目标，看 RTT 不烧录
scripts/gdb.sh                   # 起 GDB server :3333，另开终端连 riscv32-wch-elf-gdb
```

手动等价命令：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_TOOLCHAIN_FILE=cmake/riscv32-wch.toolchain.cmake
cmake --build build -j$(sysctl -n hw.ncpu)
probe-rs run --chip CH32V208WBU6 build/ch32v208_nimble.elf
```

**probe-rs 芯片名**：与 WCH-LinkE 目标封装对应——`CH32V208WBU6`（QFN48）、`CH32V208CBU6`（QFN32）、`CH32V208RBT6`（LQFP64）。任一脚本均可用 `CHIP=xxx scripts/xxx.sh` 覆盖，无需改代码。

## 上板验证计划（S0–S5）

| 阶段 | 内容 | 怎么算过 |
|---|---|---|
| **S0** | boot 路径：startup→时钟→FreeRTOS→RTT banner（`boot` 目标） | `scripts/run.sh` 能看到 RTT 输出 startup/时钟/任务启动 banner，无 HardFault；gdb attach 可停在 main |
| **S1** | RF 基础：`wch_rf_init`、RFEND TXCtune 三频点校准日志、空闲 RSSI 读数（BB[0xC0] bits[22:15]） | 校准日志给出低/中/高三频点 tCtune 值且逐次上电一致；屏蔽罩下读 RSSI 为饱和负值，靠近 2.4G 干扰源数值变化 |
| **S2** | DTM 发射：HCI_LE_Transmitter_Test（经 host HCI 或直接调 RF 原语定频发射） | nRF Connect/频谱仪在指定信道看到稳定载波/调制包；功率与频偏在 CH32V208 规格内（数据手册典型值） |
| **S3** | legacy 广播：本工程固件上电广播 "CH32V208-NIMBLE" | 手机 nRF Connect 扫描到设备名与厂商字段（0xFF, 'W','C'）；`ble_gap_adv_start` 无错误返回；可被连接 |
| **S4** | GATT + MTU：连接后交换 MTU，读 uptime 特征、写 echo 特征 | 连接日志打印 conn itvl/latency/timeout；MTU 事件打印（≥23）；读回 4 字节 uptime 递增；写任意 hex 后 RTT 打印 hex dump 且回读一致；断开后自动恢复广播 |
| **S5** | 吞吐/稳定性：长时间连接 + notify/写压测，周期校准验证 | ≥30 min 连接不掉线、无 host reset 回调；周期校准定时器日志按时触发且 RSSI 无漂移异常；丢包率符合预期 |

## 寄存器速查（BB/LLE/RFEND/AES）

来源：逆向文档 [WCH_BLE_LIB_RE_FINDINGS.md](../../WCH_BLE_LIB_RE_FINDINGS.md)（下表注明对应章节）。基址：BB `0x40024100`、LLE `0x40024200`、RFEND `0x40025000`、AES `0x40024300`（§3）。

| 域 | 偏移 | 含义 | 出处 |
|---|---|---|---|
| BB[0x00] | bits[5:0] | 信道号 | §4.2 |
| BB[0x00] | bit6 / bit7 / bit8 / bit11 | 白化关 / shut 态 / TX-RX 方向 / 发射触发前置 | §4.2 |
| BB[0x00] | bits[13:12] | PHY 选择（1M/2M/coded 组合） | §4.2、§4.4 |
| BB[0x1C/0x20/0x24/0x28/0x10/0x14] | — | RX 解调器/ADC 时序组（1M/2M 各一组魔数） | §4.4 |
| BB[0x2C] | bits[1:0] | RX arm（bit0=1 收完自动回 ack）/ TX arm | §4.8 |
| BB[0xC0] | bits[22:15] | RSSI（有符号，`BLE_ReadRssi = (int8)(BB[0xC0]>>15)`） | §4.9 |
| LLE[0x00] | =2 / =8 | 启动 TX / shut | §4.3 |
| LLE[0x08/0x0C] | — | TX 模式字 / 状态屏蔽（复位值 0xFFFF/0xF00F） | §3.2、§4.4 |
| LLE[0x14..0x4C] | 每 8B 一组 | 各 PDU 类型超时/窗口基准（单位=LLE tick，**待实测**） | §3.2 |
| LLE[0x1C] / LLE[0x64] | — | RX 等待超时 / TX 完成自旋计数（**单位待实测**） | §3.2、§4.3 |
| LLE[0x74] | — | RX 帧缓冲指针（→ gBleIPPara+0x24） | §3.2 |
| RFEND[0x2C] | bit1 | 频率模式（0=信道号，1=频率直写） | §4.2 |
| RFEND[0x44] | bits[24:20] / bits[17:0] | 合成器整数/小数分频（f=64 MHz×(int+frac/2^18)，低本振注入 −1 MHz@1M / −2 MHz@2M） | §4.2 |
| RFEND[0x48/0x4C] | — | 模拟前端基准位域（DevInit 魔数序列，原样复刻） | §3.3 |
| RFEND[40(0x28)] | =1152 | Reset 后基准值 | §3.3 |
| AES@0x40024300 | — | 硬件 AES-128（CCM 包加密的密码原语，SM 用） | §9.7、§13.1 |

## 与 trouble(Rust) 路线的关系

同一验证目标（CH32V208 BLE 硬件 + 全开源栈）的 C 快速路径：本工程用 NimBLE 先把硬件调通，寄存器事实与驱动可直接迁移给 [TROUBLE_CH32V208_PORTING.md](../../TROUBLE_CH32V208_PORTING.md) 描述的 Rust trouble-host + 自研 controller 路线。

## 第三方组件与许可

| 组件 | 位置 | 来源 | 许可 |
|---|---|---|---|
| WCH CH32V20x SDK（HAL/startup） | `third_party/sdk` | WCH EVT（MounRiver 发行包） | WCH 随包许可，原样使用 |
| FreeRTOS 内核 | `third_party/freertos` | upstream FreeRTOS-Kernel | MIT |
| SEGGER RTT | `third_party/segger_rtt` | SEGGER RTT 发行包 | SEGGER RTT 按其自带许可条款使用（permissive source-available） |
| Apache NimBLE | `../nimble`（工作区外检出，不入库） | apache/mynewt-nimble | Apache-2.0 |

## 已知限制与待上板校准项

- **LLE[0x1C]/[0x64] 单位未实测**：超时/自旋计数按逆向推断的 LLE tick 使用，上板后需用逻辑分析仪/RTT 计时校准（RE findings §3.2、§4.3）。
- **auto-ack 未启用**：v1 收发走手动模式字序列（BB[0x2C] arm 位留待 S3 后开启自动应答）。
- **RNG 非安全熵源**：控制器随机数来自硬件 LFSR 类源，可用于地址生成，**不可**作为配对/加密密钥熵源（SM v1 未启用，见下）。
- **privacy / 2M PHY / coded PHY 关闭**：仅 legacy 1M 广播与连接；地址用 public（控制器不回 BD_ADDR 时退化为静态随机）。
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
