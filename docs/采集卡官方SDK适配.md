# 采集卡官方 SDK 适配

## 当前入口

| 设备 | 采集入口 | 状态 |
| --- | --- | --- |
| 美乐威 Pro Capture | `Magewell Pro Capture SDK` | 使用官方 Windows Capture SDK 的帧通知和 `MWCaptureVideoFrameToVirtualAddressEx`，由 SDK 根据中心源矩形输出检测分辨率的 BGR 帧。 |
| 美乐威 USB Capture / Eco Capture | 系统采集 | 官方 SDK 的通用回调接口内部使用 DirectShow；目前未接入单独入口。 |
| 圆刚 GC553G2 / GC573 | 系统采集 | 对应消费级产品的官方下载页未提供可确认兼容的公开帧采集 SDK；专业采集产品的 SDK 不作为这两款的适配依据。 |

## 构建美乐威入口

从 [美乐威官网下载 Windows Capture SDK](https://www.magewell.com/downloads) 并安装。配置项目时指定 SDK 安装目录：

```powershell
cmake -S . -B build/cuda -DMAGEWELL_SDK_ROOT="C:/path/to/MagewellSDK"
cmake --build build/cuda --config Release --target ai
```

SDK 目录应包含 `SDKv3/Include/LibMWCapture/MWCapture.h` 和 `SDKv3/Lib/x64/LibMWCapture.lib`。运行机还需安装美乐威的驱动及 SDK Runtime，使 `LibMWCapture.dll` 可加载。未设置 `MAGEWELL_SDK_ROOT` 时程序仍可构建，设备列表不提供美乐威 SDK 入口。

在“采集卡”页选带 `Magewell Pro Capture SDK` 的设备。配置存储设备路径的稳定哈希，不依赖枚举序号。输入无信号时仍可选设备；信号恢复后重新刷新页面可读取当前分辨率和帧率。

SDK 裁切输出不等于已经实测证实设备内部完成硬件裁切；需要实卡测量输出耗时、总链路延迟和 CPU 使用率。当前开发环境未连接美乐威或圆刚采集卡，设备运行测试仍待实卡完成。
