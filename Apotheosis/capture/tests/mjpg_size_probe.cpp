
// MJPG 帧体积探针 —— 采集卡在当前状态下实际吐出的 JPEG 有多大。
//
// 动机: 解码耗时随【压缩体积】线性增长 (实测 343KB->4.70ms, 924KB->6.40ms)。
// 若当前是无信号占位帧 (纯色), JPEG 会被压到极小, 解码几乎免费 —— 此时
// "消费 238fps" 完全不能证明解码能扛住真实画面。必须先量出真实体积。
//
// 用法: mjpg_size_probe [设备=0] [W=1920] [H=1080] [fps=240] [帧数=60]

#define WIN32_LEAN_AND_MEAN
#define _WINSOCKAPI_
#include <windows.h>

#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace
{

std::atomic<int> g_count{ 0 };
int g_want = 60;
std::vector<DWORD> g_sizes;
std::vector<uint64_t> g_hashes;
std::vector<LONGLONG> g_ts;
std::mutex g_mutex;

uint64_t Fnv1a(const uint8_t* d, size_t n)
{
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; ++i) { h ^= d[i]; h *= 1099511628211ull; }
    return h;
}

class Cb : public IMFSourceReaderCallback
{
public:
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override
    {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == __uuidof(IMFSourceReaderCallback))
        {
            *ppv = static_cast<IMFSourceReaderCallback*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return ++ref_; }
    STDMETHODIMP_(ULONG) Release() override
    {
        const ULONG n = --ref_;
        if (n == 0) delete this;
        return n;
    }

    void SetReader(IMFSourceReader* r) { reader_ = r; }

    STDMETHODIMP OnReadSample(HRESULT hr, DWORD, DWORD flags,
                              LONGLONG ts, IMFSample* sample) override
    {
        auto reArm = [&] {
            if (reader_ && g_count.load() < g_want)
                reader_->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0,
                                    nullptr, nullptr, nullptr, nullptr);
        };

        if (FAILED(hr) || (flags & MF_SOURCE_READERF_ENDOFSTREAM)) { reArm(); return S_OK; }
        if (!sample) { reArm(); return S_OK; }

        ComPtr<IMFMediaBuffer> buf;
        if (FAILED(sample->ConvertToContiguousBuffer(&buf)) || !buf) { reArm(); return S_OK; }

        BYTE* data = nullptr;
        DWORD maxLen = 0, curLen = 0;
        if (FAILED(buf->Lock(&data, &maxLen, &curLen)) || !data) { reArm(); return S_OK; }

        if (curLen > 0 && data)
        {
            const int idx = g_count.fetch_add(1);
            if (idx < g_want)
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                g_sizes.push_back(curLen);
                g_hashes.push_back(Fnv1a(data, curLen));
                g_ts.push_back(ts);
            }
        }
        buf->Unlock();
        reArm();
        return S_OK;
    }

    STDMETHODIMP OnFlush(DWORD) override { return S_OK; }
    STDMETHODIMP OnEvent(DWORD, IMFMediaEvent*) override { return S_OK; }

private:
    IMFSourceReader* reader_ = nullptr;
    std::atomic<ULONG> ref_{ 1 };
};

