// 自旋等待 vs 阻塞等待的 CPU 代价 (v2) —— 干净地量化推理线程等待 GPU 时烧掉多少 CPU。
//
// 背景: trt_detector.cpp 的 waitForEvent() 在 use_spin_wait_sync=true 时用
// cudaEventQuery()+_mm_pause() 死等事件完成, 最长 spin_wait_timeout_ms=50ms。
// 本机只有 4 个核, 若等待真的烧掉 ~1 个核, 就直接挤压解码吞吐。
//
// ★ v1 的三个缺陷, 这里全部修掉:
//   1. 用 36 个小 memset 凑时长 -> 把"入队开销"混进"等待开销"。
//      现在只排 1 个大 memset, 入队只花一次驱动调用 (~几十 µs)。
//   2. 没预热, 首次 memset 触发 2GB 缺页映射, 标定被污染。
//      现在先整块 memset 几遍把页都摸热, 再标定。
//   3. 标定循环里的 cudaDeviceSynchronize 落在计时区间内。
//      现在同步一律在计时区间之外。
//
// ★ v1 漏掉的关键手段: cudaEventBlockingSync (事件级标志)。
//   它让 cudaEventSynchronize 真正阻塞/让出 CPU, 且**每个事件独立设置**,
//   不需要在上下文创建前设 cudaDeviceScheduleBlockingSync。
//   这很可能才是应用该用的方式 —— 本基准把它作为第三种模式专门对比。
//
// 用法: spin_wait_bench [目标等待ms=5] [迭代次数=150]

#include <cuda_runtime.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <intrin.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>

namespace
{

using Clock = std::chrono::steady_clock;

double NowMs()
{
    static const Clock::time_point start = Clock::now();
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

// 进程累计 CPU 时间 (秒), 含全部线程的用户态+内核态。
double ProcessCpuSeconds()
{
    FILETIME c, e, k, u;
    if (!GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u)) return -1.0;
    auto toSec = [](const FILETIME& f) {
        ULARGE_INTEGER v;
        v.LowPart = f.dwLowDateTime;
        v.HighPart = f.dwHighDateTime;
        return static_cast<double>(v.QuadPart) * 1e-7;
    };
    return toSec(k) + toSec(u);
}

void* g_buf = nullptr;
const size_t g_cap = 2ull * 1024 * 1024 * 1024;
size_t g_memsetBytes = 512ull * 1024 * 1024;

// 只排 1 个 memset, 保证入队开销可忽略。
void enqueueWork(cudaStream_t s)
{
    cudaMemsetAsync(g_buf, 0x5A, g_memsetBytes, s);
}

struct Stat { double wallMs = 0, cpuMs = 0; int counts = 0; };

void report(const char* name, const Stat& s, double noteMs = 0.0)
{
    if (s.counts == 0) { printf("%-30s 无样本\n", name); return; }
    const double wall = s.wallMs / s.counts;
    const double cpu = s.cpuMs / s.counts;
    printf("%-30s %9.3f %11.3f %9.1f%%%s\n", name, wall, cpu,
           wall > 0 ? 100.0 * cpu / wall : 0.0,
           noteMs > 0 ? "" : "");
}

} // namespace

