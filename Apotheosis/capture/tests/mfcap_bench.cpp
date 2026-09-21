
// 采集卡链路 bench —— 把「卡/MF 投递速率」和「程序消费速率」分开量。
//
// 为什么要分开量: 这两个数在 UI 上长得一样, 但瓶颈位置完全不同。
//   投递速率(GetSourceFpsEstimate = 读循环里 TickFps 记的 sample 到达率):
//       低 => 问题在 MF 读循环本身 (每帧同步开销、串行的 ReadSample 往返)
//   消费速率(本 bench 从输出队列取到帧的速率):
//       低而投递高 => 问题在解码 worker / 输出队列 (背压丢帧)
//
// 用法:
//   mfcap_bench [device] [FORMAT] [W] [H] [fps] [out_side] [秒数]
// 例:
//   mfcap_bench "KUHAIMI 4K60 Version" MJPG 1920 1080 240 256 8
//   mfcap_bench "KUHAIMI 4K60 Version" NV12 1920 1080 120 256 8
//
// 注意: 本工具会独占采集卡, 跑之前必须关掉主程序 (否则设备激活失败)。

#define _WINSOCKAPI_
#include <windows.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

#include "capture/mf_capture.h"
#include "runtime/latency_probe.h"

namespace
{

double NowSec()
{
    using clock = std::chrono::steady_clock;
    static const clock::time_point start = clock::now();
    return std::chrono::duration<double>(clock::now() - start).count();
}

}

int main(int argc, char** argv)
{
    SetConsoleOutputCP(CP_UTF8);
    setvbuf(stdout, nullptr, _IONBF, 0);

    const std::string wantDevice = (argc > 1) ? argv[1] : "KUHAIMI 4K60 Version";
    const std::string wantFormat = (argc > 2) ? argv[2] : "MJPG";
    const int wantW      = (argc > 3) ? std::atoi(argv[3]) : 1920;
    const int wantH      = (argc > 4) ? std::atoi(argv[4]) : 1080;
    const int wantFps    = (argc > 5) ? std::atoi(argv[5]) : 240;
    const int outSide    = (argc > 6) ? std::atoi(argv[6]) : 256;
    const double seconds = (argc > 7) ? std::atof(argv[7]) : 8.0;

    printf("=== 采集链路 bench ===\n");
    printf("目标: \"%s\"  %s %dx%d @%dfps  -> out %dx%d  (与主程序一致: crop_enabled=true)\n\n",
           wantDevice.c_str(), wantFormat.c_str(), wantW, wantH, wantFps, outSide, outSide);

    const auto devices = MFCapture::EnumerateDevices();
    int deviceIndex = -1;
    for (const auto& d : devices)
    {
        printf("  [%d] %s\n", d.index, d.friendly_name.c_str());
        if (d.friendly_name == wantDevice)
            deviceIndex = d.index;
    }
    if (deviceIndex < 0)
    {
        printf("\n[FAIL] 找不到设备 \"%s\"。主程序也按 friendly name 精确匹配。\n", wantDevice.c_str());
        return 2;
    }
    printf("\n");

    const bool cropEnabled = true;   // ★ 与 capture.cpp:456 保持一致 (那里是硬编码 true)
    MFCapture cap(wantW, wantH, outSide, cropEnabled, wantFps, wantFormat, deviceIndex, true);

    // 接收线程是异步的: 构造函数只负责起线程就返回, is_open_ 要等协商完成才置位。
    // 所以这里必须等, 不能立刻查 IsOpen()。
    const double openDeadline = NowSec() + 5.0;
    while (!cap.IsOpen() && !cap.HasStopped() && NowSec() < openDeadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));

    if (!cap.IsOpen())
    {
        printf("[FAIL] 采集链路没打开: %s\n", cap.LastError().c_str());
        printf("       最常见原因: 主程序/OBS/相机应用正占着这张卡。\n");
        return 3;
    }
    printf("\n开始采集 %.1f 秒, 全速消费输出队列...\n", seconds);
    printf("%8s %10s %10s %12s %10s\n", "t(s)", "投递fps", "消费fps", "端到端延迟ms", "空轮询%");

    const double t0 = NowSec();
    long long consumed = 0;
    long long polls = 0;
    long long emptyPolls = 0;
    double latencySumMs = 0.0;
    long long latencySamples = 0;
    double lastReport = 0.0;
    long long lastReportConsumed = 0;

    while (true)
    {
        const double elapsed = NowSec() - t0;
        if (elapsed >= seconds)
            break;

        // ★ 纯自旋消费, 不许 sleep。
        // 原因: Windows 默认定时器精度 ~15.6ms, sleep_for(200us) 实际会睡满一个
        // tick, 那样量到的是"消费者自己的睡觉粒度", 不是采集管线的天花板。
        GpuImage frame = cap.GetNextFrameGpu();
        ++polls;
        if (frame.empty())
        {
            ++emptyPolls;
        }
        else
        {
            ++consumed;

            const int64_t captureNs = cap.GetLastFrameCaptureNs();
            if (captureNs > 0)
            {
                const double ms = static_cast<double>(runtime::latency::nowNs() - captureNs) / 1.0e6;
                if (ms >= 0.0 && ms < 1000.0)
                {
                    latencySumMs += ms;
                    ++latencySamples;
                }
            }
        }

        const double now = NowSec() - t0;
        if (now - lastReport >= 1.0)
        {
            const double win = now - lastReport;
            const double consumeFps = static_cast<double>(consumed - lastReportConsumed) / win;
            const double emptyPct = polls > 0
                ? 100.0 * static_cast<double>(emptyPolls) / static_cast<double>(polls) : 0.0;
            const double latMs = latencySamples > 0 ? latencySumMs / latencySamples : -1.0;
            printf("%8.1f %10d %10.1f %12.2f %10.1f\n",
                   now, cap.GetSourceFpsEstimate(), consumeFps, latMs, emptyPct);
            lastReport = now;
            lastReportConsumed = consumed;
        }
    }

    const double total = NowSec() - t0;
    const int delivered = cap.GetSourceFpsEstimate();
    const double consumeFps = static_cast<double>(consumed) / total;

    printf("\n=== 结果 ===\n");
    printf("  协商/投递 (卡->MF读循环): %d fps\n", delivered);
    printf("  程序消费 (输出队列出帧): %.1f fps  (%lld 帧 / %.2fs)\n", consumeFps, consumed, total);
    if (latencySamples > 0)
        printf("  端到端延迟 (capture->consume): 平均 %.2f ms\n", latencySumMs / latencySamples);
    if (delivered > 0)
        printf("  消费/投递 = %.1f%%\n", 100.0 * consumeFps / delivered);

    printf("\n判读:\n");
    printf("  * 投递就低      -> 瓶颈在读循环 (ReadSample 串行往返 / 每帧拷贝开销)\n");
    printf("  * 投递高消费低  -> 瓶颈在解码 worker / 输出队列背压\n");
    printf("  * 两者都到目标  -> 链路已经到顶\n");
    return 0;
}
