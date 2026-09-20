#include "mf_capture.h"
#include "capture_card_probe.h"
#include "runtime/interruptible_slot.h"
#include "raw_frame_layout.h"
#include "runtime/latency_probe.h"

#define WIN32_LEAN_AND_MEAN
#define _WINSOCKAPI_
#include <Windows.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>

#include "gpu_color_ops.h"

#include <algorithm>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <sstream>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace
{
std::string WideToUtf8(const wchar_t* text)
{
    if (!text || !*text)
        return std::string();
    const int required = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (required <= 1)
        return std::string();
    std::string result(static_cast<size_t>(required - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, result.data(), required, nullptr, nullptr);
    return result;
}

ComPtr<IMFAttributes> CreateVideoDeviceAttributes()
{
    ComPtr<IMFAttributes> attrs;
    if (FAILED(MFCreateAttributes(&attrs, 1)))
        return nullptr;
    attrs->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
                   MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
    return attrs;
}

const GUID& SubtypeFor(MFCapture::Format f)
{
    switch (f)
    {
    case MFCapture::Format::Nv12:  return MFVideoFormat_NV12;
    case MFCapture::Format::Mjpg:  return MFVideoFormat_MJPG;
    case MFCapture::Format::Yuy2:  return MFVideoFormat_YUY2;
    case MFCapture::Format::Rgb32:
    default:                       return MFVideoFormat_RGB32;
    }
}

bool GetSubType(IMFMediaType* type, GUID* subtype)
{
    return type && subtype && SUCCEEDED(type->GetGUID(MF_MT_SUBTYPE, subtype));
}

bool SelectExactMediaType(IMFSourceReader* reader,
                          GUID wantSubtype,
                          int wantW, int wantH, int wantFps,
                          double& outFps,
                          std::string& out_error)
{
    outFps = 0.0;
    out_error.clear();

    if (!reader)
    {
        out_error = "source reader unavailable";
        return false;
    }
    if (wantW <= 0 || wantH <= 0)
    {
        out_error = "no resolution selected (pick one from the device capability list)";
        return false;
    }
    if (wantFps <= 0)
    {
        out_error = "no frame rate selected (pick one from the device capability list)";
        return false;
    }

    const char* wantName = "OTHER";
    if (wantSubtype == MFVideoFormat_NV12)  wantName = "NV12";
    if (wantSubtype == MFVideoFormat_MJPG)  wantName = "MJPG";
    if (wantSubtype == MFVideoFormat_YUY2)  wantName = "YUY2";
    if (wantSubtype == MFVideoFormat_RGB32) wantName = "RGB32";

    struct TypeInfo
    {
        ComPtr<IMFMediaType> type;
        int    w = 0;
        int    h = 0;
        double fps = 0.0;
    };

    std::vector<TypeInfo>            sameFormat;
    std::vector<TypeInfo>            sameRes;
    std::vector<std::string>         otherFormats;
    std::vector<std::pair<int,int>>  allResolutions;
    constexpr size_t kNoExactMatch = static_cast<size_t>(-1);
    size_t exactIndex = kNoExactMatch;

    for (DWORD i = 0;; ++i)
    {
        ComPtr<IMFMediaType> t;
        const HRESULT hr = reader->GetNativeMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, i, &t);
        if (hr == MF_E_NO_MORE_TYPES)
            break;
        if (FAILED(hr) || !t)
            continue;

        GUID sub{};
        if (!GetSubType(t.Get(), &sub))
            continue;

        UINT32 w = 0, h = 0;
        if (FAILED(MFGetAttributeSize(t.Get(), MF_MT_FRAME_SIZE, &w, &h)) || w == 0 || h == 0 || w > INT_MAX / 4 || h > INT_MAX)
            continue;

        TypeInfo info;
        info.type = t;
        info.w = static_cast<int>(w);
        info.h = static_cast<int>(h);
        UINT32 num = 0, den = 0;
        if (SUCCEEDED(MFGetAttributeRatio(t.Get(), MF_MT_FRAME_RATE, &num, &den)) && den != 0 && num != 0)
            info.fps = static_cast<double>(num) / den;
        else if (SUCCEEDED(MFGetAttributeRatio(t.Get(), MF_MT_FRAME_RATE_RANGE_MAX, &num, &den)) && den != 0 && num != 0)
            info.fps = static_cast<double>(num) / den;

        if (sub != wantSubtype)
        {
            const char* other = "OTHER";
            if      (sub == MFVideoFormat_NV12)  other = "NV12";
            else if (sub == MFVideoFormat_MJPG)  other = "MJPG";
            else if (sub == MFVideoFormat_YUY2)  other = "YUY2";
            else if (sub == MFVideoFormat_RGB32) other = "RGB32";
            if (std::find(otherFormats.begin(), otherFormats.end(), std::string(other)) == otherFormats.end())
                otherFormats.push_back(other);
            continue;
        }

        const std::pair<int,int> wh{ info.w, info.h };
        if (std::find(allResolutions.begin(), allResolutions.end(), wh) == allResolutions.end())
            allResolutions.push_back(wh);

        const bool sameResolution = (info.w == wantW && info.h == wantH);

        if (sameResolution
            && std::abs(info.fps - static_cast<double>(wantFps)) <= 1.0
            && exactIndex == kNoExactMatch)
        {
            exactIndex = sameRes.size();
        }

        sameFormat.push_back(info);
        if (sameResolution)
            sameRes.push_back(std::move(info));
    }

    const TypeInfo* exact =
        (exactIndex == kNoExactMatch) ? nullptr : &sameRes[exactIndex];

    const auto joinRes = [](const std::vector<std::pair<int,int>>& v) {
        std::ostringstream os;
        for (size_t i = 0; i < v.size(); ++i)
        {
            if (i) os << ", ";
            os << v[i].first << "x" << v[i].second;
        }
        return os.str();
    };
    const auto joinNames = [](const std::vector<std::string>& v) {
        std::ostringstream os;
        for (size_t i = 0; i < v.size(); ++i) { if (i) os << ", "; os << v[i]; }
        return os.str();
    };

    if (sameFormat.empty())
    {
        std::ostringstream os;
        os << "device does not offer the requested pixel format " << wantName;
        if (!otherFormats.empty())
            os << "; it offers: " << joinNames(otherFormats);
        else
            os << "; it offers no usable pixel format at all";
        out_error = os.str();
        return false;
    }

    if (sameRes.empty())
    {
        std::ostringstream os;
        os << "device does not offer " << wantW << "x" << wantH << " in " << wantName
           << "; that format supports: " << joinRes(allResolutions);
        out_error = os.str();
        return false;
    }

    if (!exact)
    {
        std::ostringstream os;
        os << "device does not support " << wantName << " " << wantW << "x" << wantH
           << " @ " << wantFps << "fps; that combination supports fps:";
        std::vector<int> rates;
        for (const auto& t : sameRes)
        {
            const int r = static_cast<int>(std::lround(t.fps));
            if (std::find(rates.begin(), rates.end(), r) == rates.end())
                rates.push_back(r);
        }
        std::sort(rates.begin(), rates.end());
        for (int r : rates) os << " " << r;
        out_error = os.str();
        return false;
    }

    const HRESULT setHr = reader->SetCurrentMediaType(
        MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, exact->type.Get());
    if (FAILED(setHr))
    {
        char hrText[32];
        std::snprintf(hrText, sizeof(hrText), "0x%08lX", static_cast<unsigned long>(setHr));
        std::ostringstream os;
        os << "device refused to switch to " << wantName << " " << wantW << "x" << wantH
           << " @ " << wantFps << "fps (SetCurrentMediaType failed, hr=" << hrText << ")";
        if (setHr == static_cast<HRESULT>(0x80070005))
            os << u8" [E_ACCESSDENIED: 设备正被另一个进程独占]";
        else if (setHr == static_cast<HRESULT>(0xC00D3704))
            os << u8" [MF_E_VIDEO_RECORDING_DEVICE_INVALIDATED: 设备被拔出或重置]";
        else if (setHr == static_cast<HRESULT>(0xC00D3E85))
            os << u8" [MF_E_SHUTDOWN: 设备对象已被关闭]";
        else if (setHr == static_cast<HRESULT>(0xC00D36B4))
            os << u8" [MF_E_INVALIDMEDIATYPE: 驱动不接受该媒体类型]";
        out_error = os.str();
        return false;
    }

    outFps = exact->fps;
    return true;
}

bool QueryCurrentFrameGeometry(IMFSourceReader* reader, int& width, int& height, int& stride)
{
    width = height = stride = 0;
    if (!reader)
        return false;

    ComPtr<IMFMediaType> current;
    if (FAILED(reader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, &current)) || !current)
        return false;

    UINT32 w = 0, h = 0;
    if (FAILED(MFGetAttributeSize(current.Get(), MF_MT_FRAME_SIZE, &w, &h)) || w == 0 || h == 0 || w > INT_MAX / 4 || h > INT_MAX)
        return false;
    width = static_cast<int>(w);
    height = static_cast<int>(h);

    UINT32 declaredStride = 0;
    if (SUCCEEDED(current->GetUINT32(MF_MT_DEFAULT_STRIDE, &declaredStride)) && declaredStride > 0)
        stride = static_cast<int>(declaredStride);
    else
    {
        GUID subtype{};
        LONG defaultStride = 0;
        if (SUCCEEDED(current->GetGUID(MF_MT_SUBTYPE, &subtype)) &&
            SUCCEEDED(MFGetStrideForBitmapInfoHeader(subtype.Data1, width, &defaultStride)))
            stride = defaultStride;
        else
            stride = width * (subtype == MFVideoFormat_RGB32 ? 4
                            : subtype == MFVideoFormat_YUY2 ? 2 : 1);
    }
    return true;
}

