
// MJPEG 解码耗时 vs 压缩体积 —— 多点扫描, 判定"固定开销"是否真实存在。
//
// 动机: 之前用两个样本点 (343KB, 924KB) 做线性拟合, 得到 "截距 3.5ms"
// 并据此宣称"存在与体积无关的固定开销"。两点定直线, 截距可以任意 ——
// 那不是证据。这里用同一张真实帧重编码出 8~10 个体积档位, 用多点回归
// 判断 耗时(体积) 到底是不是线性、截距是否显著。
//
// 用法: mjpg_sweep <src.jpg> [ROI边=416] [每档迭代=150]

#include <nvjpeg.h>
#include <cuda_runtime.h>
#include <opencv2/opencv.hpp>

#define WIN32_LEAN_AND_MEAN
#define _WINSOCKAPI_
// windows.h 定义的 min/max 宏会破坏 std::min/std::max, 必须先禁用。
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace
{

double NowMs()
{
    using clock = std::chrono::steady_clock;
    static const clock::time_point start = clock::now();
    return std::chrono::duration<double, std::milli>(clock::now() - start).count();
}

struct Point
{
    int    quality = 0;
    size_t bytes = 0;
    double hostMs = 0.0;
    double syncMs = 0.0;
    bool   ok = false;
};

// 用 split+ROI + GPU_HYBRID 解一帧, 返回纯主机耗时与含同步耗时 (均值)
bool measure(const std::vector<unsigned char>& jpeg, int roiSide, int iters,
             double& hostMs, double& syncMs)
{
    nvjpegHandle_t handle = nullptr;
    nvjpegJpegDecoder_t decoder = nullptr;
    nvjpegDecodeParams_t params = nullptr;
    nvjpegJpegState_t state = nullptr;
    nvjpegBufferPinned_t pinned = nullptr;
    nvjpegBufferDevice_t device = nullptr;
    nvjpegJpegStream_t jstream = nullptr;
    cudaStream_t stream = nullptr;

    auto fail = [&]() {
        if (jstream) nvjpegJpegStreamDestroy(jstream);
        if (device)  nvjpegBufferDeviceDestroy(device);
        if (pinned)  nvjpegBufferPinnedDestroy(pinned);
        if (state)   nvjpegJpegStateDestroy(state);
        if (params)  nvjpegDecodeParamsDestroy(params);
        if (decoder) nvjpegDecoderDestroy(decoder);
        if (handle)  nvjpegDestroy(handle);
        if (stream)  cudaStreamDestroy(stream);
        return false;
    };

    if (cudaStreamCreateWithFlags(&stream, cudaStreamNonBlocking) != cudaSuccess) return false;
    if (nvjpegCreateEx(NVJPEG_BACKEND_GPU_HYBRID, nullptr, nullptr, 0, &handle) != NVJPEG_STATUS_SUCCESS)
        return fail();
    if (nvjpegDecoderCreate(handle, NVJPEG_BACKEND_GPU_HYBRID, &decoder) != NVJPEG_STATUS_SUCCESS)
        return fail();
    if (nvjpegDecodeParamsCreate(handle, &params) != NVJPEG_STATUS_SUCCESS) return fail();
    nvjpegDecodeParamsSetOutputFormat(params, NVJPEG_OUTPUT_BGRI);
    if (nvjpegDecoderStateCreate(handle, decoder, &state) != NVJPEG_STATUS_SUCCESS) return fail();
    if (nvjpegBufferPinnedCreate(handle, nullptr, &pinned) != NVJPEG_STATUS_SUCCESS) return fail();
    if (nvjpegBufferDeviceCreate(handle, nullptr, &device) != NVJPEG_STATUS_SUCCESS) return fail();
    if (nvjpegJpegStreamCreate(handle, &jstream) != NVJPEG_STATUS_SUCCESS) return fail();
    nvjpegStateAttachPinnedBuffer(state, pinned);
    nvjpegStateAttachDeviceBuffer(state, device);

    if (nvjpegJpegStreamParse(handle, jpeg.data(), jpeg.size(), 0, 0, jstream) != NVJPEG_STATUS_SUCCESS)
        return fail();

    unsigned int fw = 0, fh = 0;
    if (nvjpegJpegStreamGetFrameDimensions(jstream, &fw, &fh) != NVJPEG_STATUS_SUCCESS) return fail();
    const int roiW = std::min(roiSide, static_cast<int>(fw));
    const int roiH = std::min(roiSide, static_cast<int>(fh));
    const int offX = std::max(0, (static_cast<int>(fw) - roiW) / 2) & ~1;
    const int offY = std::max(0, (static_cast<int>(fh) - roiH) / 2) & ~1;
    if (nvjpegDecodeParamsSetROI(params, offX, offY, roiW, roiH) != NVJPEG_STATUS_SUCCESS)
        return fail();

    unsigned char* dOut = nullptr;
    if (cudaMalloc(&dOut, static_cast<size_t>(roiW) * roiH * 3) != cudaSuccess) return fail();

    nvjpegImage_t out{};
    out.channel[0] = dOut;
    out.pitch[0] = static_cast<unsigned int>(roiW * 3);

    hostMs = 0.0;
    syncMs = 0.0;
    const int warm = 5;
    int counted = 0;

    for (int i = 0; i < warm + iters; ++i)
    {
        const double a = NowMs();
        nvjpegStatus_t st = nvjpegDecodeJpegHost(handle, decoder, state, params, jstream);
        if (st == NVJPEG_STATUS_SUCCESS)
            st = nvjpegDecodeJpegTransferToDevice(handle, decoder, state, jstream, stream);
        if (st == NVJPEG_STATUS_SUCCESS)
            st = nvjpegDecodeJpegDevice(handle, decoder, state, &out, stream);
        const double b = NowMs();
        cudaStreamSynchronize(stream);
        const double c = NowMs();

        if (st != NVJPEG_STATUS_SUCCESS)
        {
            cudaFree(dOut);
            return fail();
        }
        if (i >= warm)
        {
            hostMs += (b - a);
            syncMs += (c - a);
            ++counted;
        }
    }

    if (counted > 0)
    {
        hostMs /= counted;
        syncMs /= counted;
    }
    cudaFree(dOut);
    fail();   // 复用清理逻辑 (它返回 false, 这里忽略)
    return counted > 0;
}

} // namespace

