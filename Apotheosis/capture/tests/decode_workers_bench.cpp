
// 解码 worker 池吞吐基准 —— 回答"几个 worker × 几个核 才能跑满 240fps"。
//
// 背景: mf_capture.h 里 DECODE_WORKERS=2, 注释声称"合成耗时约 3.0ms/帧"。
// 实测真实帧单帧 host 约 6.07ms, 且进程原先被亲和性 bug 钉在 2 个核上。
// 这个基准用真实帧 + 真实 nvJPEG split+ROI 路径, 扫 (worker 数 × 核数),
// 给出各组合的实际吞吐, 直接对照 240fps 预算。
//
// 用法: decode_workers_bench <frame.jpg> [ROI=416] [每轮秒数=2]
//   自动遍历 worker ∈ {1,2,3,4,6} × mask ∈ {0xC(2核), 0xF(4核)}

#include <nvjpeg.h>
#include <cuda_runtime.h>

#define WIN32_LEAN_AND_MEAN
#define _WINSOCKAPI_
#define NOMINMAX
#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace
{

using Clock = std::chrono::steady_clock;

double NowMs()
{
    static const Clock::time_point start = Clock::now();
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

int LoadFile(const char* path, std::vector<unsigned char>& out)
{
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) return -1;
    const std::streamsize size = in.tellg();
    in.seekg(0, std::ios::beg);
    out.resize(static_cast<size_t>(size));
    in.read(reinterpret_cast<char*>(out.data()), size);
    return in ? 0 : -1;
}

// 与 GpuJpegDecoder 同构: 每线程独立 handle + 独立 state/buffer/stream。
struct WorkerCtx
{
    nvjpegHandle_t handle = nullptr;
    nvjpegJpegDecoder_t decoder = nullptr;
    nvjpegDecodeParams_t params = nullptr;
    nvjpegJpegState_t state = nullptr;
    nvjpegBufferPinned_t pinned = nullptr;
    nvjpegBufferDevice_t device = nullptr;
    nvjpegJpegStream_t jstream = nullptr;
    cudaStream_t stream = nullptr;
    unsigned char* dOut = nullptr;
    int roiW = 0, roiH = 0;

    bool init(int roiSide)
    {
        if (cudaStreamCreateWithFlags(&stream, cudaStreamNonBlocking) != cudaSuccess) return false;
        if (nvjpegCreateEx(NVJPEG_BACKEND_GPU_HYBRID, nullptr, nullptr, 0, &handle) != NVJPEG_STATUS_SUCCESS) return false;
        if (nvjpegDecoderCreate(handle, NVJPEG_BACKEND_GPU_HYBRID, &decoder) != NVJPEG_STATUS_SUCCESS) return false;
        if (nvjpegDecodeParamsCreate(handle, &params) != NVJPEG_STATUS_SUCCESS) return false;
        nvjpegDecodeParamsSetOutputFormat(params, NVJPEG_OUTPUT_BGRI);
        if (nvjpegDecoderStateCreate(handle, decoder, &state) != NVJPEG_STATUS_SUCCESS) return false;
        if (nvjpegBufferPinnedCreate(handle, nullptr, &pinned) != NVJPEG_STATUS_SUCCESS) return false;
        if (nvjpegBufferDeviceCreate(handle, nullptr, &device) != NVJPEG_STATUS_SUCCESS) return false;
        if (nvjpegJpegStreamCreate(handle, &jstream) != NVJPEG_STATUS_SUCCESS) return false;
        nvjpegStateAttachPinnedBuffer(state, pinned);
        nvjpegStateAttachDeviceBuffer(state, device);

        roiW = roiH = roiSide;
        if (cudaMalloc(&dOut, static_cast<size_t>(roiW) * roiH * 3) != cudaSuccess) return false;
        return true;
    }

    bool decode(const std::vector<unsigned char>& jpeg, int roiSide)
    {
        if (nvjpegJpegStreamParse(handle, jpeg.data(), jpeg.size(), 0, 0, jstream) != NVJPEG_STATUS_SUCCESS)
            return false;

        unsigned int fw = 0, fh = 0;
        if (nvjpegJpegStreamGetFrameDimensions(jstream, &fw, &fh) != NVJPEG_STATUS_SUCCESS) return false;
        const int W = std::min(roiSide, static_cast<int>(fw));
        const int H = std::min(roiSide, static_cast<int>(fh));
        const int offX = std::max(0, (static_cast<int>(fw) - W) / 2) & ~1;
        const int offY = std::max(0, (static_cast<int>(fh) - H) / 2) & ~1;
        if (W != roiW || H != roiH)
        {
            cudaFree(dOut);
            roiW = W; roiH = H;
            if (cudaMalloc(&dOut, static_cast<size_t>(roiW) * roiH * 3) != cudaSuccess) return false;
        }
        if (nvjpegDecodeParamsSetROI(params, offX, offY, roiW, roiH) != NVJPEG_STATUS_SUCCESS) return false;

        nvjpegImage_t out{};
        out.channel[0] = dOut;
        out.pitch[0] = static_cast<unsigned int>(roiW * 3);

        nvjpegStatus_t st = nvjpegDecodeJpegHost(handle, decoder, state, params, jstream);
        if (st == NVJPEG_STATUS_SUCCESS)
            st = nvjpegDecodeJpegTransferToDevice(handle, decoder, state, jstream, stream);
        if (st == NVJPEG_STATUS_SUCCESS)
            st = nvjpegDecodeJpegDevice(handle, decoder, state, &out, stream);
        if (st != NVJPEG_STATUS_SUCCESS) return false;

        // 同步点与 mf_capture 一致: 让 GPU 工作真的完成, 避免只测了入队速度。
        cudaStreamSynchronize(stream);
        return true;
    }

    void destroy()
    {
        if (dOut) cudaFree(dOut);
        if (jstream) nvjpegJpegStreamDestroy(jstream);
        if (device) nvjpegBufferDeviceDestroy(device);
        if (pinned) nvjpegBufferPinnedDestroy(pinned);
        if (state) nvjpegJpegStateDestroy(state);
        if (params) nvjpegDecodeParamsDestroy(params);
        if (decoder) nvjpegDecoderDestroy(decoder);
        if (handle) nvjpegDestroy(handle);
        if (stream) cudaStreamDestroy(stream);
    }
};

struct Result { int workers; DWORD_PTR mask; double fps; bool ok; };

// 跑一轮: 启动 n 个 worker 并发解码 seconds 秒, 返回聚合 fps。
Result runRound(const std::vector<unsigned char>& jpeg, int roiSide, int nWorkers,
                DWORD_PTR mask, double seconds)
{
    Result r{ nWorkers, mask, 0.0, false };

    DWORD_PTR prev = 0, sys = 0;
    GetProcessAffinityMask(GetCurrentProcess(), &prev, &sys);
    if (mask != 0)
        SetProcessAffinityMask(GetCurrentProcess(), mask);

    std::vector<WorkerCtx> ctxs(nWorkers);
    for (auto& c : ctxs)
        if (!c.init(roiSide)) { for (auto& d : ctxs) d.destroy(); return r; }

    std::atomic<bool> stop{ false };
    std::atomic<long long> total{ 0 };
    std::vector<std::thread> threads;

    for (int i = 0; i < nWorkers; ++i)
    {
        threads.emplace_back([&, i] {
            WorkerCtx& c = ctxs[i];
            // 预热: nvJPEG 首次调用会分配内部缓冲, 不计入统计。
            for (int w = 0; w < 5; ++w) c.decode(jpeg, roiSide);
            long long n = 0;
            while (!stop.load(std::memory_order_relaxed))
            {
                if (c.decode(jpeg, roiSide)) ++n;
            }
            total.fetch_add(n, std::memory_order_relaxed);
        });
    }

    const auto t0 = Clock::now();
    std::this_thread::sleep_for(std::chrono::duration<double>(seconds));
    stop.store(true);
    for (auto& t : threads) t.join();
    const double elapsed = std::chrono::duration<double>(Clock::now() - t0).count();

    const long long frames = total.load();
    r.fps = elapsed > 0 ? frames / elapsed : 0.0;
    r.ok = frames > 0;

    for (auto& c : ctxs) c.destroy();
    if (mask != 0 && prev != 0)
        SetProcessAffinityMask(GetCurrentProcess(), prev);
    return r;
}

} // namespace