double QpcNow100ns()
{
    LARGE_INTEGER counter{}, freq{};
    if (!QueryPerformanceCounter(&counter) || !QueryPerformanceFrequency(&freq)
        || freq.QuadPart <= 0)
        return -1.0;
    return static_cast<double>(counter.QuadPart) * 1.0e7 / static_cast<double>(freq.QuadPart);
}

double FileTimeNow100ns()
{
    FILETIME ft{};
#if defined(_WIN32_WINNT) && (_WIN32_WINNT >= 0x0602)
    GetSystemTimePreciseAsFileTime(&ft);
#else
    GetSystemTimeAsFileTime(&ft);
#endif
    ULARGE_INTEGER u{};
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    return static_cast<double>(u.QuadPart);
}
struct SampleResult
{
    HRESULT status = S_OK;
    DWORD flags = 0;
    ComPtr<IMFSample> sample;
    int64_t capture_ns = 0;
    uint64_t device_timestamp = 0;
    double qpc100ns = -1;
    double file100ns = -1;
};

class SampleCallback final : public IMFSourceReaderCallback
{
public:
    runtime::InterruptibleSlot<SampleResult> slot;
    STDMETHODIMP QueryInterface(REFIID iid, void** output) override
    {
        if (!output) return E_POINTER;
        *output = nullptr;
        if (iid == __uuidof(IUnknown) || iid == __uuidof(IMFSourceReaderCallback))
        {
            *output = static_cast<IMFSourceReaderCallback*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return ++refs_; }
    STDMETHODIMP_(ULONG) Release() override
    {
        const ULONG left = --refs_;
        if (!left) delete this;
        return left;
    }
    STDMETHODIMP OnReadSample(HRESULT status, DWORD, DWORD flags,
                             LONGLONG, IMFSample* sample) override
    {
        SampleResult result;
        result.capture_ns = runtime::latency::nowNs();
        result.qpc100ns = QpcNow100ns();
        result.file100ns = FileTimeNow100ns();
        result.status = status;
        result.flags = flags;
        result.sample = sample;
        if (sample)
            sample->GetUINT64(MFSampleExtension_DeviceTimestamp, &result.device_timestamp);
        slot.publish(std::move(result));
        return S_OK;
    }
    STDMETHODIMP OnFlush(DWORD) override { slot.close(); return S_OK; }
    STDMETHODIMP OnEvent(DWORD, IMFMediaEvent*) override { return S_OK; }
private:
    std::atomic<ULONG> refs_{1};
};
}

MFCapture::Format MFCapture::ParseFormat(const std::string& s)
{
    if (s == "MJPG")  return Format::Mjpg;
    if (s == "YUY2" || s == "YUYV") return Format::Yuy2;
    if (s == "RGB32" || s == "RGBA" || s == "BGRA") return Format::Rgb32;
    return Format::Nv12;
}

const char* MFCapture::FormatLabel(Format f)
{
    switch (f)
    {
    case Format::Mjpg:  return "MJPG";
    case Format::Yuy2:  return "YUY2";
    case Format::Rgb32: return "RGB32";
    case Format::Nv12:
    default:            return "NV12";
    }
}

MFCapture::MFCapture(int src_width,
                     int src_height,
                     int out_side,
                     bool crop_enabled,
                     int capture_fps,
                     const std::string& format,
                     int device_index,
                     bool gpu_decode)
    : src_width_(std::max(0, src_width))
    , src_height_(std::max(0, src_height))
    , out_side_(std::max(1, out_side))
    , crop_enabled_(crop_enabled)
    , capture_fps_(std::max(0, capture_fps))
    , format_(ParseFormat(format))
    , device_index_(std::max(0, device_index))
    , gpu_decode_(gpu_decode)
{
    source_fps_start_ = std::chrono::steady_clock::now();
    target_fps_.store(capture_fps_);
    receive_thread_ = std::thread(&MFCapture::ReceiveThread, this);
}

MFCapture::~MFCapture()
{
    should_stop_.store(true);
    if (receive_thread_.joinable())
        receive_thread_.join();

    if (gpu_stream_)
        cudaStreamSynchronize(gpu_stream_);

    if (pinned_jpeg_buffer_)
    {
        cudaFreeHost(pinned_jpeg_buffer_);
        pinned_jpeg_buffer_ = nullptr;
        pinned_jpeg_capacity_ = 0;
    }

    if (gpu_stream_)
    {
        cudaStreamDestroy(gpu_stream_);
        gpu_stream_ = nullptr;
    }
}

