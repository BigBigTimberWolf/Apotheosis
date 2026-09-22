
// 采集卡 MJPG 能力枚举 —— 直接列出每张卡在每种格式下声明的分辨率/帧率,
// 用于判断"降 MJPG 画质换解码速度"是否有可选的模式。
// 用法: mjpg_modes_dump [设备序号, 默认 0]

#define WIN32_LEAN_AND_MEAN
#define _WINSOCKAPI_
#include <windows.h>

#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace
{

std::string WideToUtf8(const wchar_t* text)
{
    if (!text || !*text) return std::string();
    const int n = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return std::string();
    std::string out(static_cast<size_t>(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), n, nullptr, nullptr);
    return out;
}

const char* SubtypeName(REFGUID sub)
{
    if (sub == MFVideoFormat_NV12)   return "NV12";
    if (sub == MFVideoFormat_MJPG)   return "MJPG";
    if (sub == MFVideoFormat_YUY2)   return "YUY2";
    if (sub == MFVideoFormat_RGB32)  return "RGB32";
    if (sub == MFVideoFormat_ARGB32) return "ARGB32";
    if (sub == MFVideoFormat_H264)   return "H264";
    if (sub == MFVideoFormat_P010)   return "P010";
    if (sub == MFVideoFormat_I420)   return "I420";
    if (sub == MFVideoFormat_YV12)   return "YV12";
    if (sub == MFVideoFormat_UYVY)   return "UYVY";
    return "OTHER";
}

struct Mode
{
    int w = 0, h = 0;
    std::vector<int> fps;
};

using ModeMap = std::map<std::string, std::vector<Mode>>;

void AddMode(ModeMap& out, const std::string& fmt, int w, int h, int fps)
{
    auto& list = out[fmt];
    for (auto& m : list)
    {
        if (m.w == w && m.h == h)
        {
            if (std::find(m.fps.begin(), m.fps.end(), fps) == m.fps.end())
                m.fps.push_back(fps);
            return;
        }
    }
    Mode m;
    m.w = w; m.h = h;
    m.fps.push_back(fps);
    list.push_back(m);
}

} // namespace

int main(int argc, char** argv)
{
    SetConsoleOutputCP(CP_UTF8);
    setvbuf(stdout, nullptr, _IONBF, 0);

    const int wantIndex = (argc > 1) ? std::atoi(argv[1]) : 0;

    if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) return 1;
    if (FAILED(MFStartup(MF_VERSION))) return 1;

    ComPtr<IMFAttributes> attrs;
    MFCreateAttributes(&attrs, 1);
    attrs->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
                   MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);

    IMFActivate** devices = nullptr;
    UINT32 count = 0;
    if (FAILED(MFEnumDeviceSources(attrs.Get(), &devices, &count)))
    {
        printf("枚举采集设备失败\n");
        return 1;
    }

    printf("=== 本机视频采集设备: %u 台 ===\n", count);
    for (UINT32 i = 0; i < count; ++i)
    {
        WCHAR* name = nullptr;
        UINT32 nameLen = 0;
        devices[i]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &name, &nameLen);
        printf("  [%u] %s\n", i, WideToUtf8(name).c_str());
        if (name) CoTaskMemFree(name);
    }
    printf("\n");

    if (wantIndex < 0 || static_cast<UINT32>(wantIndex) >= count)
    {
        printf("设备序号 %d 超范围\n", wantIndex);
        for (UINT32 i = 0; i < count; ++i) devices[i]->Release();
        CoTaskMemFree(devices);
        MFShutdown();
        CoUninitialize();
        return 1;
    }

    ComPtr<IMFMediaSource> source;
    if (FAILED(devices[wantIndex]->ActivateObject(IID_PPV_ARGS(&source))))
    {
        printf("[FAIL] 无法激活设备 %d\n", wantIndex);
        for (UINT32 i = 0; i < count; ++i) devices[i]->Release();
        CoTaskMemFree(devices);
        MFShutdown();
        CoUninitialize();
        return 1;
    }

    WCHAR* devName = nullptr;
    UINT32 devNameLen = 0;
    devices[wantIndex]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME,
                                           &devName, &devNameLen);
    printf("=== 枚举 [%d] %s 的全部原生媒体类型 ===\n\n",
           wantIndex, WideToUtf8(devName).c_str());

    ComPtr<IMFSourceReader> reader;
    ComPtr<IMFAttributes> readerAttrs;
    MFCreateAttributes(&readerAttrs, 1);
    // 关掉转换器: 要的是卡【原生】声明的类型, 不是 MF 能凑出来的类型。
    readerAttrs->SetUINT32(MF_READWRITE_DISABLE_CONVERTERS, TRUE);
    if (FAILED(MFCreateSourceReaderFromMediaSource(source.Get(), readerAttrs.Get(), &reader)))
    {
        printf("[FAIL] 创建 source reader 失败\n");
        return 1;
    }

    ModeMap modes;
    for (DWORD i = 0;; ++i)
    {
        ComPtr<IMFMediaType> type;
        const HRESULT hr = reader->GetNativeMediaType(
            static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), i, &type);
        if (hr == MF_E_NO_MORE_TYPES) break;
        if (FAILED(hr)) break;

        GUID sub{};
        if (FAILED(type->GetGUID(MF_MT_SUBTYPE, &sub))) continue;

        UINT32 w = 0, h = 0;
        MFGetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, &w, &h);
        UINT32 num = 0, den = 1;
        MFGetAttributeRatio(type.Get(), MF_MT_FRAME_RATE, &num, &den);
        const int fps = (den != 0) ? static_cast<int>((num + den / 2) / den) : 0;
        if (w == 0 || h == 0) continue;

        AddMode(modes, SubtypeName(sub), static_cast<int>(w), static_cast<int>(h), fps);
    }

    for (auto& [fmt, list] : modes)
    {
        std::sort(list.begin(), list.end(), [](const Mode& a, const Mode& b) {
            return static_cast<long long>(a.w) * a.h > static_cast<long long>(b.w) * b.h;
        });
        printf("--- %s (%zu 种分辨率) ---\n", fmt.c_str(), list.size());
        for (auto& m : list)
        {
            std::sort(m.fps.begin(), m.fps.end());
            printf("    %4dx%-5d @ ", m.w, m.h);
            for (size_t k = 0; k < m.fps.size(); ++k)
                printf("%s%d", k ? "/" : "", m.fps[k]);
            printf("\n");
        }
        printf("\n");
    }

    reader.Reset();
    source->Shutdown();
    for (UINT32 i = 0; i < count; ++i) devices[i]->Release();
    CoTaskMemFree(devices);
    if (devName) CoTaskMemFree(devName);
    MFShutdown();
    CoUninitialize();
    return 0;
}