int main(int argc, char** argv)
{
    SetConsoleOutputCP(CP_UTF8);
    setvbuf(stdout, nullptr, _IONBF, 0);

    const double wantMs = (argc > 1) ? std::atof(argv[1]) : 5.0;
    const int iters = (argc > 2) ? std::atoi(argv[2]) : 150;

    if (cudaSetDevice(0) != cudaSuccess) { printf("[FAIL] cudaSetDevice\n"); return 1; }
    if (cudaMalloc(&g_buf, g_cap) != cudaSuccess) { printf("[FAIL] cudaMalloc 2GB\n"); return 1; }

    cudaStream_t stream = nullptr;
    cudaStreamCreate(&stream);

    // 三种等待方式各用自己的事件
    cudaEvent_t evSpin = nullptr, evSync = nullptr, evBlocking = nullptr;
    cudaEventCreateWithFlags(&evSpin,     cudaEventDisableTiming);
    cudaEventCreateWithFlags(&evSync,     cudaEventDisableTiming);
    cudaEventCreateWithFlags(&evBlocking, cudaEventDisableTiming | cudaEventBlockingSync);

    printf("=== 推理线程等待 GPU 的 CPU 代价 (v2) ===\n");
    printf("目标单次等待: ~%.1f ms   迭代: %d\n\n", wantMs, iters);

    // ── 预热: 把 2GB 的页全部摸热, 否则首次 memset 的缺页开销会污染标定 ──
    printf("[预热] 整块 memset 3 遍... ");
    for (int i = 0; i < 3; ++i)
    {
        cudaMemsetAsync(g_buf, 0, g_cap, stream);
        cudaStreamSynchronize(stream);
    }
    printf("完成\n");

    // ── 标定: 找一个字节数让"单次 memset"约等于 wantMs (同步都在计时区外) ──
    for (int trial = 0; trial < 40; ++trial)
    {
        cudaStreamSynchronize(stream);
        const double t0 = NowMs();
        enqueueWork(stream);
        cudaEventRecord(evSpin, stream);
        cudaEventSynchronize(evSpin);
        const double ms = NowMs() - t0;
        if (ms >= wantMs * 0.9) break;
        g_memsetBytes = g_memsetBytes * 3 / 2;
        if (g_memsetBytes > g_cap) { g_memsetBytes = g_cap; break; }
    }
    {
        cudaStreamSynchronize(stream);
        const double t0 = NowMs();
        enqueueWork(stream);
        cudaEventRecord(evSpin, stream);
        cudaEventSynchronize(evSpin);
        printf("[标定] 单次 memset %.0f MB -> GPU 时长约 %.2f ms\n\n",
               g_memsetBytes / (1024.0 * 1024.0), NowMs() - t0);
    }

    // ── 对照 0: Sleep —— 验证计时手段本身可靠 (CPU 应接近 0) ──────────────
    {
        Stat s{};
        for (int i = 0; i < 30; ++i)
        {
            const double c0 = ProcessCpuSeconds();
            const double t0 = NowMs();
            Sleep(static_cast<DWORD>(wantMs));
            s.wallMs += NowMs() - t0;
            s.cpuMs += (ProcessCpuSeconds() - c0) * 1000.0;
            ++s.counts;
        }
        report("对照0 Sleep(不碰GPU)", s);
    }

    // ── 对照 1: 纯入队 (不等待) —— 同步放在计时区之外 ────────────────────
    {
        Stat s{};
        for (int i = 0; i < 100; ++i)
        {
            const double c0 = ProcessCpuSeconds();
            const double t0 = NowMs();
            enqueueWork(stream);
            cudaEventRecord(evSpin, stream);
            s.wallMs += NowMs() - t0;
            s.cpuMs += (ProcessCpuSeconds() - c0) * 1000.0;
            ++s.counts;
            cudaStreamSynchronize(stream);   // 计时区外
        }
        report("对照1 纯入队(不等待)", s);
    }

    printf("\n%-30s %9s %11s %10s\n", "等待方式", "墙钟ms", "CPU ms", "CPU/墙钟");
    printf("---------------------------------------------------------------\n");

    Stat spin{}, sync{}, block{};
    const int warm = 5;

    // ── A. 自旋 (app 当前做法: cudaEventQuery + _mm_pause) ────────────────
    for (int i = 0; i < iters + warm; ++i)
    {
        enqueueWork(stream);
        cudaEventRecord(evSpin, stream);

        const double c0 = ProcessCpuSeconds();
        const double t0 = NowMs();
        const auto deadline = Clock::now() + std::chrono::milliseconds(50);
        for (;;)
        {
            const cudaError_t q = cudaEventQuery(evSpin);
            if (q == cudaSuccess) break;
            if (q != cudaErrorNotReady) break;
            if (Clock::now() >= deadline) break;
            _mm_pause();
        }
        const double ms = NowMs() - t0;
        const double cpu = (ProcessCpuSeconds() - c0) * 1000.0;
        if (i >= warm) { spin.wallMs += ms; spin.cpuMs += cpu; ++spin.counts; }
    }
    report("A. 自旋 (当前实现)", spin);

    // ── B. cudaEventSynchronize, 事件未加 BlockingSync ────────────────────
    for (int i = 0; i < iters + warm; ++i)
    {
        enqueueWork(stream);
        cudaEventRecord(evSync, stream);

        const double c0 = ProcessCpuSeconds();
        const double t0 = NowMs();
        cudaEventSynchronize(evSync);
        const double ms = NowMs() - t0;
        const double cpu = (ProcessCpuSeconds() - c0) * 1000.0;
        if (i >= warm) { sync.wallMs += ms; sync.cpuMs += cpu; ++sync.counts; }
    }
    report("B. EventSync (默认, 会自旋)", sync);

    // ── C. cudaEventSynchronize, 事件带 cudaEventBlockingSync ★关键 ───────
    for (int i = 0; i < iters + warm; ++i)
    {
        enqueueWork(stream);
        cudaEventRecord(evBlocking, stream);

        const double c0 = ProcessCpuSeconds();
        const double t0 = NowMs();
        cudaEventSynchronize(evBlocking);
        const double ms = NowMs() - t0;
        const double cpu = (ProcessCpuSeconds() - c0) * 1000.0;
        if (i >= warm) { block.wallMs += ms; block.cpuMs += cpu; ++block.counts; }
    }
    report("C. EventSync+BlockingSync ★", block);

    // ── 结论 ───────────────────────────────────────────────────────────────
    const double spinCpu = spin.counts ? spin.cpuMs / spin.counts : 0;
    const double blockCpu = block.counts ? block.cpuMs / block.counts : 0;
    const double spinWall = spin.counts ? spin.wallMs / spin.counts : 0;
    const double blockWall = block.counts ? block.wallMs / block.counts : 0;

    printf("\n=== 换算到 240fps (每秒 240 次等待) ===\n");
    printf("  A. 自旋            : %.2f 个核\n", spinCpu * 240.0 / 1000.0);
    printf("  C. BlockingSync    : %.2f 个核\n", blockCpu * 240.0 / 1000.0);
    printf("  可省回             : %.2f 个核\n", (spinCpu - blockCpu) * 240.0 / 1000.0);
    printf("  代价: 墙钟 %+.3f ms/次\n", blockWall - spinWall);

    printf("\n=== 判读 ===\n");
    const double saved = (spinCpu - blockCpu) * 240.0 / 1000.0;
    if (saved >= 0.4)
        printf(">>> 自旋确实在烧 CPU。改 cudaEventBlockingSync 可回收约 %.2f 个核,\n"
               "    代价只有墙钟 %.3f ms/次 —— 本机共 4 核, 解码已用约 2 核, 值得改。\n",
               saved, blockWall - spinWall);
    else if (saved <= -0.4)
        printf(">>> 反直觉: BlockingSync 反而更费 CPU (%.2f 个核)。保持现状。\n", -saved);
    else
        printf(">>> 两者差 %.2f 个核, 不显著。若 A 与 C 的 CPU 都接近墙钟,\n"
               "    说明本驱动在两种模式下都在自旋, 该设置不是有效杠杆。\n", saved);

    cudaEventDestroy(evSpin);
    cudaEventDestroy(evSync);
    cudaEventDestroy(evBlocking);
    cudaStreamDestroy(stream);
    cudaFree(g_buf);
    return 0;
}
