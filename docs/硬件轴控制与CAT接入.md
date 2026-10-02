# 轴控制与 CAT / Ferrum 接入（2026-09-28）

## 两组轴开关

在瞄准设置的 FOV 下方，X/Y 各自独立：

- **屏蔽轴**：拦截连接到硬件上的真实鼠标在该方向的移动，程序注入的移动照常发送。运行中激活瞄准热键时生效，松键、停止运行、切换设备或退出时解除。热键设为“无 / 始终活跃”时，运行期间持续生效。
- **解锁轴**：程序不在该方向执行自动瞄准，真实鼠标输入不受此开关影响。PID 状态和轨迹输出同时处理，避免残留自动位移。

两项可以独立组合。INI 中 `mask_x/y` 现在表示真实输入屏蔽；新字段 `unlock_x/y` 表示停止自动瞄准，默认关闭。局域网页面同步提供四个开关。

预览和回放的 `Input mask requested` 是配置请求，`Aim unlock` 是自动瞄准的轴设置。硬件命令结果看 FOV 下方状态提示；串口 / UDP 写入成功并不代表已经完成实机验证。

| 输入方式 | 真实轴屏蔽 | 解锁自动瞄准 |
| --- | --- | --- |
| MAKCU | `km.lock_mx/my`，查询固件确认 | 支持 |
| MAKCUNEW | 支持轴锁的 ASCII 固件；旧二进制固件不支持 | 支持 |
| KMBox Net | SDK 的独立 X/Y 屏蔽 | 支持 |
| Ferrum | Software / Legacy API 的 `km.lock_mx/my` | 支持 |
| DHZBox Mini | `mask_x/y`，只能确认命令已发送 | 支持 |
| Windows 原生 | 当前通道不支持可靠的物理轴屏蔽 | 支持 |
| CAT | 用户提供的协议没有明确的轴屏蔽命令 | 支持 |

使用本项目自制 MAKCU 鼠标固件时，需要更新鼠标设备的 **LEFT 板**到 `CUSTOM_V1_2_AXIS`，RIGHT 板无需为本功能更新。程序不会自动刷写设备。新固件在真实输入与注入输入合并前过滤轴，并在开关变化时清掉该轴待发送的真实位移，避免解除后补发积压。

## CAT

硬件页选择 **CAT（加密网络）**，填写盒子 IP、命令端口、8 位十六进制 UUID、本机监听端口。默认 IP `192.168.7.1`、命令端口 `8888`、监听端口 `1234`；UUID 不填示例值，使用实际盒子的值。两个端口须不同且本机可用。

根据用户提供的 `cat协议.rar` 内 `cat_net.h/.cpp` 实现：

- UDP；7 字节小端命令；AES-128-CBC + PKCS#7，报文前缀为 16 字节 IV。
- UUID 转为小写 ASCII 后补 `0` 至 16 字节作为密钥，与示例一致；加密使用 Windows CNG。
- 连接和监听订阅收到解密后的匹配 ACK 才视为就绪。发送和接收均有等待上限，丢弃非目标端返回的 ACK。
- 支持相对移动、五个鼠标键、键盘按下 / 松开、物理按键监听、逐按键屏蔽。项目 HID 键码转换为 CAT 使用的 Linux 事件码。
- 宏、瞄准热键、参数选择键使用统一物理按键读取；宏键盘输出接入同一驱动。
- SDK 未定义独立轴屏蔽和滚轮输出，界面 / 宏校验明确提示，不用猜测的命令代替。

协议 ACK 只带命令类型，没有请求序号，无法从协议上完全区分同类型的极迟到回包。驱动在每次发送前清理旧回包并串行发送。按键监控为事件推送，连接后可先按下再松开一次需要监听的键。

## Ferrum 官方协议核对

核对入口：[官方文档](https://docs.xferrum.dev/)。原文档域名已经重定向到这里。

2026-10-02 再次核对官方完整文档和 [官方文档仓库](https://github.com/FerrumLLC/ferrumllc.github.io)。公开主分支最新提交为 `fe83ca1`（2026-01-06）；未找到另一个已公开的新协议。现有串口移动、按键、轴锁、键盘屏蔽和回调命令与当前 Software API 一致。本次接入 [组合键批量接口](https://docs.xferrum.dev/software_api/km_api/keyboard/keys/multi.html)：组合键使用 `km.multidown` / `km.multiup` 同份指令按下或释放；单键仍使用 `km.down` / `km.up`。保留软件指定按住时长，不改用固件随机时长的 `km.multipress`。Legacy API 不发送键盘命令。协议构造经过离线测试，未连接 Ferrum 实机验证。

- 优先连接 Ferrum App 提供的虚拟串口，版本响应为 `kmbox: Ferrum`，支持 Software API 的键鼠操作。
- 直连 CP210x 为 Legacy API，版本响应为 `kmbox: 2.0.0 Aug 31 2020 21:49:51`，只支持文档列出的鼠标接口；重上电默认 115200 波特率。
- 本次补全旧接口识别、新接口键盘宏 `km.down/up`、侧键 `km.side1/side2`、真实轴屏蔽。按接口能力启用键盘监听，避免给 Legacy API 发送键盘命令。
- 官方文档还提供 `km.catch_xy` 和 `km.axes` 等物理移动读取接口，本次不将它们接入 PID 或预测，保持现有控制算法。

依据：[FE 轴锁](https://docs.xferrum.dev/software_api/km_api/mouse/axes/lock.html)、[FE 官方完整文档](https://docs.xferrum.dev/print.html)、[DHZBox Mini 厂商协议手册副本](https://www.scribd.com/document/1077423048/Dhzbox-Mini-%E9%80%8F%E4%BC%A0%E9%BC%A0%E6%A0%87%E7%89%88%E6%9C%AC)。

Windows 的 Raw Input 可直接接收设备数据，和普通鼠标窗口消息是两条输入路径，当前 SendInput 后端不能据此宣称能屏蔽所有游戏的真实轴输入。参见 [Microsoft Raw Input](https://learn.microsoft.com/en-us/windows/win32/inputdev/about-raw-input)。