std::vector<MFDeviceInfo> MFCapture::EnumerateDevices()
{
    std::vector<MFDeviceInfo> devices;
    const HRESULT coInit = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool shouldUninit = SUCCEEDED(coInit);
    if (SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_LITE)))
    {
        ComPtr<IMFAttributes> attrs = CreateVideoDeviceAttributes();
        IMFActivate** activates = nullptr;
        UINT32 count = 0;
        if (attrs && SUCCEEDED(MFEnumDeviceSources(attrs.Get(), &activates, &count)))
        {
            for (UINT32 i = 0; i < count; ++i)
            {
                wchar_t* name = nullptr;
                UINT32 nameLen = 0;
                std::string label;
                if (SUCCEEDED(activates[i]->GetAllocatedString(
                        MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &name, &nameLen)))
                {
                    label = WideToUtf8(name);
                    CoTaskMemFree(name);
                }

                MFDeviceInfo info;
                info.index = static_cast<int>(i);
                info.friendly_name = label;
                std::ostringstream display;
                display << u8"设备 #" << i;
                if (!label.empty())
                    display << " - " << label;
                info.name = display.str();
                devices.push_back(std::move(info));
                activates[i]->Release();
            }
            CoTaskMemFree(activates);
        }
        MFShutdown();
    }
    if (shouldUninit)
        CoUninitialize();
    return devices;
}

namespace
{

struct KnownFormat
{
    const GUID* guid;
    const char* name;
    bool        supported;
};

const KnownFormat kKnownFormats[] = {
    { &MFVideoFormat_NV12,   "NV12",  true  },
    { &MFVideoFormat_MJPG,   "MJPG",  true  },
    { &MFVideoFormat_YUY2,   "YUY2",  true  },
    { &MFVideoFormat_RGB32,  "RGB32", true  },
    { &MFVideoFormat_ARGB32, "RGB32", true  },
    { &MFVideoFormat_H264,   "H264",  false },
    { &MFVideoFormat_H264_ES,"H264",  false },
    { &MFVideoFormat_P010,   "P010",  false },
    { &MFVideoFormat_P016,   "P016",  false },
    { &MFVideoFormat_I420,   "I420",  false },
    { &MFVideoFormat_IYUV,   "IYUV",  false },
    { &MFVideoFormat_YV12,   "YV12",  false },
    { &MFVideoFormat_UYVY,   "UYVY",  false },
    { &MFVideoFormat_M4S2,   "M4S2",  false },
    { &MFVideoFormat_NV11,   "NV11",  false },
    { &MFVideoFormat_MP43,   "MP43",  false },
    { &MFVideoFormat_MP4S,   "MP4S",  false },
};

std::string FormatNameFromSubtype(REFGUID subtype, bool& supported)
{
    for (const auto& k : kKnownFormats)
    {
        if (IsEqualGUID(subtype, *k.guid))
        {
            supported = k.supported;
            return k.name;
        }
    }
    supported = false;
    wchar_t buf[64]{};
    StringFromGUID2(subtype, buf, 64);
    return WideToUtf8(buf);
}

int RoundFps(UINT32 num, UINT32 den)
{
    if (den == 0) return 0;
    return static_cast<int>(std::lround(static_cast<double>(num) / den));
}

}

bool MFCapture::ProbeCapabilities(MFDeviceInfo& dev)
{
    dev.caps.clear();
    dev.caps_probed = false;

    const HRESULT coInit = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool shouldUninit = SUCCEEDED(coInit);

    bool ok = false;
    if (SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_LITE)))
    {
        ComPtr<IMFAttributes> attrs = CreateVideoDeviceAttributes();
        IMFActivate** activates = nullptr;
        UINT32 count = 0;
        if (attrs && SUCCEEDED(MFEnumDeviceSources(attrs.Get(), &activates, &count)))
        {
            if (dev.index >= 0 && static_cast<UINT32>(dev.index) < count)
            {
                IMFActivate* activate = activates[dev.index];

                ComPtr<IMFMediaSource> source;
                if (SUCCEEDED(activate->ActivateObject(
                        __uuidof(IMFMediaSource),
                        reinterpret_cast<void**>(source.GetAddressOf()))))
                {
                    ComPtr<IMFSourceReader> reader;
                    if (SUCCEEDED(MFCreateSourceReaderFromMediaSource(
                            source.Get(), nullptr, &reader)))
                    {
                        for (DWORD i = 0; ; ++i)
                        {
                            ComPtr<IMFMediaType> mt;
                            const HRESULT hr = reader->GetNativeMediaType(
                                static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM),
                                i, mt.GetAddressOf());
                            if (hr == MF_E_NO_MORE_TYPES || FAILED(hr))
                                break;

                            GUID sub{};
                            if (FAILED(mt->GetGUID(MF_MT_SUBTYPE, &sub)))
                                continue;

                            UINT32 w = 0, h = 0;
                            if (FAILED(MFGetAttributeSize(mt.Get(), MF_MT_FRAME_SIZE, &w, &h)))
                                continue;
                            if (w == 0 || h == 0)
                                continue;

                            UINT32 num = 0, den = 0;
                            if (FAILED(MFGetAttributeRatio(mt.Get(), MF_MT_FRAME_RATE, &num, &den)))
                            {
                                if (FAILED(MFGetAttributeRatio(
                                        mt.Get(), MF_MT_FRAME_RATE_RANGE_MAX, &num, &den)))
                                    continue;
                            }
                            const int fps = RoundFps(num, den);
                            if (fps <= 0) continue;

                            bool supported = false;
                            const std::string fname = FormatNameFromSubtype(sub, supported);

                            MFCapability* slot = nullptr;
                            for (auto& c : dev.caps)
                            {
                                if (c.format == fname && c.width == static_cast<int>(w)
                                    && c.height == static_cast<int>(h))
                                {
                                    slot = &c;
                                    break;
                                }
                            }
                            if (!slot)
                            {
                                MFCapability c;
                                c.format    = fname;
                                c.width     = static_cast<int>(w);
                                c.height    = static_cast<int>(h);
                                c.supported = supported;
                                dev.caps.push_back(std::move(c));
                                slot = &dev.caps.back();
                            }
                            if (std::find(slot->fps.begin(), slot->fps.end(), fps)
                                == slot->fps.end())
                                slot->fps.push_back(fps);
                        }

                        for (auto& c : dev.caps)
                            std::sort(c.fps.begin(), c.fps.end());

                        ok = !dev.caps.empty();
                    }
                    activate->ShutdownObject();
                }
                for (UINT32 i = 0; i < count; ++i)
                    activates[i]->Release();
                CoTaskMemFree(activates);
            }
            else
            {
                for (UINT32 i = 0; i < count; ++i)
                    activates[i]->Release();
                CoTaskMemFree(activates);
            }
        }
        MFShutdown();
    }
    if (shouldUninit)
        CoUninitialize();

    dev.caps_probed = ok;
    return ok;
}

namespace capture_card
{

std::vector<MFDeviceInfo> ProbeAll()
{
    return MFCapture::EnumerateDevicesWithCaps( -1);
}

std::vector<MFDeviceInfo> ProbeOne(int device_index)
{
    return MFCapture::EnumerateDevicesWithCaps(device_index);
}

const MFDeviceInfo* FindByName(const std::vector<MFDeviceInfo>& devs,
                               const std::string& friendly_name)
{
    if (friendly_name.empty())
        return nullptr;
    for (const auto& d : devs)
        if (d.friendly_name == friendly_name)
            return &d;
    return nullptr;
}

}

std::vector<MFDeviceInfo> MFCapture::EnumerateDevicesWithCaps(int probe_index)
{
    std::vector<MFDeviceInfo> devices = EnumerateDevices();
    for (auto& d : devices)
    {
        if (probe_index >= 0 && d.index != probe_index)
            continue;
        ProbeCapabilities(d);
    }
    return devices;
}

