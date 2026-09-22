
// NV12 1080p240 真伪探针 —— 判定"卡是否真的在 240fps 出【内容不同】的帧",
// 还是只在 120fps 出帧、被驱动/MF 复制成 240 个 sample。
//
// 判据 (三条互相独立, 必须同时成立才算真 240):
//   1. device timestamp 间隔分布: 真 240 -> 集中在 ~4.17ms; 复制 -> 大量 0 或双峰
//   2. 相邻帧内容差异: 逐像素 MAD。复制帧 -> MAD==0 计数接近一半
//   3. 唯一帧率: 对 sample 内容做哈希去重后的 unique fps
//
// 用法: nv12_truth_probe [设备序号=0] [宽=1920] [高=1080] [fps=240] [秒=6]

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
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace
{

struct SampleSlot
{
    std::mutex mutex;
    std::condition_variable cv;
    bool ready = false;
    // 每帧一个序号: 消费者据此判断"这是不是我上次取走的那一帧"。
    // 只靠 ready 标志会漏帧 —— 回调在消费者还没取走时覆盖 ready=true,
    // 消费者于是反复拷贝同一份内容, 表现得像"帧内容从不改变"。
    long long seq = 0;
    std::vector<uint8_t> bytes;
    LONGLONG deviceTs = 0;
    LONGLONG qpc100ns = 0;
    double arrivalMs = 0.0;
    long long delivered = 0;
};

double NowMs()
{
    using clock = std::chrono::steady_clock;
    static const clock::time_point start = clock::now();
    return std::chrono::duration<double, std::milli>(clock::now() - start).count();
}

// 简单的 64 位 FNV-1a, 用来对整帧内容去重 (只做等价性判断, 不需要抗碰撞)
uint64_t Fnv1a(const uint8_t* data, size_t n)
{
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; ++i)
    {
        h ^= data[i];
        h *= 1099511628211ull;
    }
    return h;
}

class Callback : public IMFSourceReaderCallback
{
public:
    explicit Callback(SampleSlot& slot) : slot_(slot) {}

    // 异步模式下, 每次 OnReadSample 返回前必须再发一次 ReadSample, 否则流只
    // 交付一个 sample 就停住 —— 消费者会不断看到同一块内存, 于是"所有帧都相同"。
    void SetReader(IMFSourceReader* reader) { reader_ = reader; }

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

    STDMETHODIMP OnReadSample(HRESULT hrStatus, DWORD, DWORD dwFlags,
                              LONGLONG ts, IMFSample* sample) override
    {
        const long long n = ++calls_;
        if (n <= 5 || (n % 100) == 0)
            printf("    [cb] #%lld hr=0x%08lX flags=0x%lX ts=%lld sample=%p\n",
                   n, static_cast<unsigned long>(hrStatus),
                   static_cast<unsigned long>(dwFlags), ts, static_cast<void*>(sample));

        // 流会给空 sample (stream tick / 设备尚未出图)。这种情况也必须补发请求,
        // 否则整条流就停在第一个回调上 —— 这正是"采集 0 个 sample"的原因。
        if (FAILED(hrStatus))
            return S_OK;
        if (dwFlags & MF_SOURCE_READERF_ENDOFSTREAM)
            return S_OK;

        if (!sample)
        {
            if (reader_ && !stop_)
                reader_->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0,
                                    nullptr, nullptr, nullptr, nullptr);
            return S_OK;
        }

        ComPtr<IMFMediaBuffer> buffer;
        if (FAILED(sample->ConvertToContiguousBuffer(&buffer)) || !buffer) return S_OK;

        BYTE* data = nullptr;
        DWORD maxLen = 0, currentLen = 0;
        if (FAILED(buffer->Lock(&data, &maxLen, &currentLen)) || !data) return S_OK;

