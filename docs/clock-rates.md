# BLE 主频适配（72 / 96 / 144 MHz）

`CH32_SYSCLK_MHZ` 选择上电时的 CPU 主频，默认 96。三档都使用同一颗
32 MHz HSE，分别经 HSE/4 ×9、×12、×18 得到系统时钟。

```sh
SYSCLK_MHZ=72 scripts/build.sh
SYSCLK_MHZ=96 scripts/build.sh
SYSCLK_MHZ=144 scripts/build.sh
scripts/run.sh --verify
```

也可以使用独立构建目录，避免三档产物相互覆盖：

```sh
cmake -S . -B build/clock-144 \
  -DCMAKE_TOOLCHAIN_FILE=cmake/riscv32-wch.toolchain.cmake \
  -DCMAKE_BUILD_TYPE=Release -DCH32_SYSCLK_MHZ=144
cmake --build build/clock-144 -j8
```

脚本不指定 `SYSCLK_MHZ` 时保留现有 CMake 缓存；首次配置默认 96。
其他值在 CMake 配置阶段报错。该选项针对启动配置；运行中改变 PLL 必须先停止
BLE，并重新配置 TIM3、SysTick 和受影响的外设。

## 时钟及延时实现

| 项目 | 实现 |
| --- | --- |
| TIM3 / NimBLE cputime | 用 `RCC_GetClocksFreq()` 获取 PCLK1，结合 APB1 定时器倍频规则计算输入；拒绝不能整除的目标频率，保持精确 1 MHz |
| FreeRTOS tick | 按启动时实读的 `SystemCoreClock` 设置 SysTick，保持 1000 Hz |
| ADC | 查询 PCLK2 后选 /2、/4、/6、/8，确保不超过 14 MHz；144 MHz 时 APB2 /2 |
| RF 复位、校准和寄存器切换等待 | 用 TIM3 微秒等待替换 NOP 和固定次数循环 |
| ADC 超时、稳定等待 | 用 TIM3 时间判断，取消固定循环预算 |
| RX→TX 应答 | 先完成帧和 RF 准备，再等待绝对 TX 命令截止时间，避免准备耗时随主频叠加到应答间隔；等待期间暂停 LLE 忙窗，触发前重载 |
| coded RX 起点修正 | 从 CRC 正确的 1M 包和已知 airtime 在线估算 BB 捕获与 RX_DONE 的时间戳差，替代固定 18 μs |
| TX 截止边界 | 超时时重新读取 TX_DONE，避免完成状态在首次轮询与时间读取之间到达时误报超时；没有 TX_DONE 仍返回失败 |
| RTOS 临界区与任务切换 | 保存/恢复 GINTENR 的 MIE/MPIE，保留调用前屏蔽状态；临界区内的 yield 延后到最外层退出，再置软件中断；SWI 恢复寄存器时保持 MIE 关闭直到 MRET |

| CPU MHz | APB1 MHz | APB2 MHz | TIM3 输入 MHz | TIM3 PSC | ADC 分频 | ADC MHz |
| --- | --- | --- | --- | --- | --- | --- |
| 72 | 36 | 72 | 72 | 71 | /6 | 12 |
| 96 | 48 | 96 | 96 | 95 | /8 | 12 |
| 144 | 72 | 72 | 144 | 143 | /6 | 12 |

射频参考 RFCLK 来自 HSE，BLE 总线接口来自 HCLK（本地数据手册图 2-3）。
PHY 的 airtime、IFS、LLE 半微秒窗口及 RF 参数不能按 CPU 主频比例缩放。
120 μs 参数现在表示 RX_DONE 软件时间戳到 TX 命令的间隔，2M 短包修正仍保留。
它仍是互通参数，仪器级空口 T_IFS 精度需要另外测量。

## 自动检查

```sh
python3 scripts/check-clocks.py
python3 scripts/check-phy.py
python3 scripts/check-irq.py
python3 scripts/check-image.py build/clock-144/boot.elf build/clock-144/ch32v208_nimble.elf
```

`check-clocks.py` 使用真实生产函数和模拟 MMIO/时间，覆盖 APB1 分频及定时器倍频、
不同准备耗时下的绝对发送截止时间、16/32 位回卷、迟到拒发、ADC 分频及超时、
接收时间戳修正，以及 TX_DONE 在截止判断期间到达和真正缺失的两种情况。
这些检查不模拟射频硬件；实机记录和镜像保存在忽略目录
`build/frequency-validation/`，测试中不附加 RTT/GDB，结束后才停机取证。

`check-irq.py` 检查真实 FreeRTOS 移植函数：嵌套临界区内 yield 不触发切换、
多次请求合并、最外层退出后触发，以及任务切换改变全局保存值时仍恢复原调用者的屏蔽状态。