cv::Mat MFCapture::GetNextFrameCpu()
{
    std::lock_guard<std::mutex> lock(frame_mutex_);
    if (cpu_frame_queue_.empty())
        return cv::Mat();
    last_dequeued_capture_ns_ = cpu_frame_queue_.front().capture_ns;
    cv::Mat frame = std::move(cpu_frame_queue_.front().image);
    cpu_frame_queue_.pop();
    return frame;
}

GpuImage MFCapture::GetNextFrameGpu()
{
    std::lock_guard<std::mutex> lock(frame_mutex_);
    if (gpu_frame_queue_.empty())
        return GpuImage();
    last_dequeued_capture_ns_ = gpu_frame_queue_.front().capture_ns;
    GpuImage frame = std::move(gpu_frame_queue_.front().image);
    gpu_frame_queue_.pop();
    return frame;
}

bool MFCapture::EnsureGpuContext()
{
    if (gpu_stream_)
        return true;
    if (cudaStreamCreateWithFlags(&gpu_stream_, cudaStreamNonBlocking) != cudaSuccess)
    {
        std::cerr << "[MFCapture] CUDA stream creation failed." << std::endl;
        return false;
    }

    int dev = 0;
    cudaGetDevice(&dev);
    cudaDeviceProp props{};
    cudaGetDeviceProperties(&props, dev);
    npp_ctx_.hStream                            = gpu_stream_;
    npp_ctx_.nCudaDeviceId                      = dev;
    npp_ctx_.nMultiProcessorCount               = props.multiProcessorCount;
    npp_ctx_.nMaxThreadsPerMultiProcessor       = props.maxThreadsPerMultiProcessor;
    npp_ctx_.nMaxThreadsPerBlock                = props.maxThreadsPerBlock;
    npp_ctx_.nSharedMemPerBlock                 = props.sharedMemPerBlock;
    npp_ctx_.nCudaDevAttrComputeCapabilityMajor = props.major;
    npp_ctx_.nCudaDevAttrComputeCapabilityMinor = props.minor;
    cudaStreamGetFlags(gpu_stream_, &npp_ctx_.nStreamFlags);

    return true;
}

void MFCapture::ReceiveThread()
{
    const HRESULT coInit = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool shouldUninit = SUCCEEDED(coInit);
    if (FAILED(MFStartup(MF_VERSION, MFSTARTUP_LITE)))
    {
        std::cerr << "[MFCapture] MFStartup failed." << std::endl;
        if (shouldUninit)
            CoUninitialize();
        receive_finished_.store(true);
        return;
    }

    ComPtr<IMFMediaSource> source;
    ComPtr<IMFSourceReader> reader;
    ComPtr<SampleCallback> callback;
    callback.Attach(new SampleCallback());
    IMFActivate** activates = nullptr;
    UINT32 count = 0;
    double negotiatedFps = 0.0;

    ComPtr<IMFAttributes> deviceAttrs = CreateVideoDeviceAttributes();
    if (!deviceAttrs || FAILED(MFEnumDeviceSources(deviceAttrs.Get(), &activates, &count)) || count == 0)
    {
        std::cerr << "[MFCapture] No video capture devices found." << std::endl;
        goto cleanup;
    }

    if (device_index_ >= static_cast<int>(count))
    {
        std::cerr << "[MFCapture] Device index " << device_index_
                  << " out of range (only " << count << " device(s) present). "
                     "No substitution is performed." << std::endl;
        open_error_ = "selected capture device is no longer present";
        goto cleanup;
    }

    if (FAILED(activates[device_index_]->ActivateObject(IID_PPV_ARGS(&source))))
    {
        std::cerr << "[MFCapture] Failed to activate capture device." << std::endl;
        goto cleanup;
    }

    {
        bool sourceAccepts = false;
        ComPtr<IMFMediaSourceEx> sourceEx;
        if (SUCCEEDED(source->QueryInterface(IID_PPV_ARGS(&sourceEx))) && sourceEx)
        {
            ComPtr<IMFAttributes> sourceAttrs;
            if (SUCCEEDED(sourceEx->GetSourceAttributes(&sourceAttrs)) && sourceAttrs)
            {
                sourceAttrs->SetUINT32(MF_LOW_LATENCY, TRUE);
                UINT32 flag = 0;
                sourceAccepts = SUCCEEDED(sourceAttrs->GetUINT32(MF_LOW_LATENCY, &flag)) && flag != 0;
            }
        }
        std::cout << "[MFCapture] MF_LOW_LATENCY: media source attributes "
                  << (sourceAccepts ? "set" : "not accessible on this source")
                  << "; source reader: set" << std::endl;
    }

    {
        reader.Reset();
        ComPtr<IMFAttributes> readerAttrs;
        MFCreateAttributes(&readerAttrs, 4);
        if (!readerAttrs || FAILED(readerAttrs->SetUnknown(
                MF_SOURCE_READER_ASYNC_CALLBACK, callback.Get())))
        {
            open_error_ = "failed to configure asynchronous Media Foundation reader";
            goto cleanup;
        }
        if (readerAttrs)
        {
            readerAttrs->SetUINT32(MF_READWRITE_DISABLE_CONVERTERS, TRUE);
            readerAttrs->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, FALSE);
            readerAttrs->SetUINT32(MF_LOW_LATENCY, TRUE);
        }
        if (FAILED(MFCreateSourceReaderFromMediaSource(source.Get(), readerAttrs.Get(), &reader)))
        {
            std::cerr << "[MFCapture] Failed to create source reader." << std::endl;
            open_error_ = "failed to create Media Foundation source reader";
            goto cleanup;
        }

        double negotiatedOut = 0.0;
        std::string why;
        const bool ok = SelectExactMediaType(reader.Get(), SubtypeFor(format_),
                                             src_width_, src_height_, capture_fps_,
                                             negotiatedOut, why);
        if (!ok)
        {
            std::cerr << "[MFCapture] " << why << std::endl;
            open_error_ = why;
            goto cleanup;
        }
        negotiatedFps = negotiatedOut;

        if (!QueryCurrentFrameGeometry(reader.Get(), frame_width_, frame_height_, frame_stride_))
        {
            std::cerr << "[MFCapture] Device accepted the mode but reported no frame geometry." << std::endl;
            open_error_ = "device accepted the mode but reported no frame geometry";
            goto cleanup;
        }
    }

    if (gpu_decode_)
    {
        if (!EnsureGpuContext())
            goto cleanup;
        for (auto& slot : out_pool_)
            if (!slot.create(out_side_, out_side_, 3))
                goto cleanup;
        if (format_ != Format::Mjpg)
        {
            int left = 0, top = 0, roiW = 0, roiH = 0;
            ResolveRoi(frame_width_, frame_height_, left, top, roiW, roiH);
            const int sourceChannels = format_ == Format::Rgb32 ? 4
                                     : format_ == Format::Yuy2 ? 2 : 1;
            if (!scratch_a_.create(roiH, roiW, sourceChannels)
                || (format_ == Format::Nv12
                    && !scratch_b_.create(roiH / 2, roiW, 1))
                || !scratch_full_.create(roiH, roiW, 3))
                goto cleanup;
        }
        if (format_ == Format::Mjpg)
        {
            gpu_decoder_ = std::make_unique<capture::GpuJpegDecoder>();
            if (!gpu_decoder_->init())
            {
                std::cerr << "[MFCapture] nvJPEG is unavailable but MJPG was selected. "
                             "Refusing to fall back to CPU decode (that would silently cost "
                             "several ms per frame). Pick an uncompressed format instead."
                          << std::endl;
                open_error_ = "MJPG selected but nvJPEG GPU decoder is unavailable";
                goto cleanup;
            }
        }
    }

    std::cout << "[MFCapture] " << FormatLabel(format_)
              << " @ " << frame_width_ << "x" << frame_height_
              << "@" << negotiatedFps << "fps"
              << " stride=" << frame_stride_
              << " -> " << out_side_ << "x" << out_side_
              << (crop_enabled_ ? " (center-crop)" : " (scaled)")
              << " [" << (gpu_decode_ ? "GPU" : "CPU") << "]" << std::endl;
    negotiated_fps_.store(static_cast<int>(std::lround(negotiatedFps)));

    source_frame_count_ = 0;
    source_fps_smoothed_ = 0.0;
    source_fps_start_ = std::chrono::steady_clock::now();
    is_open_.store(true);
    StartDecodeWorkers();
    StartProcessWorker();
    while (!should_stop_.load())
    {
        const HRESULT requested = reader->ReadSample(
            MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, nullptr, nullptr, nullptr, nullptr);
        if (FAILED(requested))
        {
            open_error_ = "Media Foundation sample request failed";
            break;
        }
        SampleResult result;
        if (!callback->slot.wait(result, should_stop_)) break;
        if (FAILED(result.status) || (result.flags & MF_SOURCE_READERF_ERROR))
        {
            open_error_ = "Media Foundation sample delivery failed";
            break;
        }
        if (result.flags & MF_SOURCE_READERF_ENDOFSTREAM) break;
        if (result.flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED)
        {
            ComPtr<IMFMediaType> changedType;
            GUID subtype{};
            UINT32 width = 0, height = 0;
            if (FAILED(reader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, &changedType))
                || FAILED(changedType->GetGUID(MF_MT_SUBTYPE, &subtype))
                || FAILED(MFGetAttributeSize(changedType.Get(), MF_MT_FRAME_SIZE, &width, &height))
                || subtype != SubtypeFor(format_) || width != src_width_ || height != src_height_
                || !QueryCurrentFrameGeometry(reader.Get(), frame_width_, frame_height_, frame_stride_))
            {
                open_error_ = "capture media type changed unexpectedly; reopening selected mode";
                break;
            }
        }
        auto sample = std::move(result.sample);
        if (!sample) continue;
        device_age_.update(result.device_timestamp, result.qpc100ns,
                           result.file100ns, result.capture_ns);
        if (!result.device_timestamp && !device_age_missing_logged_)
        {
            device_age_missing_logged_ = true;
            std::cout << "[MFCapture] no device timestamp; driver frame age unavailable."
                      << std::endl;
        }

        ComPtr<IMFMediaBuffer> buffer;
        if (FAILED(sample->ConvertToContiguousBuffer(&buffer)) || !buffer)
            continue;

        BYTE* data = nullptr;
        DWORD maxLen = 0, currentLen = 0;
        if (FAILED(buffer->Lock(&data, &maxLen, &currentLen)))
            continue;

        if (data && currentLen > 0)
        {
            TickFps();
            if (ShouldDispatchFrame())
            {
                if (format_ == Format::Mjpg && gpu_decode_
                    && !mjpg_cpu_fallback_ && crop_enabled_)
                    EnqueueJpegJob(data, static_cast<size_t>(currentLen), result.capture_ns);
                else
                    EnqueueProcessJob(data, static_cast<size_t>(currentLen), result.capture_ns);
            }
        }

        buffer->Unlock();
    }