int main(int argc, char** argv)
{
    SetConsoleOutputCP(CP_UTF8);
    setvbuf(stdout, nullptr, _IONBF, 0);

    const char* path = (argc > 1) ? argv[1] : "build\\diag\\live_frame.jpg";
    const int roiSide = (argc > 2) ? std::atoi(argv[2]) : 416;
    const double secs = (argc > 3) ? std::atof(argv[3]) : 2.0;

    std::vector<unsigned char> jpeg;
    if (LoadFile(path, jpeg) != 0 || jpeg.empty()) { printf("读不到: %s\n", path); return 2; }

    SYSTEM_INFO si;
    GetSystemInfo(&si);
    printf("=== 解码 worker 池吞吐基准 ===\n");
    printf("帧: %s (%.1f KB)   ROI: %dx%d   本机逻辑核: %lu   每轮 %.1fs\n\n",
           path, jpeg.size() / 1024.0, roiSide, roiSide,
           static_cast<unsigned long>(si.dwNumberOfProcessors), secs);

    if (cudaSetDevice(0) != cudaSuccess) { printf("[FAIL] cudaSetDevice\n"); return 3; }
    cudaFree(nullptr);

    struct Combo { DWORD_PTR mask; const char* label; };
    const Combo combos[] = {
        { 0xC, "2 核 (0xC, 修复前)" },
        { 0xF, "4 核 (0xF, 修复后)" },
    };
    const int workerCounts[] = { 1, 2, 3, 4, 6 };

    printf("%-20s", "worker 数");
    for (const auto& c : combos) printf(" %22s", c.label);
    printf("   (目标 240fps)\n");

    for (int w : workerCounts)
    {
        printf("%-20d", w);
        for (const auto& c : combos)
        {
            const Result r = runRound(jpeg, roiSide, w, c.mask, secs);
            if (!r.ok) printf(" %22s", "失败");
            else printf(" %12.1f fps%s", r.fps, r.fps >= 240.0 ? " OK" : "   ");
        }
        printf("\n");
    }

    printf("\n=== 判读 ===\n");
    printf("  * 同一 worker 数下 2 核 vs 4 核的差 = 亲和性 bug 修掉的收益\n");
    printf("  * worker 数增加但 fps 不再涨 = 已到核数上限 (i5-4590 只有 4 核)\n");
    printf("  * 到 240fps 的最小组合就是该配的 DECODE_WORKERS\n");
    return 0;
}
