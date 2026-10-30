# 2026-10-30 Android 手机互通排查

测试手机为 USB 连接的 OPPO Find X3（PEDM00，Android14），nRF Connect4.29.1。
主机工具直接通过 ADB 操作现有 nRF Connect，没有安装 APK。用户固定过滤器后，
测试全过程保留 `CH, -75 dBm` 及现有厂商排除项；前后 UI 快照一致。
工具没有修改过滤器的命令，点击接口也拒绝过滤器控件。

## 不是 RSSI 过滤：手机控制器已经收到广播

同一时间 Mac 能扫描到设备，而手机 nRF 列表为空。读取 Android 自带的
`BTSNOOP_LOG_SUMMARY` 内存环并按 AOSP btsnooz 格式解码，目标地址
`D0:54:7B:21:AE:B1` 有230条 primary report、0条 scan response，RSSI为-58到-44dBm。
primary report 的完整广播数据为：

```
02 01 06 10 09 43 48 33 32 56 32 30 38 2d 4e 49 4d 42 4c 45 03 ff 57 43
```

其中完整名称就是 `CH32V208-NIMBLE`。这直接排除了“手机根本收不到”及当前
名称/RSSI过滤条件不匹配的解释。判断列表为空与缺失扫描响应有关，也与
[AOSP 主动扫描等待 scan response 的处理规则](https://android.googlesource.com/platform/packages/modules/Bluetooth/+/cdb1c6dca4/system/stack/btm/btm_ble_gap.cc)
一致；这里是根据 HCI 现场和 AOSP 规则作出的归因。

原始证据：`tools/android-ble/.build/phone-preserved-scan/` 中的
`bluetooth-after.txt`、`hci.btsnoop`、`hci-matches.json` 和扫描截图。
早期整环统计可能包含之前扫描，不能当作严格20秒收包率。

## RX→TX 软件等待参数的对照实验

原 `g_wch_phy_response_delay_us=108` 在 Mac 上可完成1M/2M连接读写，但本手机
收不到扫描响应。80us 对照同样没有 Android 扫描响应，Mac仍能完成两种PHY回读。
因此 Mac 互通成功不足以证明这条响应时序适用于手机。

临时实验让 SCAN_RSP 的软件等待按70..132us、步长2us轮换，并在响应的测试
manufacturer AD 中带上所选等待值与序号。手机 HCI 收到81条有标记的响应：

| 软件等待/us | 手机接收条数 |
|---|---|
|110|8|
|112|6|
|114|7|
|116|7|
|118|8|
|120|8|
|122|6|
|124|7|
|126|6|
|128|5|
|130|6|
|132|7|

这轮 nRF 列表出现设备。证据位于
`tools/android-ble/.build/phone-delay-sweep/{bluetooth-after.txt,hci.btsnoop,accepted-delays.json}`。
数值是软件等待参数，不能直接当作仪器测得的空口 T_IFS。

先只将扫描响应改成120us，保持数据响应108us：手机20秒扫描中有170条primary、
163条不带实验标记的scan response，列表正常；但连接后报133。Android HCI显示
Enhanced Connection Complete status0，随后 Disconnect reason0x3e。板端连接状态
保留 `features_rxd=1`，说明已收到对端特征请求，不能简单归因为未收到CONNECT_IND。

最终将共用 RX→TX 软件等待改为120us，移除实验轮换、AD标记和扫描专用分支。
2M短包的现有修正表仍然生效，没有改动寄存器映射、广播内容或手机过滤器。

## 手机实测

最终共用120us版本，手机完成：

- 连接并发现测试服务 `…3e01`；读取uptime。
- 默认1M下写入并逐字节读回 `00 ff 80 7f 11 22 33`。
- 写PHY控制 `02`，读回 `01 02 02 00 00 02`，确认实际双向2M且无pending/error。
- 2M下写入并逐字节读回64字节 `00..3f`。
- 同一连接从19:02:53保持到19:13:25，约10分32秒；末次uptime639秒。

上述过程未连接GDB或实时RTT。证据目录分别为 `.build/phone-all-120-connect`、
`phone-echo-1m-keyevents`、`phone-phy-2m-pass`、`phone-echo-64-2m`、
`phone-uptime-final`、`phone-disconnect-final`。

切回1M时一轮UI导出遇到窗口切换、未生成XML，脚本正确报错，没有复用旧XML；
这一轮不记作脚本通过。随后保存的HCI环包含成功的PHY更新事件
`3e 06 0c 00 05 00 01 01`，说明协议层实际切回1M。导出助手随后加了有限的
只读重取，写入动作不会因此重复。

主机工具也做了两处实际验证：普通 `uiautomator dump` 在活跃扫描画面可能因
无法idle而失败，所以使用临时shell测试JAR、进程内零idle等待和唯一文件名；
`ACTION_SET_TEXT` 能改变nRF显示文本却发出空值，因此最终使用有节奏的真实按键
输入，并要求GATT回读一致。工具的早期失败记录保留，不与通过记录混计。

## 调试边界与回归

本轮早期另保存过发送挂起现场：LLE CMD=2，CPU任务还在运行，后续TX失败累积。
独立复位后约7.7分钟的快照为TX2746、失败/迟到均0；后来实时RTT attach附近又
出现挂起。尚未确定调试连接与该硬件状态的具体机制，所以没有把一次复位恢复
描述成这个问题已修复。本轮手机和Mac互通测试均独立运行，结束或失败后才取证。

初版共用120us测试ELF SHA256：
`dc6d560121cd5abf46b592c99c87af30f563f664981754bf87a97a45db7ecba4`。
此前84分钟的长测使用108us旧版本，不能直接套用到120us版本。
120us版本的Mac回归结果记录在 `tools/android-ble/.build/mac-120-regression/report.json`。

这仍是当前96MHz板的互通参数。未用射频仪器测量T_IFS，不能据此声称满足全部
蓝牙空口时序要求。


## 后续短测与尚未解决的问题

120us版本Mac回归：1M持续300.04秒、644轮/12466字节通过；随后2M在173.44秒、
370轮后断开。取证发现LL任务的队列事件节点被重复插入，形成自环，CPU停在
`vListInsert`，当时`uxCriticalNesting=1`、`s_critical_nesting=0`。这是独立的
调度/临界区问题，不能用此前84分钟通过或手机短测覆盖掉。

移植层已补关中断后的`fence.i`与编译器memory屏障，任务切换前断言临界区计数，
队列阻塞前断言事件节点未在其他链表中。它们是加固与提前取证，尚未证明彻底
修复上述自环。其后一轮2M在56秒断开，但自动取证遇到探针被占用，后来读取时
目标已经是另一次启动；不能把后来现场当作那次失败的现场。用户随后要求暂停
长测，本轮没有完成新的20分钟回归。

加入GAP(0x1800)、GATT(0x1801)标准服务后，手机实际发现三个服务；原先只有测试
服务是应用未注册标准服务，不能直接作为空口丢包证据。手机完成普通echo、2M
空值echo、2M->1M切换、主动断开后重连并读uptime，过滤器仍为原配置。证据在
`tools/android-ble/.build/services-rtt/`。首轮工具因假定测试服务排第一而失败，
已改成按UUID选择服务，修正后的实际回读通过；7项离线工具检查通过。

日志后端现在保留NimBLE调用方的换行，ACL头、多个hex片段与末尾换行组成一行，
不再给每个字节添加前缀和换行。主机模拟验证了251字节分片dump的行格式。
RTT锁也补了memory屏障，修正解锁asm修改只读输入操作数的问题。原RTT配置的
缓冲区宏名未生效，实际一直是1KB；现使用正确宏名，保持1KB分配。

TX失败路径原先只改软件IDLE，现保存首次错误的CMD/IRQ/window/BB控制字并调用
`ble_phy_disable()`关闭硬件。临时故障注入让首个TX等待立即超时，现场记录CMD=2、
错误数1；后续手机连接与echo回读成功，错误没有被重试抹掉。证据在
`tools/android-ble/.build/recovery-injection/`。注入代码已移除。此结果仅证明清理
路径可恢复这次注入，不能证明所有硬件挂起均已解决。该轮Mac通过旧GATT缓存写到
错误句柄而失败，没有计作PHY互通通过；Android重新发现的服务句柄正常。

最新正常固件ELF SHA256：
`4394616a1e750303c7a76ab34bf81b3ddf0a173d376ed6a77d190fedd42614d6`。
该版本开启RTT后仍复现无法连接：CPU心跳继续，TX错误累积2823，首次失败CMD=2。
清理后硬件CMD虽回0，后续TX仍不能完成，单纯关闭引擎不足以恢复这类故障。
完整现场在`tools/android-ble/.build/final-short/rtt-failure/`，不能描述为已修复。

## 已确认RTT传输会暂停CPU

`tools/android-ble/.build/final-short/rtt-debug.trace`明确记录本板`sbcs=0`，RTT RAM
访问为`read_multiple(A32) ... using ProgramBuffer`。对应本机probe-rs0.32源码的
`perform_memory_read_progbuf`/批量访问经`halted_access`暂停CPU、执行访存后恢复。
因此当前RTT路径会干扰BLE时序，`no-block-skip`并不消除CPU暂停。该证据解释了
运行时日志附加的侵入性，但不能单独解释所有脱机故障。

`run.sh`现在只烧录、正常复位，不自动附加RTT。日志将按用户要求交给另一个agent
迁移为EasyLogger + USART1(PA9/PA10)、921600 + DMA，当前尚未实现串口迁移。
不采用原厂阻塞SDI `_write()`。

时钟实读：HSE on/ready=1/1，LSE=0/0，LSI=0/0；96MHz主频及TIM3 1MHz计时来自
32MHz HSE。启动显式关闭LSI，不新增RTC/低速RC依赖。SCA预算改为SDK外晶体路径
使用的50ppm，避免未经测量就宣称20ppm。HSE负载配置与SDK MCU.c三行一致。