        if (maxLen == 0 || currentLen == 0)
        {
            DWORD bufLen = 0;
            buffer->GetCurrentLength(&bufLen);
            if (n <= 5) printf("    [cb] Lock 返回 maxLen=%lu currentLen=%lu; GetCurrentLength=%lu\n",
                                static_cast<unsigned long>(maxLen),
                                static_cast<unsigned long>(currentLen),
                                static_cast<unsigned long>(bufLen));
            buffer->Unlock();
            if (reader_ && !stop_)
                reader_->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0,
                                    nullptr, nullptr, nullptr, nullptr);
            return S_OK;
        }

        LONGLONG qpc = 0;
        sample->GetSampleTime(&qpc);

        {
            std::lock_guard<std::mutex> lock(slot_.mutex);
            // 始终用最新的 sample 覆盖: 我们要量的是"卡吐帧的节奏", 不是消费者的
            // 消费速度。丢掉积压只会让测量偏向最新帧, 不影响重复帧判定。
            slot_.bytes.assign(data, data + currentLen);
            slot_.deviceTs = ts;
            slot_.qpc100ns = qpc;
            slot_.arrivalMs = NowMs();
            slot_.ready = true;
            ++slot_.seq;
            ++slot_.delivered;
        }
        slot_.cv.notify_one();

        buffer->Unlock();

        // 关键: 补发下一次请求, 让流持续交付。
        // 注意必须在解锁 slot_.mutex 之后调用 —— MF 可能在同一个回调线程上
        // 同步重入, 持锁请求会直接死锁。
        if (reader_ && !stop_)
            reader_->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0,
                                nullptr, nullptr, nullptr, nullptr);
        return S_OK;
    }

    void Stop() { stop_ = true; }

    STDMETHODIMP OnFlush(DWORD) override { return S_OK; }
    STDMETHODIMP OnEvent(DWORD, IMFMediaEvent*) override { return S_OK; }

private:
    SampleSlot& slot_;
    IMFSourceReader* reader_ = nullptr;
    std::atomic<bool> stop_{ false };
    std::atomic<long long> calls_{ 0 };
    std::atomic<ULONG> ref_{ 1 };
};

std::string WideToUtf8(const wchar_t* text)
{
    if (!text || !*text) return std::string();
    const int n = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return std::string();
    std::string out(static_cast<size_t>(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), n, nullptr, nullptr);
    return out;
}

} // namespace