std::string WideToUtf8(const wchar_t* t)
{
    if (!t || !*t) return std::string();
    const int n = WideCharToMultiByte(CP_UTF8, 0, t, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return std::string();
    std::string o(static_cast<size_t>(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, t, -1, o.data(), n, nullptr, nullptr);
    return o;
}

} // namespace

int main(int argc, char** argv)
{
    SetConsoleOutputCP(CP_UTF8);
    setvbuf(stdout, nullptr, _IONBF, 0);

    const int dev = (argc > 1) ? std::atoi(argv[1]) : 0;
    const int W = (argc > 2) ? std::atoi(argv[2]) : 1920;
    const int H = (argc > 3) ? std::atoi(argv[3]) : 1080;
    const int fps = (argc > 4) ? std::atoi(argv[4]) : 240;
    g_want = (argc > 5) ? std::atoi(argv[5]) : 60;

    if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) return 1;
    if (FAILED(MFStartup(MF_VERSION))) return 1;

    ComPtr<IMFAttributes> attrs;
    MFCreateAttributes(&attrs, 1);
    attrs->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
                   MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);

    IMFActivate** devs = nullptr;
    UINT32 count = 0;
    if (FAILED(MFEnumDeviceSources(attrs.Get(), &devs, &count)) || count == 0) return 1;

    ComPtr<IMFMediaSource> src;
    if (FAILED(devs[dev]->ActivateObject(IID_PPV_ARGS(&src)))) return 1;

    auto* cb = new Cb();
    ComPtr<IMFAttributes> ra;
    MFCreateAttributes(&ra, 4);
    ra->SetUnknown(MF_SOURCE_READER_ASYNC_CALLBACK, cb);
    ra->SetUINT32(MF_READWRITE_DISABLE_CONVERTERS, TRUE);
    ra->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, FALSE);
    ra->SetUINT32(MF_LOW_LATENCY, TRUE);

    ComPtr<IMFSourceReader> reader;
    if (FAILED(MFCreateSourceReaderFromMediaSource(src.Get(), ra.Get(), &reader))) return 1;
    cb->SetReader(reader.Get());

    ComPtr<IMFMediaType> chosen;
    for (DWORD i = 0;; ++i)
    {
        ComPtr<IMFMediaType> t;
        const HRESULT hr = reader->GetNativeMediaType(
            static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), i, &t);
        if (hr == MF_E_NO_MORE_TYPES || FAILED(hr)) break;
        GUID sub{};
        if (FAILED(t->GetGUID(MF_MT_SUBTYPE, &sub)) || sub != MFVideoFormat_MJPG) continue;
        UINT32 w = 0, h = 0;
        MFGetAttributeSize(t.Get(), MF_MT_FRAME_SIZE, &w, &h);
        UINT32 num = 0, den = 1;
        MFGetAttributeRatio(t.Get(), MF_MT_FRAME_RATE, &num, &den);
        const int f = den ? static_cast<int>((num + den / 2) / den) : 0;
        if (static_cast<int>(w) == W && static_cast<int>(h) == H && f == fps) { chosen = t; break; }
    }
    if (!chosen) { printf("[FAIL] 卡未声明 MJPG %dx%d@%d\n", W, H, fps); return 1; }
    if (FAILED(reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, chosen.Get())))
    { printf("[FAIL] SetCurrentMediaType\n"); return 1; }

    printf("=== MJPG %dx%d@%d 帧体积探针 (取前 %d 帧) ===\n\n", W, H, fps, g_want);
    reader->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, nullptr, nullptr, nullptr, nullptr);

    const DWORD t0 = GetTickCount();
    while (g_count.load() < g_want && (GetTickCount() - t0) < 15000)
        Sleep(10);

    const DWORD elapsed = GetTickCount() - t0;
    const size_t n = g_sizes.size();
    if (n == 0) { printf("[FAIL] 没取到帧\n"); return 1; }

    printf("取到 %zu 帧, 耗时 %lu ms -> %.1f fps\n\n",
           n, static_cast<unsigned long>(elapsed),
           elapsed ? n * 1000.0 / elapsed : 0.0);

    // 体积统计
    std::vector<DWORD> sorted = g_sizes;
    std::sort(sorted.begin(), sorted.end());
    uint64_t sum = 0;
    for (DWORD s : g_sizes) sum += s;
    const double avg = static_cast<double>(sum) / n;

    printf("=== 帧体积 (字节) ===\n");
    printf("  最小 %lu   中位 %lu   最大 %lu   平均 %.0f (%.1f KB)\n\n",
           static_cast<unsigned long>(sorted.front()),
           static_cast<unsigned long>(sorted[n / 2]),
           static_cast<unsigned long>(sorted.back()),
           avg, avg / 1024.0);

    printf("=== 前 20 帧逐个体积 ===\n");
    for (size_t i = 0; i < n && i < 20; ++i)
        printf("  #%02zu  %8lu 字节 (%.1f KB)\n", i,
               static_cast<unsigned long>(g_sizes[i]), g_sizes[i] / 1024.0);

    // 内容去重
    std::map<uint64_t, int> hc;
    for (uint64_t h : g_hashes) ++hc[h];
    printf("\n=== 内容去重 ===\n");
    printf("  唯一内容 %zu / %zu 帧\n", hc.size(), n);

    // 时间戳
    printf("\n=== 设备时间戳间隔 ===\n");
    std::vector<double> gaps;
    for (size_t i = 1; i < g_ts.size(); ++i)
        if (g_ts[i] && g_ts[i - 1])
            gaps.push_back((g_ts[i] - g_ts[i - 1]) / 10000.0);
    if (gaps.empty()) printf("  (无时间戳)\n");
    else
    {
        std::vector<double> gs = gaps;
        std::sort(gs.begin(), gs.end());
        printf("  中位 %.3f ms  (240fps 期望 4.167, 120fps 期望 8.333)\n", gs[gs.size() / 2]);
    }

    printf("\n=== 判读 ===\n");
    if (avg < 150 * 1024.0)
        printf(">>> 帧体积只有 %.0f KB —— 这是【无信号/静止画面】的特征。\n"
               "    真实 1080p 画面 (quality~95) 约 900KB, 解码要 6.4ms/帧。\n"
               "    这种小帧解码几乎免费, \"消费 240fps\" 不能证明解码能扛真实画面。\n", avg / 1024.0);
    else if (avg < 500 * 1024.0)
        printf(">>> 帧体积 %.0f KB —— 中等压缩, 解码约 4.7ms/帧, 240fps 预算 4.17ms 偏紧。\n",
               avg / 1024.0);
    else
        printf(">>> 帧体积 %.0f KB —— 高质量大帧, 解码约 6.4ms/帧, 单 worker 只有 ~156fps。\n",
               avg / 1024.0);

    reader.Reset();
    src->Shutdown();
    for (UINT32 i = 0; i < count; ++i) devs[i]->Release();
    CoTaskMemFree(devs);
    MFShutdown();
    CoUninitialize();
    return 0;
}
