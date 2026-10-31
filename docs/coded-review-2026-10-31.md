# 2026-10-31 Coded适配复核与1537重启排查

初版基线为`41faa21`加工作区coded改动，ELF SHA256
`756325b8c3b901c3bb04f2fad80ee97e31d9cffd7f964d780b4647ddfa72e630`。
初版能连接、报告HCI coded并回读数据，但不足以证明S=2/S=8均已实际切换。

## 1. 查实的编码切换缺陷

### 射频寄存器残留

初版`set_phy_tx_mode()`只清BB[0x00] bits13:12，再OR目标值。
S2的0x6000切到S8的0x2000时，bit14残留，硬件仍选S2。
原厂`BLE_SetPHYTxMode`在0x13668..0x136b2明确先清bits15:14，
再只在vendor phy3分支置bit14。本移植现在逐次coded TX同样清编码位。

`BLE_SetPHYRxMode` 0x1378e..0x1379e的分支也与初版转录相反：
普通coded RX的BB+0x10应为0x34A4DF，DTM才是0x3FA4DF，现已纠正。

### 同为coded时，LL没有应用新编码偏好

修正上述寄存器后，另一次手机实测仍发现：请求03(S2)，再请求04(S8)，
手机读回`010303000004`且日志报告status=0。完成此场景后再停机取证得到：

```text
connsm.phy_data: cur_tx_phy=3, cur_rx_phy=3, pref_opts=2,
                tx_phy_mode=0, rx_phy_mode=0
tx_good_coded_s2=817, tx_good_coded_s8=0
TX elapsed by mode: {496, 114, 0, 0} us
TX RFCTRL by mode:  {0x1400631d, 0x1400131e, 0, 0}
```

即HCI请求成功，但实际一直是S2。该场景ELF为
`e9e4ebaada63f6a78a2e61e41f6fb2701a49f0fc06e2457df227949e992cf728`。
`ble_ll_conn_hci_le_set_phy()`更新`pref_opts`；
`ble_ll_conn_phy_update_if_needed()`发现仍是相同PHY就不启动PHY更新；
`tx_phy_mode`原本只在PHY instant更新，因而保留旧值。

本地`port/ble_ll_coded.c`通过链接器`--wrap`补这一集成缺口：
原HCI函数成功且当前TX已是coded时，在临界区更新本端`tx_phy_mode`。
新PHY的协商仍走原流程，不修改同级NimBLE源码，不强制对端采用同一种编码。

HCI Read PHY / PHY Update Complete只报告coded=3；手机工具现还检查返回的请求selector，
以拒绝旧状态，但这仍不能证明实际编码。最终验证需结合TX寄存器、耗时或空口抓包。

## 2. “原厂S2 ACK窗口过短”结论撤回

原厂`rf_tx_auto_txProcess`与`ll_slave_process`都按`(BB[0] >> 12) & 3`判断：
0→0x196，2→0x43E，其余→0x1BE。S2=0x6000和S8=0x2000经此掩码都得到2，
因此两者原本就共用0x43E=543us。没有据此认定原厂S2窗口缺陷的依据。
本移植仍使用543us coded ACK总窗口。

## 3. 广播期间host reset reason=1537的实际路径

本地NimBLE定义`BLE_HS_ECONTROLLER=12`，`BLE_HS_HW_ERR(x)=0x600+x`。
因此1537=0x601是Hardware Error(1)，并非Number-of-Completed-Packets记账下溢。
早期日志的A/B复现说明现象在主线也存在，但不能支持原来的下溢归因。

可查到的调用链是：

```text
BB地址事件 → phy_rx_started=1
LLE接收窗口超时且无RX_DONE
→ phy_rx_timeout_isr()原“started”分支
→ ble_ll_hw_error()
→ ble_ll_hw_err_timer_cb()
→ HCI Hardware Error(BLE_HW_ERR_HCI_SYNC_LOSS=1)
→ ble_hs_hw_error(1) → host reset(0x601)
```

该移植只在完整RX_DONE后才把包交给LL；地址事件不保证能收到完整、CRC正确的帧。
半包、错误检测或接收窗口耗尽应能丢包恢复，原实现却升级为整个host重置。
现改为计数`rx_partial_timeouts`/`rx_aborts`，关闭本次PHY接收、清除started，
再调用`ble_ll_wfr_timer_exp()`，由LL正常结束事件。没有清零或放宽completed-packets记账。
另一个错误码也需勘误：`set fields failed rc=22`在NimBLE中是`BLE_HS_ENOTSYNCED`，
不是POSIX errno的EINVAL（NimBLE的EINVAL=3）。host reset先标记未同步，再通知广播结束，
应用立即重启广播会得到22；随后sync回调恢复广播。这是reset的后续表现，不是广播字段非法。