int main(int argc, char** argv)
{
    SetConsoleOutputCP(CP_UTF8);
    setvbuf(stdout, nullptr, _IONBF, 0);

    const char* path = (argc > 1) ? argv[1] : "build\\diag\\live_frame.jpg";
    const int roiSide = (argc > 2) ? std::atoi(argv[2]) : 416;
    const int iters = (argc > 3) ? std::atoi(argv[3]) : 150;

    cv::Mat src = cv::imread(path, cv::IMREAD_COLOR);
    if (src.empty()) { printf("读不到图片: %s\n", path); return 2; }

    printf("=== MJPEG 解码耗时 vs 体积 多点扫描 ===\n");
    printf("源帧: %s  (%dx%d)\n", path, src.cols, src.rows);
    printf("ROI: %dx%d   每档迭代: %d   (GPU_HYBRID split+ROI)\n\n", roiSide, roiSide, iters);

    // 从高到低扫质量, 覆盖宽范围体积
    const std::vector<int> qualities = { 100, 98, 95, 90, 85, 80, 70, 60, 50, 40 };
    const std::vector<int> subsample = { 0, 0, 0, 0, 1, 1, 1, 2, 2, 2 };

    std::vector<Point> pts;
    for (size_t i = 0; i < qualities.size(); ++i)
    {
        std::vector<unsigned char> buf;
        const std::vector<int> p = { cv::IMWRITE_JPEG_QUALITY, qualities[i],
                                     cv::IMWRITE_JPEG_SAMPLING_FACTOR, subsample[i] ? 2 : 0 };
        try { cv::imencode(".jpg", src, buf, p); }
        catch (...) { continue; }
        if (buf.empty()) continue;

        Point pt;
        pt.quality = qualities[i];
        pt.bytes = buf.size();
        pt.ok = measure(buf, roiSide, iters, pt.hostMs, pt.syncMs);
        pts.push_back(pt);

        printf("q=%3d  体积 %7zu B (%6.1f KB)  host=%6.2f ms  host+sync=%6.2f ms  %s\n",
               pt.quality, pt.bytes, pt.bytes / 1024.0, pt.hostMs, pt.syncMs,
               pt.ok ? "" : "<解不出来>");
    }

    // ── 最小二乘回归 host = a + b*KB, 并给出残差 ──────────────────────────
    std::vector<Point> v;
    for (const auto& p : pts) if (p.ok) v.push_back(p);
    if (v.size() < 3) { printf("\n有效点不足, 无法回归\n"); return 1; }

    const double n = static_cast<double>(v.size());
    double sx = 0, sy = 0, sxx = 0, sxy = 0;
    for (const auto& p : v)
    {
        const double x = p.bytes / 1024.0;
        sx += x; sy += p.hostMs; sxx += x * x; sxy += x * p.hostMs;
    }
    const double denom = n * sxx - sx * sx;
    const double b = (n * sxy - sx * sy) / denom;
    const double a = (sy - b * sx) / n;

    double ssTot = 0, ssRes = 0;
    const double ybar = sy / n;
    for (const auto& p : v)
    {
        const double x = p.bytes / 1024.0;
        const double pred = a + b * x;
        ssRes += (p.hostMs - pred) * (p.hostMs - pred);
        ssTot += (p.hostMs - ybar) * (p.hostMs - ybar);
    }
    const double r2 = ssTot > 0 ? 1.0 - ssRes / ssTot : 0.0;

    printf("\n=== 最小二乘回归 (host 时间 = a + b*KB) ===\n");
    printf("  截距 a = %+.3f ms      斜率 b = %.5f ms/KB\n", a, b);
    printf("  R^2 = %.4f   (1.0 表示完美线性)\n", r2);
    printf("  残差标准差 = %.3f ms\n\n", std::sqrt(ssRes / n));

    // 逐点残差, 看线性模型是否成立
    printf("=== 逐点残差 ===\n");
    for (const auto& p : v)
    {
        const double x = p.bytes / 1024.0;
        const double pred = a + b * x;
        printf("  %6.1f KB  实测 %6.2f  预测 %6.2f  残差 %+6.2f ms\n",
               x, p.hostMs, pred, p.hostMs - pred);
    }

    // ── 外推到目标帧率 ────────────────────────────────────────────────────
    printf("\n=== 外推: 达到各帧率的体积上限 ===\n");
    const double budgets[] = { 4.167, 5.0, 6.0 };
    const char* labels[] = { "240fps", "200fps", "167fps" };
    for (int i = 0; i < 3; ++i)
    {
        if (b <= 0) break;
        const double kb = (budgets[i] - a) / b;
        if (kb <= 0)
            printf("  %s (预算 %.2f ms): 即使体积压到 0 也达不到 (截距 %.2f ms 已超预算)\n",
                   labels[i], budgets[i], a);
        else
            printf("  %s (预算 %.2f ms): 体积需 <= %.0f KB\n", labels[i], budgets[i], kb);
    }

    printf("\n=== 判读 ===\n");
    if (a > 2.5 && r2 > 0.9)
        printf(">>> 截距 %.2f ms 显著且线性度好 (R^2=%.3f) —— 【固定开销确实存在】,\n"
               "    只靠降体积无法把 240fps 跑满, 需要从算法/多 worker 侧想办法。\n", a, r2);
    else if (a <= 2.5 && r2 > 0.9)
        printf(">>> 截距只有 %.2f ms —— 固定开销【不显著】, 耗时几乎全由体积决定。\n"
               "    降体积是有效手段。\n", a);
    else
        printf(">>> 线性度差 (R^2=%.3f) —— 简单线性模型不成立, 不能用'截距'解释。\n"
               "    我先前那个 3.5ms 固定开销的说法没有依据。\n", r2);

    return 0;
}