## 实机验证

测试日期：2026-10-31；板卡 CH32V208WBU6，Mac CoreBluetooth，
OPPO Find X3 / Android 14 / nRF Connect。测试使用已有
`CH32_BLE_TEST_IDENTITY=ON`，避免 Mac 对公开地址的旧 GATT 表缓存。
手机名称、RSSI、厂商及服务排除过滤器保持原样。

最终 Release 固件的独立运行结果如下。每个持续读写阶段单独保持连接至少 60 秒，
载荷循环覆盖 0/1/2/3/4/5/7/16/20/23/31/32/63/64 字节，逐字节校验。

| CPU MHz | 1M 持续读写轮数 | 2M 持续读写轮数 | 手机 S2/S8 读写 | 额外重连及 1M→2M 切换 | TX失败/迟到/断言 |
| --- | --- | --- | --- | --- | --- |
| 72 | 130 | 130 | 均通过 | 10/10 通过 | 0/0/0 |
| 96 | 130 | 128 | 均通过 | 10/10 通过 | 0/0/0 |
| 144 | 129 | 130 | 均通过 | 10/10 通过 | 0/0/0 |

三档累计完成 777 轮持续读写校验；手机各完成扫描、连接、S2/S8 选择与 8 字节回读。
停机前保留的本端 S2/S8 TX 寄存器分别置位/清零编码选择位，发送计数均增长，
空包耗时分别约 497/755 μs，确认本端两种编码实际生效。对端编码仍由对端选择。
三档的 `g_port_switch_bad_critical`、`g_port_yield_bad_mask`、硬件错误和异常记录均为 0。
TIM3 分别实读 PSC=71/95/143，cputime 均为 1 MHz；捕获补偿在线估算为 16/17/17 μs。

完整镜像、SHA-256、源码快照、逐项 JSONL/手机报告和停机快照见
`build/frequency-validation/manifest.json` 及同目录文件。测试结束后正常复位，
板卡与默认构建产物恢复为 96 MHz，沿用原有测试地址配置。没有重做 30 分钟以上长测，
也没有做仪器级空口时序/射频合规验证；本轮不能代替这些验证。

首轮完整三档互通记录保存在 `build/frequency-validation/pre-timeout-check/`。
该轮 96 MHz 快照中有一次 `tx_fail=1`，现场为 CMD=0、IRQ=0x62002（含 TX_DONE）、
window=0；72/144 MHz 为 0。原代码在轮询状态为 0 后读取时间，若 TX_DONE 在两次
读取之间到达，截止分支仍直接返回失败。新增完成状态复查覆盖了这个竞争窗口；
这与原始现场相符，但不能仅凭该快照认定已经找出了当次超时的全部原因。
补充修复后重新构建并执行完整三档实机矩阵，最终结果与原始记录分别保存。

GDB 快照的原始 RCC_CFGR0 三档都读到 0x0034040a，与各档启动配置不一致，
未用这个停机调试读值推导运行时 ADC 频率。上表 ADC 12 MHz 是启动配方及 SDK
API 的计算结果；运行时主频由独立复位后的串口启动日志确认，未做仪器级频率测量。

## 重连复测发现的调度缺陷

第二轮 72 MHz 的 Mac 1M/2M 各 60 秒通过后，手机连接触发
`vPortSwitchContext()` 的临界区保护断言，原始记录保存在
`build/frequency-validation/critical-failure-72/`。当时 `uxCriticalNesting=1`、
NPL 临界区计数为 0，保存的任务 PC 指向 `vPortExitCritical` 的入口；其调用点位于
`xQueueGenericSend`，紧接在临界区内的 `portYIELD()` 后。TX 失败和硬件错误均为 0。

该移植使用全局临界区计数，不能在计数非零时切换到另一任务。现由 `vPortYield()`
合并并延后临界区内请求，最外层退出后才设置 SWI；未取消 `vPortSwitchContext` 的断言。
同时按本地 QingKeV4 手册 §2.2/§8.3 统一保存、屏蔽和恢复 MIE/MPIE，
避免旧代码无条件使能中断、覆盖调用前屏蔽状态。计时器与串口也使用相同的保存/恢复规则。
手册明确 QingKe 的 MIE 不会在进入中断时自动清零，因此不能套用标准 RISC-V 的入口假设。

GINTENR 恢复必须用 `csrc`/`csrs` 位操作。中间一次整字 `csrw 0x800` 写法在实机上
清掉 MPP，启动到创建 LL 任务时触发非法 CSR 指令；该错误镜像与快照保存在
`build/frequency-validation/irq-alias-write-rejected/`，最终代码已移除此写法。