cleanup:
    if (!open_error_.empty()) std::cerr << "[MFCapture] " << open_error_ << std::endl;
    callback->slot.close();
    if (reader) reader->Flush(MF_SOURCE_READER_ALL_STREAMS);
    StopProcessWorker();
    StopDecodeWorkers();
    is_open_.store(false);
    if (activates)
    {
        for (UINT32 i = 0; i < count; ++i)
            activates[i]->Release();
        CoTaskMemFree(activates);
    }
    if (source)
        source->Shutdown();
    reader.Reset();
    source.Reset();
    device_age_.invalidate();
    receive_finished_.store(true);
    MFShutdown();
    if (shouldUninit)
        CoUninitialize();
}

void MFCapture::StartProcessWorker()
{
    process_stop_.store(false);
    process_thread_ = std::thread(&MFCapture::ProcessWorkerLoop, this);
}

void MFCapture::StopProcessWorker()
{
    process_stop_.store(true);
    process_cv_.notify_all();
    if (process_thread_.joinable())
        process_thread_.join();
    std::lock_guard<std::mutex> lock(process_mutex_);
    std::queue<ProcessJob> empty;
    process_queue_.swap(empty);
    process_free_buffers_.clear();
}

void MFCapture::EnqueueProcessJob(const uint8_t* data, size_t size, int64_t capture_ns)
{
    if (!data || size == 0)
        return;
    const int channels = format_ == Format::Rgb32 ? 4 : format_ == Format::Yuy2 ? 2 : 1;
    if (format_ != Format::Mjpg && !capture::rawFrameFits(size, frame_width_, frame_height_,
            frame_stride_, channels, format_ == Format::Nv12))
    {
        std::cerr << "[MFCapture] Dropped truncated or invalid raw sample." << std::endl;
        return;
    }
    ProcessJob job;
    job.capture_ns = capture_ns;
    {
        std::lock_guard<std::mutex> lock(process_mutex_);
        if (!process_free_buffers_.empty())
        {
            job.bytes = std::move(process_free_buffers_.back());
            process_free_buffers_.pop_back();
        }
    }

    job.width = frame_width_;
    job.height = frame_height_;
    job.stride = frame_stride_;
    bool copied = false;
    if (format_ != Format::Mjpg && frame_width_ > 0 && frame_height_ > 0)
    {
        int left = 0, top = 0, roiW = 0, roiH = 0;
        ResolveRoi(frame_width_, frame_height_, left, top, roiW, roiH);
        const bool bottomUp = frame_stride_ < 0;
        const int srcStride = bottomUp ? -frame_stride_ : frame_stride_;
        if (format_ == Format::Nv12) { roiW &= ~1; roiH &= ~1; }
        else if (format_ == Format::Yuy2) roiW &= ~1;
        const size_t rowBytes = static_cast<size_t>(roiW) * channels;
        const size_t yEnd = static_cast<size_t>(top + roiH - 1) * srcStride
                          + static_cast<size_t>(left) * channels + rowBytes;

        if (roiW > 0 && roiH > 0 && yEnd <= size)
        {
            size_t total = rowBytes * roiH;
            if (format_ == Format::Nv12)
                total += static_cast<size_t>(roiW) * (roiH / 2);
            job.bytes.resize(total);
            for (int row = 0; row < roiH; ++row)
                std::memcpy(job.bytes.data() + static_cast<size_t>(row) * rowBytes,
                            data + static_cast<size_t>(bottomUp ? frame_height_ - 1 - (top + row) : top + row) * srcStride
                                 + static_cast<size_t>(left) * channels,
                            rowBytes);

            if (format_ == Format::Nv12)
            {
                const size_t uvBase = static_cast<size_t>(srcStride) * frame_height_;
                if (roiH >= 2)
                {
                    const size_t uvEnd = uvBase
                        + static_cast<size_t>(top / 2 + roiH / 2 - 1) * srcStride
                        + left + roiW;
                    if (uvEnd <= size)
                    {
                        uint8_t* dstUv = job.bytes.data() + rowBytes * roiH;
                        for (int row = 0; row < roiH / 2; ++row)
                            std::memcpy(dstUv + static_cast<size_t>(row) * roiW,
                                        data + uvBase
                                             + static_cast<size_t>(top / 2 + row) * srcStride + left,
                                        static_cast<size_t>(roiW));
                        copied = true;
                    }
                }
            }
            else
            {
                copied = true;
            }

            if (copied)
            {
                job.width = roiW;
                job.height = roiH;
                job.stride = static_cast<int>(rowBytes);
            }
        }
    }
    if (!copied)
    {
        job.bytes.resize(size);
        std::memcpy(job.bytes.data(), data, size);
    }

    {
        std::lock_guard<std::mutex> lock(process_mutex_);
        while (static_cast<int>(process_queue_.size()) >= MAX_PROCESS_QUEUE)
        {
            ProcessJob stale = std::move(process_queue_.front());
            process_queue_.pop();
            if (process_free_buffers_.size() < MAX_PROCESS_QUEUE + 8)
                process_free_buffers_.push_back(std::move(stale.bytes));
        }
        process_queue_.push(std::move(job));
    }
    process_cv_.notify_one();
}