尚未确定触发原始不完整接收的每一种射频条件；修复针对的是错误恢复策略。

## 4. 其他边界与验证方法

`ble_ll_pdu_us()`参数是8位载荷长度，函数内部已包含两字节PDU头。
初版TX忙窗又加了头，既重复计时，又令254/255字节参数截断，现改为直接传payload长度。
本驱动硬件TX截止时间实际来自vendor窗口，故这个辅助忙窗错误不能直接当作已观测断连的原因。

`scripts/check-phy.py`抽取真实寄存器配方/超时函数，用主机C编译器及mock MMIO核对：
各PHY转换、普通/DTM参数、长度边界、半包超时恢复，以及同coded PHY更新本端TX编码。
它不模拟真实中断和射频时序。Android工具离线检查另用`unittest`。

S=2选项是本端TX偏好，对端可以按S8发送。此行为符合
[Bluetooth Core 5.4 Link Layer](https://www.bluetooth.com/wp-content/uploads/Files/Specification/HTML/Core-54/out/en/low-energy-controller/link-layer-specification.html)
中CI逐包表示编码、双向编码可不同的规定。接收时间戳保留地址事件基准，
18us是本板测得的ISR时间修正，不能当作所有负载和板子的精确硬件捕获延迟。

大PDU/持续负载仍未验证；LL部分接收事件预算仍使用配置的RX模式，可能与对端逐包CI不同。
HSE保持启用，LSE和LSI均关闭。此前独立队列自环也未由本轮短测证明根治。

## 5. 本轮实机证据

证据保存于忽略目录`tools/android-ble/.build/coded-review/`：

- `inherited.elf`、`inherited.patch`：接手时镜像和改动；`session.json`记录各阶段ELF哈希。
- `mac-cycles.json`与`mac-cycle-*.jsonl`：修正寄存器后的10次Mac 1M→2M读写/断开均通过。
- `coded-s2/`、`coded-s8/`与`coding-before.gdb.txt`：HCI成功但TX仍S2的反例。
- `final.elf`、`final-uart.log`、`final-phone-report.json`：最终镜像与手机复测结果。

手机过滤器始终保留`ch, -75 dBm`及原有厂商/服务排除项。运行验证不附加RTT/GDB；
GDB仅在场景结束后读取证据，随后停止调试器并正常复位。

最终实测ELF SHA256：
`7ebb77f305f90c05d18aa82b81133e90c70d9b3601572ac22b91793ac03da662`。
FLASH 140352B，RAM 65304/65536B（包括预留堆与栈）。

OPPO手机的15步全部通过：连接、1M回读，然后同一连接内依次请求S2→S8→S2→S8→1M→2M，
每态均写入并逐字节回读；首轮S2/S8各64B，反向切换覆盖5B/空数据，2M也覆盖64B，最后主动断开并关闭目标页。
同一段连接没有非预期断连，日志中无host reset。最终镜像的Mac 1M→2M回归另通过。

完成手机与Mac场景、均已主动断开后，`final.gdb.txt`取证得到：

| TX模式 | 发包计数 | 最后载荷长度 | 最后TX耗时 | RFCTRL & 0xF000 |
|---|---:|---:|---:|---:|
| S2 | 1567 | 0 | 496us | 0x6000 |
| S8 | 1541 | 0 | 754us | 0x2000 |
| 1M | 2959 | 30 | 354us | 0x1000 |
| 2M | 990 | 0 | 78us | 0x0000 |

空包理论时长为S2=462us、S8=720us、2M=44us，实测均多34us驱动/硬件启动开销；
S8与S2的差值258us也一致。这些证据同时验证了本端实际发送编码，而非仅HCI状态。
`rx_valid_coded_s2/s8`按配置的RX模式计数，**不表示对端实际CI编码**；
本轮对端S2发射未实测，不能把该计数或HCI coded状态当作双向S2/S8完整覆盖。

TX失败、HW错误、断言、异常stash、串口丢包和DMA错误均为0。
`rx_partial_timeouts=0`，本轮未自然触发原半包超时；该恢复分支以mock检查验证，
不能把本轮无reset解释成已覆盖所有原始射频触发条件。
`final-validation-uart.log`保留停机前日志，`final.ram.bin`保留64KB RAM。
停止GDB后已正常复位，`after-reset-scan.jsonl`复核独立广播。

构建、双镜像布局检查、PHY配方/恢复检查、Android8项离线检查、Mac CLI自检均通过。
没有执行新的长测，也未据此关闭此前队列自环和coded大负载的遗留项。