int main(int argc, char** argv)
{
    SetConsoleOutputCP(CP_UTF8);
    setvbuf(stdout, nullptr, _IONBF, 0);

    const int devIndex = (argc > 1) ? std::atoi(argv[1]) : 0;
    const int W = (argc > 2) ? std::atoi(argv[2]) : 1920;
    const int H = (argc > 3) ? std::atoi(argv[3]) : 1080;
    const int FPS = (argc > 4) ? std::atoi(argv[4]) : 240;
    const int SECONDS = (argc > 5) ? std::atoi(argv[5]) : 6;

    if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) return 1;
    if (FAILED(MFStartup(MF_VERSION))) return 1;

    ComPtr<IMFAttributes> attrs;
    MFCreateAttributes(&attrs, 1);
    attrs->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
                   MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);

    IMFActivate** devices = nullptr;
    UINT32 count = 0;
    if (FAILED(MFEnumDeviceSources(attrs.Get(), &devices, &count)) || count == 0)
    {
        printf("枚举采集设备失败\n");
        return 1;
    }

    ComPtr<IMFMediaSource> source;
    if (FAILED(devices[devIndex]->ActivateObject(IID_PPV_ARGS(&source))))
    {
        printf("[FAIL] 激活设备失败\n");
        return 1;
    }

    WCHAR* devName = nullptr;
    UINT32 devNameLen = 0;
    devices[devIndex]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME,
                                          &devName, &devNameLen);
    printf("=== NV12 %dx%d@%d 真伪探针 ===\n设备: %s\n\n",
           W, H, FPS, WideToUtf8(devName).c_str());

    auto* slot = new SampleSlot();
    auto* cb = new Callback(*slot);

    ComPtr<IMFAttributes> readerAttrs;
    MFCreateAttributes(&readerAttrs, 4);
    readerAttrs->SetUnknown(MF_SOURCE_READER_ASYNC_CALLBACK, cb);
    readerAttrs->SetUINT32(MF_READWRITE_DISABLE_CONVERTERS, TRUE);
    readerAttrs->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, FALSE);
    readerAttrs->SetUINT32(MF_LOW_LATENCY, TRUE);

    ComPtr<IMFSourceReader> reader;
    if (FAILED(MFCreateSourceReaderFromMediaSource(source.Get(), readerAttrs.Get(), &reader)))
    {
        printf("[FAIL] 创建 reader 失败\n");
        return 1;
    }
    cb->SetReader(reader.Get());

    // 精确匹配 NV12 WxH@FPS 的原生类型
    ComPtr<IMFMediaType> chosen;
    for (DWORD i = 0;; ++i)
    {
        ComPtr<IMFMediaType> type;
        const HRESULT hr = reader->GetNativeMediaType(
            static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), i, &type);
        if (hr == MF_E_NO_MORE_TYPES || FAILED(hr)) break;

        GUID sub{};
        if (FAILED(type->GetGUID(MF_MT_SUBTYPE, &sub)) || sub != MFVideoFormat_NV12) continue;

        UINT32 w = 0, h = 0;
        MFGetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, &w, &h);
        UINT32 num = 0, den = 1;
        MFGetAttributeRatio(type.Get(), MF_MT_FRAME_RATE, &num, &den);
        const int fps = (den != 0) ? static_cast<int>((num + den / 2) / den) : 0;

        if (static_cast<int>(w) == W && static_cast<int>(h) == H && fps == FPS)
        {
            chosen = type;
            break;
        }
    }

    if (!chosen)
    {
        printf("[FAIL] 卡没有声明 NV12 %dx%d@%d\n", W, H, FPS);
        return 1;
    }

    if (FAILED(reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, chosen.Get())))
    {
        printf("[FAIL] SetCurrentMediaType 被拒绝\n");
        return 1;
    }

    ComPtr<IMFMediaType> actual;
    reader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, &actual);
    UINT32 an = 0, ad = 1;
    MFGetAttributeRatio(actual.Get(), MF_MT_FRAME_RATE, &an, &ad);
    printf("协商结果: NV12 %dx%d @ %.3f fps (num=%u den=%u)  期望帧字节=%zu (4:2:0)\n\n",
           W, H, ad ? static_cast<double>(an) / ad : 0.0, an, ad,
           static_cast<size_t>(W) * H * 3 / 2);

    // ── 采集 ────────────────────────────────────────────────────────────────
    // NV12 = 4:2:0: Y 平面 W*H + 交错 UV 平面 W*H/2 = W*H*1.5 字节。
    // 之前按 W*2 估算是错的 (那是 YUY2), 导致"期望字节数"虚高、采样只覆盖到
    // 帧的前 3/4, 判定失真。
    const size_t frameBytes = static_cast<size_t>(W) * H * 3 / 2;

    std::vector<LONGLONG> tsList;
    std::vector<double>   arrivalList;
    std::vector<uint64_t> hashList;
    std::vector<double>   madList;      // 与前一帧的逐像素平均绝对差
    std::vector<uint8_t>  prev;
    std::vector<uint8_t>  first;
    bool sawTypeChange = false;
    int  bytesSeen = 0;

    tsList.reserve(4096);
    arrivalList.reserve(4096);
    hashList.reserve(4096);
    madList.reserve(4096);

    const double tEnd = NowMs() + SECONDS * 1000.0;

    long long lastSeq = 0;

    reader->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, nullptr, nullptr, nullptr, nullptr);

    while (NowMs() < tEnd)
    {
        std::vector<uint8_t> bytes;
        LONGLONG ts = 0, qpc = 0;
        double arr = 0.0;
        {
            std::unique_lock<std::mutex> lock(slot->mutex);
            // 必须等"序号变化"而不是只等 ready: ready 会被回调反复置回 true,
            // 只等 ready 会让消费者在同一份内容上打转。
            if (!slot->cv.wait_for(lock, std::chrono::milliseconds(500),
                                   [&] { return slot->seq != lastSeq; }))
                continue;
            lastSeq = slot->seq;
            // 拷贝而不是 move: move 会把 slot->bytes 掏空, 之后回调写入的是新
            // 缓冲区, 而消费者拿到的 size 与实际内容不一致 —— 哈希会全部相同。
            bytes = slot->bytes;
            ts = slot->deviceTs;
            qpc = slot->qpc100ns;
            arr = slot->arrivalMs;
        }

        DWORD flags = 0, actualLen = 0;
        reader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, &actual);
        (void)flags;

        if (static_cast<int>(bytes.size()) != static_cast<int>(frameBytes))
            bytesSeen = static_cast<int>(bytes.size());

        tsList.push_back(ts);
        arrivalList.push_back(arr);
        hashList.push_back(Fnv1a(bytes.data(), bytes.size()));

        if (!prev.empty() && prev.size() == bytes.size())
        {
            double sum = 0.0;
            // 步长取质数 97: 与 1920 行宽互质, 保证采样点均匀铺满 Y+UV 整个帧,
            // 不会每帧都反复落在同一列上。
            constexpr int kStep = 97;
            size_t n = 0;
            for (size_t k = 0; k < bytes.size(); k += kStep)
            {
                sum += std::abs(static_cast<int>(bytes[k]) - static_cast<int>(prev[k]));
                ++n;
            }
            madList.push_back(n ? sum / static_cast<double>(n) : -1.0);
        }
        else
        {
            madList.push_back(-1.0);
        }

        if (first.empty()) first = bytes;
        prev = std::move(bytes);
    }

    cb->Stop();

    const size_t total = tsList.size();
    printf("采集 %zu 个 sample (%.1f 秒)\n", total, static_cast<double>(SECONDS));
    printf("sample 字节数: %d   期望 NV12 %dx%d (4:2:0) = %zu 字节\n\n",
           bytesSeen, W, H, frameBytes);

    // 时间戳是"驱动/卡真正标注的", 与内容无关。先看它, 因为它决定"卡到底在什么
    // 节奏吐帧"; 内容重复只说明"吐出来的帧是不是新的"。
    printf("[0] 原始序列前 40 个 sample 的 (设备时间戳ms, 到达ms, 内容指纹前8位)\n");
    {
        const LONGLONG base = tsList.empty() ? 0 : tsList.front();
        const uint64_t h0 = hashList.empty() ? 0 : hashList.front();
        const double a0 = arrivalList.empty() ? 0.0 : arrivalList.front();
        for (size_t i = 0; i < hashList.size() && i < 40; ++i)
        {
            const double tsMs = (tsList[i] - base) / 10000.0;
            const double arMs = arrivalList[i] - a0;
            printf("    #%02zu  ts=%9.3f  arr=%8.3f  hash=%016llx%s\n",
                   i, tsMs, arMs,
                   static_cast<unsigned long long>(hashList[i]),
                   (hashList[i] == h0 && i > 0) ? "   <-- 与首帧字节完全相同" : "");
        }
    }
    printf("\n");

    if (total < 10)
    {
        printf("[FAIL] sample 太少, 无法判定\n");
        return 1;
    }

    // ── 1. 到达率 ───────────────────────────────────────────────────────────
    const double wallMs = arrivalList.back() - arrivalList.front();
    const double arrivalFps = (total > 1 && wallMs > 0.0)
        ? (total - 1) * 1000.0 / wallMs : 0.0;
    printf("[1] 到达率 (sample 字节到达)\n");
    printf("    总 sample %zu, 实际墙钟 %.1f ms -> %.1f fps\n\n", total, wallMs, arrivalFps);

    // ── 2. 相邻帧内容差异 ───────────────────────────────────────────────────
    // 注意: NV12 是 4:2:0, 每帧 = W*H*1.5 字节, 不是 W*H*2。
    // 字节采样必须覆盖整帧 (含 UV 平面), 步长取质数避免与行宽对齐而反复采到同一列。
    size_t identical = 0, compared = 0;
    double madSum = 0.0;
    for (double m : madList)
    {
        if (m < 0.0) continue;
        ++compared;
        if (m == 0.0) ++identical;
        madSum += m;
    }
    const double identPct = compared ? 100.0 * identical / compared : 0.0;
    printf("[2] 相邻帧内容差异 (整帧稀疏采样, 步长 97)\n");
    printf("    比较 %zu 对, 完全相同的帧对: %zu (%.1f%%)\n", compared, identical, identPct);
    printf("    平均逐像素差 (MAD): %.3f  (真画面应 > 0)\n\n",
           compared ? madSum / compared : 0.0);

    // ── 3. 内容去重后的唯一帧率 ─────────────────────────────────────────────
    std::map<uint64_t, int> hashCount;
    for (uint64_t h : hashList) ++hashCount[h];
    size_t uniqueFrames = 0, dupFrames = 0;
    for (auto& [h, c] : hashCount)
    {
        ++uniqueFrames;
        dupFrames += (c - 1);
    }
    const double uniqueFps = wallMs > 0.0 ? uniqueFrames * 1000.0 / wallMs : 0.0;
    printf("[3] 内容去重 (FNV-1a 整帧哈希)\n");
    printf("    唯一帧 %zu, 重复帧 %zu  -> 唯一帧率 %.1f fps\n\n", uniqueFrames, dupFrames, uniqueFps);

    // ── 4. 设备时间戳间隔 ───────────────────────────────────────────────────
    printf("[4] 设备时间戳 (100ns 单位) 间隔分布\n");
    std::vector<double> gapsMs;
    size_t zeroGaps = 0;
    for (size_t i = 1; i < tsList.size(); ++i)
    {
        if (tsList[i] == 0 || tsList[i - 1] == 0) continue;
        const double d = (tsList[i] - tsList[i - 1]) / 10000.0;
        gapsMs.push_back(d);
        if (d == 0.0) ++zeroGaps;
    }
    if (gapsMs.empty())
    {
        printf("    (驱动未提供设备时间戳, 此项不可用)\n\n");
    }
    else
    {
        std::vector<double> sorted = gapsMs;
        std::sort(sorted.begin(), sorted.end());
        auto pct = [&](double p) { return sorted[static_cast<size_t>(p * (sorted.size() - 1))]; };
        printf("    样本 %zu, 间隔 0 的: %zu (%.1f%%)\n",
               gapsMs.size(), zeroGaps, 100.0 * zeroGaps / gapsMs.size());
        printf("    最小 %.3f ms  p25 %.3f  p50 %.3f  p75 %.3f  最大 %.3f ms\n",
               sorted.front(), pct(0.25), pct(0.50), pct(0.75), sorted.back());
        printf("    真 240fps 期望 ~4.167 ms; 真 120fps 期望 ~8.333 ms\n\n");
    }

    // ── 判读 ────────────────────────────────────────────────────────────────
    printf("=== 判读 ===\n");
    const bool contentReal = (identPct < 5.0) && (uniqueFps > 0.75 * FPS);
    const bool arrivalReal = arrivalFps > 0.75 * FPS;

    if (contentReal && arrivalReal)
        printf(">>> 真 %d fps: 帧内容逐帧变化, 没有复制帧, 到达率也到位。\n", FPS);
    else if (!contentReal && arrivalReal)
        printf(">>> 你的怀疑成立: sample 按 %d/s 到达, 但只有 ~%.0f 个【内容不同】的帧 ——\n"
               "    驱动/MF 在复制同一批帧。真实采集只有约 %.0f fps。\n",
               FPS, uniqueFps, uniqueFps);
    else
        printf(">>> 采集没跑起来: 到达率只有 %.1f fps, 唯一帧率 %.1f fps。\n"
               "    这个模式在本机实际达不到 %d fps。\n", arrivalFps, uniqueFps, FPS);

    (void)sawTypeChange;
    cb->Release();
    delete slot;
    reader.Reset();
    source->Shutdown();
    for (UINT32 i = 0; i < count; ++i) devices[i]->Release();
    CoTaskMemFree(devices);
    if (devName) CoTaskMemFree(devName);
    MFShutdown();
    CoUninitialize();
    return 0;
}