void MFCapture::ProcessWorkerLoop()
{
    static constexpr size_t HOST_RING = 8;
    std::array<ProcessJob, HOST_RING> hostRing;
    std::array<cudaEvent_t, HOST_RING> hostDone{};
    std::array<bool, HOST_RING> hostPending{};
    size_t hostIdx = 0;
    const bool rawGpu = gpu_decode_ && format_ != Format::Mjpg;
    if (rawGpu)
        for (auto& event : hostDone)
            cudaEventCreateWithFlags(&event, cudaEventDisableTiming);

    for (;;)
    {
        ProcessJob job;
        {
            std::unique_lock<std::mutex> lock(process_mutex_);
            process_cv_.wait(lock, [this] {
                return process_stop_.load() || !process_queue_.empty();
            });
            if (process_stop_.load())
                break;
            job = std::move(process_queue_.front());
            process_queue_.pop();
        }

        const uint8_t* data = job.bytes.data();
        size_t rawSlot = 0;
        if (rawGpu)
        {
            rawSlot = hostIdx;
            hostIdx = (hostIdx + 1) % HOST_RING;
            if (hostPending[rawSlot] && hostDone[rawSlot])
                cudaEventSynchronize(hostDone[rawSlot]);
            if (!hostRing[rawSlot].bytes.empty())
            {
                std::lock_guard<std::mutex> lock(process_mutex_);
                if (process_free_buffers_.size() < MAX_PROCESS_QUEUE + HOST_RING)
                    process_free_buffers_.push_back(std::move(hostRing[rawSlot].bytes));
            }
            hostRing[rawSlot] = std::move(job);
            data = hostRing[rawSlot].bytes.data();
        }
        const ProcessJob& activeJob = rawGpu ? hostRing[rawSlot] : job;
        process_capture_ns_ = activeJob.capture_ns;
        switch (format_)
        {
        case Format::Nv12:
            gpu_decode_ ? PushNv12Gpu(data, activeJob.width, activeJob.height, activeJob.stride)
                        : PushNv12Cpu(data, activeJob.width, activeJob.height, activeJob.stride);
            break;
        case Format::Yuy2:
            gpu_decode_ ? PushYuy2Gpu(data, activeJob.width, activeJob.height, activeJob.stride)
                        : PushYuy2Cpu(data, activeJob.width, activeJob.height, activeJob.stride);
            break;
        case Format::Rgb32:
            gpu_decode_ ? PushRgb32Gpu(data, activeJob.width, activeJob.height, activeJob.stride)
                        : PushRgb32Cpu(data, activeJob.width, activeJob.height, activeJob.stride);
            break;
        case Format::Mjpg:
            if (gpu_decode_ && !mjpg_cpu_fallback_)
                PushMjpgGpu(data, job.bytes.size());
            else
                PushMjpgCpu(data, job.bytes.size());
            break;
        }

        if (rawGpu)
        {
            if (hostDone[rawSlot])
            {
                cudaEventRecord(hostDone[rawSlot], gpu_stream_);
                hostPending[rawSlot] = true;
            }
            else
            {
                cudaStreamSynchronize(gpu_stream_);
                hostRing[rawSlot].bytes.clear();
            }
        }
        else if (!job.bytes.empty())
        {
            std::lock_guard<std::mutex> lock(process_mutex_);
            if (process_free_buffers_.size() < MAX_PROCESS_QUEUE + HOST_RING)
                process_free_buffers_.push_back(std::move(job.bytes));
        }
    }

    if (gpu_stream_)
        cudaStreamSynchronize(gpu_stream_);
    for (auto& event : hostDone)
        if (event) cudaEventDestroy(event);
}

void MFCapture::TickFps()
{
    ++source_frame_count_;
    const auto now = std::chrono::steady_clock::now();
    const std::chrono::duration<double> elapsed = now - source_fps_start_;
    if (elapsed.count() >= 0.5)
    {
        const double instant = source_frame_count_ / elapsed.count();
        source_fps_smoothed_ = source_fps_smoothed_ <= 0.0
            ? instant
            : source_fps_smoothed_ * 0.75 + instant * 0.25;
        source_fps_.store(static_cast<int>(std::lround(source_fps_smoothed_)));
        source_frame_count_ = 0;
        source_fps_start_ = now;
    }
}

int MFCapture::GetDeviceFrameAgeUs() const
{
    return device_age_.read(runtime::latency::nowNs());
}

void MFCapture::EnqueueCpu(cv::Mat&& frame)
{
    if (frame.empty())
        return;
    {
        std::lock_guard<std::mutex> lock(frame_mutex_);
        while (static_cast<int>(cpu_frame_queue_.size()) >= MAX_QUEUE_SIZE)
            cpu_frame_queue_.pop();
        cpu_frame_queue_.push({std::move(frame), process_capture_ns_});
    }
    frame_cv_.notify_one();
}

void MFCapture::EnqueueGpu(GpuImage&& frame)
{
    if (frame.empty())
        return;
    {
        std::lock_guard<std::mutex> lock(frame_mutex_);
        while (static_cast<int>(gpu_frame_queue_.size()) >= MAX_QUEUE_SIZE)
            gpu_frame_queue_.pop();
        gpu_frame_queue_.push({std::move(frame), process_capture_ns_});
    }
    frame_cv_.notify_one();
}

bool MFCapture::WaitFrame(int timeoutMs)
{
    std::unique_lock<std::mutex> lock(frame_mutex_);
    return frame_cv_.wait_for(
        lock,
        std::chrono::milliseconds(timeoutMs),
        [this] { return !gpu_frame_queue_.empty() || !cpu_frame_queue_.empty(); });
}

void MFCapture::StartDecodeWorkers()
{
    if (!(format_ == Format::Mjpg && gpu_decode_ && crop_enabled_))
        return;
    workers_stop_.store(false);
    enqueued_seq_.store(0);
    job_seq_ = 0;
    decode_workers_.clear();
    for (int i = 0; i < DECODE_WORKERS; ++i)
        decode_workers_.emplace_back(&MFCapture::DecodeWorkerLoop, this);
}

void MFCapture::StopDecodeWorkers()
{
    if (decode_workers_.empty())
        return;
    workers_stop_.store(true);
    job_cv_.notify_all();
    for (auto& t : decode_workers_)
        if (t.joinable())
            t.join();
    decode_workers_.clear();
    std::lock_guard<std::mutex> lock(job_mutex_);
    std::queue<DecodeJob> empty;
    job_queue_.swap(empty);
}

