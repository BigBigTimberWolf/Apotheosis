
// 最简可信检查: 直接把每一帧的字节 dump 到磁盘, 用外部工具比较文件是否不同。
// 不依赖任何自制的哈希/锁/队列逻辑 —— 每帧同步写一个文件, 不共享状态。
// 用法: nv12_dump_frames <outdir> [设备序号=0] [W=1920] [H=1080] [fps=240] [帧数=30]

#define WIN32_LEAN_AND_MEAN
#define _WINSOCKAPI_
#include <windows.h>

#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace
{

std::atomic<int> g_saved{ 0 };
std::atomic<int> g_calls{ 0 };
int g_want = 30;
std::string g_dir;
int g_W = 1920, g_H = 1080;

// 每帧写独立文件。用 CreateFileW + WriteFile 直接落盘, 不做任何跨线程共享。
void DumpFrame(int idx, const BYTE* data, DWORD len)
{
    char name[512];
    std::snprintf(name, sizeof(name), "%s\\frame_%03d.raw", g_dir.c_str(), idx);
    HANDLE f = CreateFileA(name, GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE)
    {
        printf("[FAIL] 无法创建 %s (err=%lu)\n", name, GetLastError());
        return;
    }
    DWORD written = 0;
    WriteFile(f, data, len, &written, nullptr);
    CloseHandle(f);
    if (idx < 6 || idx == g_want - 1)
        printf("    写出 %s (%lu 字节)\n", name, static_cast<unsigned long>(written));
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
        ++g_calls;

        // 无论什么情况都要补发请求, 否则流只走一次就停。
        auto reArm = [&] {
            if (reader_ && g_saved.load() < g_want)
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
            const int idx = g_saved.fetch_add(1);
            if (idx < g_want)
            {
                printf("    帧 #%d: ts=%lld 长度=%lu 前16字节=", idx, ts,
                       static_cast<unsigned long>(curLen));
                for (int k = 0; k < 16 && k < static_cast<int>(curLen); ++k)
                    printf("%02X", data[k]);
                printf("\n");
                DumpFrame(idx, data, curLen);
            }
        }
        else
        {
            static std::atomic<int> warned{ 0 };
            if (warned.fetch_add(1) < 3)
                printf("    [warn] sample 非空但 Lock 报长度 0 (maxLen=%lu)\n",
                       static_cast<unsigned long>(maxLen));
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

    if (argc < 2) { printf("用法: nv12_dump_frames <outdir> [dev=0] [W=1920] [H=1080] [fps=240] [n=30]\n"); return 1; }
    g_dir = argv[1];
    const int dev = (argc > 2) ? std::atoi(argv[2]) : 0;
    g_W = (argc > 3) ? std::atoi(argv[3]) : 1920;
    g_H = (argc > 4) ? std::atoi(argv[4]) : 1080;
    const int fps = (argc > 5) ? std::atoi(argv[5]) : 240;
    g_want = (argc > 6) ? std::atoi(argv[6]) : 30;

    CreateDirectoryA(g_dir.c_str(), nullptr);

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
        if (FAILED(t->GetGUID(MF_MT_SUBTYPE, &sub)) || sub != MFVideoFormat_NV12) continue;
        UINT32 w = 0, h = 0;
        MFGetAttributeSize(t.Get(), MF_MT_FRAME_SIZE, &w, &h);
        UINT32 num = 0, den = 1;
        MFGetAttributeRatio(t.Get(), MF_MT_FRAME_RATE, &num, &den);
        const int f = den ? static_cast<int>((num + den / 2) / den) : 0;
        if (static_cast<int>(w) == g_W && static_cast<int>(h) == g_H && f == fps) { chosen = t; break; }
    }
    if (!chosen) { printf("[FAIL] 卡未声明 NV12 %dx%d@%d\n", g_W, g_H, fps); return 1; }
    if (FAILED(reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, chosen.Get())))
    { printf("[FAIL] SetCurrentMediaType\n"); return 1; }

    printf("=== dump %dx%d@%d 的前 %d 帧到 %s ===\n", g_W, g_H, fps, g_want, g_dir.c_str());
    reader->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, nullptr, nullptr, nullptr, nullptr);

    const DWORD t0 = GetTickCount();
    while (g_saved.load() < g_want && (GetTickCount() - t0) < 15000)
        Sleep(20);

    printf("\n回调次数=%d, 落盘帧数=%d, 耗时=%lu ms\n",
           g_calls.load(), g_saved.load(),
           static_cast<unsigned long>(GetTickCount() - t0));

    reader.Reset();
    src->Shutdown();
    for (UINT32 i = 0; i < count; ++i) devs[i]->Release();
    CoTaskMemFree(devs);
    MFShutdown();
    CoUninitialize();
    return 0;
}
