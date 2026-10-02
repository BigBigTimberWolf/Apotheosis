# NDI 接收与桌面发射

主程序在“采集来源”中新增 **NDI · 低延迟接收**。发送端是独立的
`ndi_sender.exe`，无需 Qt、CUDA、TensorRT 或 OBS。标准 NDI High Bandwidth
负责压缩和网络传输，DXGI Desktop Duplication 负责采集桌面，不发送声音和鼠标光标。

## 使用

本次已生成 `build/NDI发射端.zip`，解压后双击“启动发射端.cmd”即可开始发送，
压缩包已含运行库。接收程序为 `build/cuda/Release/Apotheosis.exe`，
其旁边也已准备好 NDI 运行库。

1. 两台电脑接入同一有线局域网，并安装
   [NDI 6 Runtime](https://ndi.link/NDIRedistV6)。也可以使用官方 SDK 中的
   `Processing.NDI.Lib.x64.dll`，放到各自程序旁；部署时保留官方许可证文件并遵守 SDK 的发行条款。
2. 在发送电脑运行：

   ```powershell
   .\ndi_sender.exe --size 320 --fps 120
   ```

3. 在接收电脑打开主程序的采集页面，选 **NDI · 低延迟接收**，点击“发现源”，
   选择 `发送电脑名 (Apotheosis)`。也可以直接输入完整源名。
4. `--size` 尽量和接收端的检测分辨率一致；例如检测分辨率 640，发送端用
   `--size 640`。接收端只做中心裁切，输入小于检测分辨率时才放大。

常用参数：

```powershell
# 列出显示器，序号与 --monitor 对应
.\ndi_sender.exe --list

# 第二个显示器，640×640 中心区域，最多 120 fps
.\ndi_sender.exe --monitor 1 --size 640 --fps 120 --name Apotheosis

# 传整个显示器（对 CPU、GPU 读回、网络的压力更大）
.\ndi_sender.exe --size 0 --fps 60

# 低占用优先
.\ndi_sender.exe --size 320 --fps 60

# 更短采样周期，需显示器和网络有足够余量
.\ndi_sender.exe --size 320 --fps 240

# 自测彩色图案，30 秒后退出
.\ndi_sender.exe --name ApotheosisTest --test-pattern --size 320 --fps 60 --seconds 30
```

Ctrl+C 退出。支持普通横屏显示器；旋转屏幕暂不支持。桌面采集受 Windows 会话、
受保护内容以及显卡驱动限制。锁屏、切换分辨率或设备失效后会重新创建 DXGI 采集。
显示器序号越界时请重新用 `--list` 查询。

## 性能与延迟设计

- 默认发送屏幕中心 **320×320，最高 120 fps**。GPU 先复制 ROI，CPU 只读回 ROI，
  不先把全屏下载到 CPU 再裁切；320×320 的像素量约为 1080p 全屏的 4.9%。
- 三个固定大小的 GPU staging 缓冲，`Map(DO_NOT_WAIT)`；GPU 未完成时跳过等待。
  四个可复用 CPU 图像缓冲，分别容纳采集、待发送和 NDI 正在使用的帧。
- 采集线程和发送线程分离，待发送槽只有一帧，新帧覆盖旧帧；发送拥塞不会积累帧队列。
  已在 NDI 中异步编码的帧无法取消，但其后的待发送帧会持续被更新。
- NDI 关闭自带视频时钟，采集端负责限帧；使用异步视频发送。前一帧内存一直保留到
  下次发送调用返回，退出前调用空帧同步，再释放图像。
- 接收线程直接使用 NDI capture，不使用额外 frame-sync 缓冲。转换前先排掉最多
  16 帧积压，中心区域转 BGR 后投递到单帧槽；主程序消费慢时只拿最新结果。
- 没有接收连接时停掉桌面采集；桌面没有新的显示更新时不重复采集。SDK 持有最近一帧，
  新接收连接能得到当前画面。显示器刷新率是有效画面更新速度的上限。

标准 NDI 仍然需要 SDK 进行 CPU 压缩，BGRA/BGRX 还可能有内部转色开销。
本实现并非 NVENC 硬件编码或 GPU 到网卡零拷贝。低占用主要来自提前裁切、复用内存、
非阻塞读回和限制帧率。240 fps 不代表比 120 fps 必然更快，负载过高会增加延迟。
参见官方 [NDI 异步发送与缓冲所有权](https://docs.ndi.video/all/developing-with-ndi/sdk/ndi-send)。

接收端按**完整源名精确连接**，源离线后等待同名源恢复，不自动切到另一台电脑。
选择结果保存在配置项 `capture_ndi_source`，网络 URL 配置独立保留。

## 统计与实测

发射端每两秒输出连接状态、读回产帧率、实际提交帧率、过期丢弃数量、NDI 提交调用的
平均/最大耗时，以及提交前帧在本机采集管线里的最大停留时间。
过期阈值为 `max(50ms, 3 × 帧周期)`。

这些数值**不是双机端到端延迟**。接收端的帧时间戳从 NDI 解码完成、返回帧时开始，
用于后续处理时效，不能包含跨机传输时间。
双机延迟请用发送屏幕的毫秒计时器和接收预览同框录像测量，或使用可校准的时钟同步测量方案。
CPU/GPU 占用请在相同游戏场景下比较“关闭发送”“320/60”“320/120”“640/120”，
同时观察帧时间 P95/P99；实际结果取决于显卡、CPU、显示器和网络。

源发现不到时，先检查发送端正在运行、两端防火墙允许程序、同网段和网络发现，
避免 VPN/虚拟网卡选路干扰。跨网段源发现可通过官方 NDI Access Manager 配置，
此版本页面只提供源名发现和选择。

## 构建与回归

本仓库使用 CMake 构建。NDI 头文件已随仓库提供，运行库动态加载；编译不需要安装 NDI SDK。

```powershell
# 只构建轻量发送端
cmake -S tools/ndi_sender -B build/ndi-sender -G "Visual Studio 18 2026" -A x64
cmake --build build/ndi-sender --config Release

# 主程序及 NDI 回归（沿用已配置的 CUDA/Qt/TensorRT 构建目录）
cmake -S . -B build/cuda
cmake --build build/cuda --config Release --target ai ndi_sender ndi_capture_test ndi_latest_slot_test config_migration_test

# 内存所有权、单帧覆盖、唤醒退出及配置保存回归
ctest --test-dir build/cuda -C Release -R "ndi_latest_slot_test|config_migration_test" --output-on-failure

# 真实 NDI 回环（自动启动独立测试发射端，并模拟发射端退出/重启）
ctest --test-dir build/cuda -C Release -R "^ndi_capture_test$" --output-on-failure
```

主程序名称固定为 `Apotheosis.exe`。重新编译前退出正在运行的主程序，以便更新文件。

`ndi_capture_test` 验证发现源、中心裁切、BGR 通道、慢消费者获取最新帧、图像所有权、
缺失源不被替换、发射端重启后的重连和限时退出。运行库不存在时返回 77，由 CTest 标记为跳过。
回环不等于真实双机网络性能测试。

2026-09-30 本机验证：单帧槽和配置保存回归、真实 NDI 彩色图案回环、DXGI 桌面 ROI
读回与 NDI 接收均通过；回环包含发射端突然退出并以同名重启后的恢复。
尚未测量真实双机局域网延迟、游戏帧时间变化或 CPU/GPU 占用百分比。