void MFCapture::EnqueueJpegJob(const uint8_t* data, size_t size, int64_t capture_ns)
{
    if (!data || size == 0)
        return;
    DecodeJob job;
    job.capture_ns = capture_ns;
    job.jpeg.assign(data, data + size);
    job.seq = ++job_seq_;
    {
        std::lock_guard<std::mutex> lock(job_mutex_);
        while (static_cast<int>(job_queue_.size()) >= MAX_JOB_QUEUE)
            job_queue_.pop();
        job_queue_.push(std::move(job));
    }
    job_cv_.notify_one();
}

void MFCapture::SetTargetFps(int fps)
{
    target_fps_.store(std::max(0, fps));
}

bool MFCapture::ShouldDispatchFrame()
{
    const int fps = target_fps_.load();
    if (fps <= 0)
        return true;
    const int sourceFps = negotiated_fps_.load();
    const int tolerance = std::max(2, fps / 100);
    if (sourceFps > 0 && sourceFps <= fps + tolerance)
        return true;
    const auto now = std::chrono::steady_clock::now();
    const auto interval = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
        std::chrono::duration<double, std::milli>(1000.0 / fps));
    if (next_dispatch_.time_since_epoch().count() == 0)
    {
        next_dispatch_ = now + interval;
        return true;
    }
    if (now >= next_dispatch_)
    {
        next_dispatch_ += interval;
        if (next_dispatch_ < now)
            next_dispatch_ = now + interval;
        return true;
    }
    return false;
}

void MFCapture::DecodeWorkerLoop()
{
    capture::GpuJpegDecoder decoder;
    if (!decoder.init())
    {
        std::cerr << u8"[MFCapture] decode worker: GpuJpegDecoder init 失败" << std::endl;
        return;
    }
    cudaStream_t stream = nullptr;
    if (cudaStreamCreateWithFlags(&stream, cudaStreamNonBlocking) != cudaSuccess)
        return;

    constexpr int RING = 3;
    std::array<GpuImage, RING> outRing;
    GpuReadyEventPool<RING> evRing;
    size_t ringIdx = 0;

    for (;;)
    {
        DecodeJob job;
        {
            std::unique_lock<std::mutex> lock(job_mutex_);
            job_cv_.wait(lock, [this] { return !job_queue_.empty() || workers_stop_.load(); });
            if (workers_stop_.load())
                break;
            job = std::move(job_queue_.front());
            job_queue_.pop();
        }

        const size_t slot = ringIdx;
        ringIdx = (ringIdx + 1) % RING;
        GpuImage& dst = outRing[slot];
        if (!decoder.decodeCropped(job.jpeg.data(), job.jpeg.size(), out_side_, out_side_, dst, stream))
            continue;

        auto event = evRing.record(stream);
        if (!event && cudaStreamSynchronize(stream) != cudaSuccess) continue;
        GpuImage out = dst;
        out.setReadyEvent(std::move(event));
        EnqueueGpuOrdered(std::move(out), job.seq, job.capture_ns);
    }

    cudaStreamSynchronize(stream);
    cudaStreamDestroy(stream);
}

bool MFCapture::EnqueueGpuOrdered(GpuImage&& frame, uint64_t seq, int64_t capture_ns)
{
    if (frame.empty())
        return false;
    {
        std::lock_guard<std::mutex> lock(frame_mutex_);
        if (seq <= enqueued_seq_.load(std::memory_order_relaxed))
            return false;
        enqueued_seq_.store(seq, std::memory_order_relaxed);
        while (static_cast<int>(gpu_frame_queue_.size()) >= MAX_QUEUE_SIZE)
            gpu_frame_queue_.pop();
        gpu_frame_queue_.push({std::move(frame), capture_ns});

    }
    frame_cv_.notify_one();
    return true;
}

void MFCapture::ResolveRoi(int width, int height, int& left, int& top, int& roiW, int& roiH) const
{
    if (crop_enabled_ && width >= out_side_ && height >= out_side_)
    {
        roiW = roiH = out_side_;
        left = ((width - out_side_) / 2) & ~1;
        top  = ((height - out_side_) / 2) & ~1;
    }
    else
    {
        left = 0;
        top = 0;
        roiW = width;
        roiH = height;
    }
}

GpuImage& MFCapture::nextOutSlot()
{
    current_out_idx_ = out_pool_idx_;
    GpuImage& slot = out_pool_[out_pool_idx_];
    out_pool_idx_ = (out_pool_idx_ + 1) % OUT_POOL_SIZE;
    return slot;
}

GpuImage MFCapture::ResizeToOut(const GpuImage& src)
{
    if (src.empty())
        return GpuImage();
    GpuImage& slot = nextOutSlot();
    if (!slot.create(out_side_, out_side_, 3))
        return GpuImage();
    launch_resize_bgr_u8_bilinear(src.data(), src.step(), src.cols(), src.rows(),
                                  slot.data(), slot.step(), out_side_, out_side_, gpu_stream_);
    return slot;
}

cv::Mat MFCapture::FinalizeCpu(cv::Mat bgr)
{
    if (bgr.empty())
        return cv::Mat();
    if (bgr.cols == out_side_ && bgr.rows == out_side_)
        return std::move(bgr);

    if (crop_enabled_ && bgr.cols >= out_side_ && bgr.rows >= out_side_)
    {
        const int left = (bgr.cols - out_side_) / 2;
        const int top = (bgr.rows - out_side_) / 2;
        return bgr(cv::Rect(left, top, out_side_, out_side_)).clone();
    }

    cv::Mat out;
    cv::resize(bgr, out, cv::Size(out_side_, out_side_));
    return out;
}

bool MFCapture::PushNv12Gpu(const uint8_t* data, int width, int height, int stride)
{
    if (!gpu_stream_ || !data || width <= 0 || height <= 0)
        return false;
    if (stride <= 0)
        stride = width;

    int left, top, roiW, roiH;
    ResolveRoi(width, height, left, top, roiW, roiH);

    if (!scratch_a_.upload(data + static_cast<size_t>(top) * stride + left,
                           roiH, roiW, 1, stride, gpu_stream_))
        return false;
    const uint8_t* uvBase = data + static_cast<size_t>(stride) * height;
    if (!scratch_b_.upload(uvBase + static_cast<size_t>(top / 2) * stride + left,
                           roiH / 2, roiW, 1, stride, gpu_stream_))
        return false;

    const Npp8u* pSrc[2] = { scratch_a_.data(), scratch_b_.data() };
    NppiSize roi = { roiW, roiH };
    const bool direct = crop_enabled_ && roiW == out_side_ && roiH == out_side_;
    GpuImage& dst = direct ? nextOutSlot() : scratch_full_;
    if (!dst.create(roiH, roiW, 3))
        return false;
    if (nppiNV12ToBGR_8u_P2C3R_Ctx(pSrc, static_cast<int>(scratch_a_.step()),
                                   dst.data(), static_cast<int>(dst.step()),
                                   roi, npp_ctx_) != NPP_SUCCESS)
        return false;

    GpuImage out = direct ? dst : ResizeToOut(scratch_full_);
    if (out.empty())
        return false;
    auto event = out_events_.record(gpu_stream_);
    if (!event && cudaStreamSynchronize(gpu_stream_) != cudaSuccess) return false;
    out.setReadyEvent(std::move(event));
    EnqueueGpu(std::move(out));
    return true;
}

bool MFCapture::PushYuy2Gpu(const uint8_t* data, int width, int height, int stride)
{
    if (!gpu_stream_ || !data || width <= 0 || height <= 0)
        return false;
    if (stride <= 0)
        stride = width * 2;

    int left, top, roiW, roiH;
    ResolveRoi(width, height, left, top, roiW, roiH);

    if (!scratch_a_.upload(data + static_cast<size_t>(top) * stride + static_cast<size_t>(left) * 2,
                           roiH, roiW, 2, stride, gpu_stream_))
        return false;

    NppiSize roi = { roiW, roiH };
    const bool direct = crop_enabled_ && roiW == out_side_ && roiH == out_side_;
    GpuImage& dst = direct ? nextOutSlot() : scratch_full_;
    if (!dst.create(roiH, roiW, 3))
        return false;
    if (nppiYCbCr422ToBGR_8u_C2C3R_Ctx(scratch_a_.data(), static_cast<int>(scratch_a_.step()),
                                       dst.data(), static_cast<int>(dst.step()),
                                       roi, npp_ctx_) != NPP_SUCCESS)
        return false;

    GpuImage out = direct ? dst : ResizeToOut(scratch_full_);
    if (out.empty())
        return false;
    auto event = out_events_.record(gpu_stream_);
    if (!event && cudaStreamSynchronize(gpu_stream_) != cudaSuccess) return false;
    out.setReadyEvent(std::move(event));
    EnqueueGpu(std::move(out));
    return true;
}

bool MFCapture::PushRgb32Gpu(const uint8_t* data, int width, int height, int stride)
{
    if (!gpu_stream_ || !data || width <= 0 || height <= 0)
        return false;
    if (stride <= 0)
        stride = width * 4;

    int left, top, roiW, roiH;
    ResolveRoi(width, height, left, top, roiW, roiH);

    if (!scratch_a_.upload(data + static_cast<size_t>(top) * stride + static_cast<size_t>(left) * 4,
                           roiH, roiW, 4, stride, gpu_stream_))
        return false;

    const bool direct = crop_enabled_ && roiW == out_side_ && roiH == out_side_;
    GpuImage& dst = direct ? nextOutSlot() : scratch_full_;
    if (!dst.create(roiH, roiW, 3))
        return false;
    launch_bgra_to_bgr_u8(scratch_a_.data(), scratch_a_.step(), dst.data(), dst.step(),
                          roiW, roiH, gpu_stream_);

    GpuImage out = direct ? dst : ResizeToOut(scratch_full_);
    if (out.empty())
        return false;
    auto event = out_events_.record(gpu_stream_);
    if (!event && cudaStreamSynchronize(gpu_stream_) != cudaSuccess) return false;
    out.setReadyEvent(std::move(event));
    EnqueueGpu(std::move(out));
    return true;
}

bool MFCapture::PushMjpgGpu(const uint8_t* data, size_t size)
{
    if (!gpu_decoder_ || !gpu_stream_ || !data || size == 0)
        return false;

    if (pinned_jpeg_capacity_ < size)
    {
        if (pinned_jpeg_buffer_)
            cudaFreeHost(pinned_jpeg_buffer_);
        pinned_jpeg_buffer_ = nullptr;
        pinned_jpeg_capacity_ = 0;

        const size_t want = std::max<size_t>(size, 256 * 1024);
        if (cudaHostAlloc(reinterpret_cast<void**>(&pinned_jpeg_buffer_), want, cudaHostAllocDefault) == cudaSuccess)
            pinned_jpeg_capacity_ = want;
    }

    const uint8_t* decodeSrc = data;
    if (pinned_jpeg_buffer_ && pinned_jpeg_capacity_ >= size)
    {
        std::memcpy(pinned_jpeg_buffer_, data, size);
        decodeSrc = pinned_jpeg_buffer_;
    }

    GpuImage out;
    if (crop_enabled_)
    {
        GpuImage& slot = nextOutSlot();
        if (gpu_decoder_->decodeCropped(decodeSrc, size, out_side_, out_side_, slot, gpu_stream_))
            out = slot;
    }

    if (out.empty())
    {
        if (!gpu_decoder_->decode(decodeSrc, size, scratch_full_, gpu_stream_))
            return false;
        if (crop_enabled_ && scratch_full_.cols() >= out_side_ && scratch_full_.rows() >= out_side_)
        {
            const int left = (scratch_full_.cols() - out_side_) / 2;
            const int top = (scratch_full_.rows() - out_side_) / 2;
            out = ResizeToOut(scratch_full_.subRect(left, top, out_side_, out_side_));
        }
        else
        {
            out = ResizeToOut(scratch_full_);
        }
    }

    if (out.empty())
        return false;

    auto event = out_events_.record(gpu_stream_);
    if (!event && cudaStreamSynchronize(gpu_stream_) != cudaSuccess) return false;
    out.setReadyEvent(std::move(event));
    EnqueueGpu(std::move(out));
    return true;
}

bool MFCapture::PushNv12Cpu(const uint8_t* data, int width, int height, int stride)
{
    if (!data || width <= 0 || height <= 0)
        return false;
    if (stride <= 0)
        stride = width;

    cv::Mat nv12(height + height / 2, width, CV_8UC1, const_cast<uint8_t*>(data), static_cast<size_t>(stride));
    cv::Mat bgr;
    cv::cvtColor(nv12, bgr, cv::COLOR_YUV2BGR_NV12);
    cv::Mat out = FinalizeCpu(bgr);
    if (out.empty())
        return false;
    EnqueueCpu(std::move(out));
    return true;
}

bool MFCapture::PushYuy2Cpu(const uint8_t* data, int width, int height, int stride)
{
    if (!data || width <= 0 || height <= 0)
        return false;
    if (stride <= 0)
        stride = width * 2;

    cv::Mat yuy2(height, width, CV_8UC2, const_cast<uint8_t*>(data), static_cast<size_t>(stride));
    cv::Mat bgr;
    cv::cvtColor(yuy2, bgr, cv::COLOR_YUV2BGR_YUY2);
    cv::Mat out = FinalizeCpu(bgr);
    if (out.empty())
        return false;
    EnqueueCpu(std::move(out));
    return true;
}

bool MFCapture::PushRgb32Cpu(const uint8_t* data, int width, int height, int stride)
{
    if (!data || width <= 0 || height <= 0)
        return false;
    if (stride <= 0)
        stride = width * 4;

    cv::Mat bgra(height, width, CV_8UC4, const_cast<uint8_t*>(data), static_cast<size_t>(stride));
    cv::Mat bgr;
    cv::cvtColor(bgra, bgr, cv::COLOR_BGRA2BGR);
    cv::Mat out = FinalizeCpu(bgr);
    if (out.empty())
        return false;
    EnqueueCpu(std::move(out));
    return true;
}

bool MFCapture::PushMjpgCpu(const uint8_t* data, size_t size)
{
    if (!data || size == 0)
        return false;

    cv::Mat raw(1, static_cast<int>(size), CV_8UC1, const_cast<uint8_t*>(data));
    cv::Mat bgr = cv::imdecode(raw, cv::IMREAD_COLOR);
    if (bgr.empty())
        return false;
    cv::Mat out = FinalizeCpu(bgr);
    if (out.empty())
        return false;
    EnqueueCpu(std::move(out));
    return true;
}
