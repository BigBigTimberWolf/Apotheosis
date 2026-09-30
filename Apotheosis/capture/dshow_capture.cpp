#include "dshow_capture.h"

#define WIN32_LEAN_AND_MEAN
#define _WINSOCKAPI_
#define NOMINMAX
#include <Windows.h>
#include <dshow.h>
#include <dvdmedia.h>
#include <wrl/client.h>

#include "../mem/gpu_ready_event.h"
#include "gpu_color_ops.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>

using Microsoft::WRL::ComPtr;

namespace dshow
{
namespace
{
std::string WideToUtf8(const wchar_t* value)
{
    if (!value || !*value) return {};
    const int count = WideCharToMultiByte(CP_UTF8, 0, value, -1, nullptr, 0, nullptr, nullptr);
    if (count <= 1) return {};
    std::string result(static_cast<size_t>(count), '\0');
    if (!WideCharToMultiByte(CP_UTF8, 0, value, -1, result.data(), count, nullptr, nullptr))
        return {};
    result.pop_back();
    return result;
}

void FreeMediaType(AM_MEDIA_TYPE* type)
{
    if (!type) return;
    if (type->cbFormat) CoTaskMemFree(type->pbFormat);
    if (type->pUnk) type->pUnk->Release();
    CoTaskMemFree(type);
}

const char* FormatName(const GUID& subtype)
{
    if (subtype == MEDIASUBTYPE_YUY2) return "YUY2";
    if (subtype == MEDIASUBTYPE_MJPG) return "MJPG";
    if (subtype == MEDIASUBTYPE_NV12) return "NV12";
    if (subtype == MEDIASUBTYPE_RGB24) return "RGB24";
    if (subtype == MEDIASUBTYPE_RGB32) return "RGB32";
    return nullptr;
}

void AddCapabilities(IBaseFilter* filter, Device& device)
{
    ComPtr<IEnumPins> pins;
    if (FAILED(filter->EnumPins(&pins))) return;
    ComPtr<IPin> pin;
    while (pins->Next(1, &pin, nullptr) == S_OK)
    {
        PIN_DIRECTION direction{};
        ComPtr<IAMStreamConfig> config;
        if (SUCCEEDED(pin->QueryDirection(&direction)) && direction == PINDIR_OUTPUT
            && SUCCEEDED(pin.As(&config)))
        {
            int count = 0, size = 0;
            if (SUCCEEDED(config->GetNumberOfCapabilities(&count, &size))
                && size == sizeof(VIDEO_STREAM_CONFIG_CAPS) && count > 0 && count < 4096)
            {
                std::vector<BYTE> buffer(static_cast<size_t>(size));
                for (int i = 0; i < count; ++i)
                {
                    AM_MEDIA_TYPE* type = nullptr;
                    const HRESULT hr = config->GetStreamCaps(i, &type, buffer.data());
                    if (FAILED(hr) || !type) { FreeMediaType(type); continue; }
                    const char* format = FormatName(type->subtype);
                    int w = 0, h = 0;
                    LONGLONG nominalInterval = 0;
                    if (type->pbFormat && type->formattype == FORMAT_VideoInfo
                        && type->cbFormat >= sizeof(VIDEOINFOHEADER))
                    {
                        const auto* header = reinterpret_cast<const VIDEOINFOHEADER*>(type->pbFormat);
                        w = std::abs(header->bmiHeader.biWidth);
                        h = std::abs(header->bmiHeader.biHeight);
                        nominalInterval = header->AvgTimePerFrame;
                    }
                    else if (type->pbFormat && type->formattype == FORMAT_VideoInfo2
                             && type->cbFormat >= sizeof(VIDEOINFOHEADER2))
                    {
                        const auto* header = reinterpret_cast<const VIDEOINFOHEADER2*>(type->pbFormat);
                        w = std::abs(header->bmiHeader.biWidth);
                        h = std::abs(header->bmiHeader.biHeight);
                        nominalInterval = header->AvgTimePerFrame;
                    }
                    const auto* limits = reinterpret_cast<const VIDEO_STREAM_CONFIG_CAPS*>(buffer.data());
                    if (format && w > 0 && h > 0 && w <= 8192 && h <= 8192)
                    {
                        auto found = std::find_if(device.caps.begin(), device.caps.end(),
                            [&](const MFCapability& cap) {
                                return cap.format == format && cap.width == w && cap.height == h;
                            });
                        if (found == device.caps.end())
                        {
                            device.caps.push_back({format, w, h, {}, true});
                            found = std::prev(device.caps.end());
                        }
                        for (const LONGLONG interval : {nominalInterval, limits->MinFrameInterval})
                        {
                            const int fps = interval > 0 ? static_cast<int>(std::lround(10000000.0 / interval)) : 0;
                            if (fps <= 0 || fps > 1000) continue;
                            if (std::find(found->fps.begin(), found->fps.end(), fps) == found->fps.end())
                                found->fps.push_back(fps);
                        }
                    }
                    FreeMediaType(type);
                }
            }
        }
        pin.Reset();
    }
    for (auto& cap : device.caps)
        std::sort(cap.fps.begin(), cap.fps.end());
}

std::vector<Device> EnumerateOnStaThread()
{
    std::vector<Device> devices;
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(initialized)) return devices;
    ComPtr<ICreateDevEnum> enumerator;
    if (SUCCEEDED(CoCreateInstance(CLSID_SystemDeviceEnum, nullptr, CLSCTX_INPROC_SERVER,
                                   IID_PPV_ARGS(&enumerator))))
    {
        ComPtr<IEnumMoniker> monikers;
        if (enumerator->CreateClassEnumerator(CLSID_VideoInputDeviceCategory,
                                             &monikers, 0) == S_OK && monikers)
        {
            ComPtr<IMoniker> moniker;
            while (monikers->Next(1, &moniker, nullptr) == S_OK)
            {
                Device device;
                device.index = static_cast<int>(devices.size());
                ComPtr<IPropertyBag> bag;
                if (SUCCEEDED(moniker->BindToStorage(nullptr, nullptr, IID_PPV_ARGS(&bag))))
                {
                    VARIANT name;
                    VariantInit(&name);
                    if (SUCCEEDED(bag->Read(L"FriendlyName", &name, nullptr)) && name.vt == VT_BSTR)
                        device.name = WideToUtf8(name.bstrVal);
                    VariantClear(&name);
                }
                ComPtr<IBaseFilter> filter;
                if (SUCCEEDED(moniker->BindToObject(nullptr, nullptr, IID_PPV_ARGS(&filter))))
                    AddCapabilities(filter.Get(), device);
                devices.push_back(std::move(device));
                moniker.Reset();
            }
        }
    }
    enumerator.Reset();
    CoUninitialize();
    return devices;
}

const GUID* SubtypeFor(const std::string& format)
{
    if (format == "YUY2") return &MEDIASUBTYPE_YUY2;
    if (format == "MJPG") return &MEDIASUBTYPE_MJPG;
    if (format == "NV12") return &MEDIASUBTYPE_NV12;
    if (format == "RGB24") return &MEDIASUBTYPE_RGB24;
    if (format == "RGB32") return &MEDIASUBTYPE_RGB32;
    return nullptr;
}

int64_t NowNs()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

struct __declspec(uuid("0579154A-2B53-4994-B0D0-E773148EFF85")) ISampleGrabberCB : IUnknown
{
    virtual HRESULT STDMETHODCALLTYPE SampleCB(double, IMediaSample*) = 0;
    virtual HRESULT STDMETHODCALLTYPE BufferCB(double, BYTE*, long) = 0;
};

struct __declspec(uuid("6B652FFF-11FE-4FCE-92AD-0266B5D7C78F")) ISampleGrabber : IUnknown
{
    virtual HRESULT STDMETHODCALLTYPE SetOneShot(BOOL) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetMediaType(const AM_MEDIA_TYPE*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetConnectedMediaType(AM_MEDIA_TYPE*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetBufferSamples(BOOL) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetCurrentBuffer(long*, long*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetCurrentSample(IMediaSample**) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetCallback(ISampleGrabberCB*, long) = 0;
};

const GUID kSampleGrabberClsid =
    {0xC1F400A0, 0x3F08, 0x11D3, {0x9F, 0x0B, 0x00, 0x60, 0x08, 0x03, 0x9E, 0x37}};
const GUID kNullRendererClsid =
    {0xC1F400A4, 0x3F08, 0x11D3, {0x9F, 0x0B, 0x00, 0x60, 0x08, 0x03, 0x9E, 0x37}};

class FrameCallback final : public ISampleGrabberCB
{
public:
    FrameCallback(int width, int height, int stride, int side, bool nv12)
        : nv12_(nv12), width_(width), height_(height), stride_(stride)
    {
        if (nv12_)
        {
            crop_ = std::min({width, height, side}) & ~1;
            left_ = ((width - crop_) / 2) & ~1;
            top_ = ((height - crop_) / 2) & ~1;
        }
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** output) override
    {
        if (!output) return E_POINTER;
        *output = nullptr;
        if (iid == __uuidof(IUnknown) || iid == __uuidof(ISampleGrabberCB))
        {
            *output = static_cast<ISampleGrabberCB*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override
    {
        const ULONG left = --refs_;
        if (!left) delete this;
        return left;
    }
    HRESULT STDMETHODCALLTYPE SampleCB(double, IMediaSample*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE BufferCB(double, BYTE* data, long length) override
    {
        if (!data || length <= 0) return S_OK;
        std::lock_guard<std::mutex> lock(mutex_);
        const int64_t received = NowNs();
        if (nv12_)
        {
            const size_t required = static_cast<size_t>(stride_) * (height_ + height_ / 2);
            if (crop_ < 2 || stride_ < width_ || static_cast<size_t>(length) < required)
                return S_OK;
            pixels_.resize(static_cast<size_t>(crop_) * (crop_ + crop_ / 2));
            for (int row = 0; row < crop_; ++row)
                std::memcpy(pixels_.data() + static_cast<size_t>(row) * crop_,
                            data + static_cast<size_t>(top_ + row) * stride_ + left_, crop_);
            const size_t uv_base = static_cast<size_t>(height_) * stride_;
            for (int row = 0; row < crop_ / 2; ++row)
                std::memcpy(pixels_.data() + static_cast<size_t>(crop_ + row) * crop_,
                            data + uv_base + static_cast<size_t>(top_ / 2 + row) * stride_ + left_, crop_);
        }
        else
            pixels_.assign(data, data + length);
        capture_ns_ = received;
        has_frame_ = true;
        ++source_frame_count_;
        if (!source_fps_start_ns_) source_fps_start_ns_ = received;
        const int64_t elapsed = received - source_fps_start_ns_;
        if (elapsed >= 1000000000LL)
        {
            source_fps_.store(static_cast<int>(std::llround(
                static_cast<double>(source_frame_count_) * 1000000000.0 / elapsed)),
                std::memory_order_relaxed);
            source_frame_count_ = 0;
            source_fps_start_ns_ = received;
        }
        cv_.notify_one();
        return S_OK;
    }
    bool Take(std::vector<BYTE>& pixels, int64_t& captured)
    {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait_for(lock, std::chrono::milliseconds(20), [&] { return has_frame_; });
        if (!has_frame_) return false;
        pixels.swap(pixels_);
        captured = capture_ns_;
        has_frame_ = false;
        return true;
    }
    int SourceFpsEstimate() const { return source_fps_.load(std::memory_order_relaxed); }
    int CropSide() const { return crop_; }
private:
    std::atomic<ULONG> refs_{1};
    std::mutex mutex_;
    std::condition_variable cv_;
    std::vector<BYTE> pixels_;
    int64_t capture_ns_ = 0;
    bool has_frame_ = false;
    bool nv12_ = false;
    int width_ = 0;
    int height_ = 0;
    int stride_ = 0;
    int crop_ = 0;
    int left_ = 0;
    int top_ = 0;
    int source_frame_count_ = 0;
    int64_t source_fps_start_ns_ = 0;
    std::atomic<int> source_fps_{0};
};

ComPtr<IBaseFilter> FilterAtIndex(int index)
{
    ComPtr<IBaseFilter> filter;
    ComPtr<ICreateDevEnum> enumerator;
    if (FAILED(CoCreateInstance(CLSID_SystemDeviceEnum, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&enumerator)))) return filter;
    ComPtr<IEnumMoniker> monikers;
    if (enumerator->CreateClassEnumerator(CLSID_VideoInputDeviceCategory,
                                         &monikers, 0) != S_OK || !monikers) return filter;
    ComPtr<IMoniker> moniker;
    for (int i = 0; monikers->Next(1, &moniker, nullptr) == S_OK; ++i)
    {
        if (i == index)
        {
            moniker->BindToObject(nullptr, nullptr, IID_PPV_ARGS(&filter));
            break;
        }
        moniker.Reset();
    }
    return filter;
}

bool SelectMode(IAMStreamConfig* config, int width, int height,
                int fps, const std::string& format)
{
    if (!config || width <= 0 || height <= 0 || fps <= 0) return false;
    const GUID* subtype = SubtypeFor(format);
    if (!subtype) return false;
    int count = 0, size = 0;
    if (FAILED(config->GetNumberOfCapabilities(&count, &size))
        || size != sizeof(VIDEO_STREAM_CONFIG_CAPS) || count < 1 || count > 4096)
        return false;
    std::vector<BYTE> buffer(static_cast<size_t>(size));
    for (int i = 0; i < count; ++i)
    {
        AM_MEDIA_TYPE* type = nullptr;
        if (FAILED(config->GetStreamCaps(i, &type, buffer.data())) || !type)
        {
            FreeMediaType(type);
            continue;
        }
        int w = 0, h = 0;
        if (type->pbFormat && type->formattype == FORMAT_VideoInfo
            && type->cbFormat >= sizeof(VIDEOINFOHEADER))
        {
            const auto* header = reinterpret_cast<const VIDEOINFOHEADER*>(type->pbFormat);
            w = std::abs(header->bmiHeader.biWidth);
            h = std::abs(header->bmiHeader.biHeight);
        }
        else if (type->pbFormat && type->formattype == FORMAT_VideoInfo2
                 && type->cbFormat >= sizeof(VIDEOINFOHEADER2))
        {
            const auto* header = reinterpret_cast<const VIDEOINFOHEADER2*>(type->pbFormat);
            w = std::abs(header->bmiHeader.biWidth);
            h = std::abs(header->bmiHeader.biHeight);
        }
        bool accepted = false;
        if (type->subtype == *subtype && w == width && h == height)
        {
            const auto* limits = reinterpret_cast<const VIDEO_STREAM_CONFIG_CAPS*>(buffer.data());
            LONGLONG interval = static_cast<LONGLONG>(std::llround(10000000.0 / fps));
            if (limits->MinFrameInterval > 0 && limits->MaxFrameInterval >= limits->MinFrameInterval)
                interval = std::clamp(interval, limits->MinFrameInterval, limits->MaxFrameInterval);
            if (interval > 0)
            {
                if (type->formattype == FORMAT_VideoInfo)
                    reinterpret_cast<VIDEOINFOHEADER*>(type->pbFormat)->AvgTimePerFrame = interval;
                else if (type->formattype == FORMAT_VideoInfo2)
                    reinterpret_cast<VIDEOINFOHEADER2*>(type->pbFormat)->AvgTimePerFrame = interval;
            }
            accepted = SUCCEEDED(config->SetFormat(type));
        }
        FreeMediaType(type);
        if (accepted) return true;
    }
    return false;
}

class Capture final : public IScreenCapture
{
public:
    Capture(int index, int width, int height, int fps, const std::string& format, int side)
        : side_(std::max(1, side)), target_fps_(std::max(0, fps)), format_(format)
    {
        const HRESULT initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (FAILED(initialized)) throw std::runtime_error("DirectShow COM initialization failed");
        com_initialized_ = true;
        try
        {
            Open(index, width, height, fps, format);
            if (format_ == "NV12") InitializeNv12Gpu();
        }
        catch (...) { Close(); throw; }
    }

    ~Capture() override { Close(); }

    cv::Mat GetNextFrameCpu() override { return {}; }

    GpuImage GetNextFrameGpu() override
    {
        auto& pixels = frame_pixels_;
        int64_t captured = 0;
        auto* callback = static_cast<FrameCallback*>(callback_.Get());
        if (!callback->Take(pixels, captured))
        {
            const auto now = std::chrono::steady_clock::now();
            if (now - last_wait_log_ >= std::chrono::seconds(3))
            {
                std::cerr << "[DirectShow] Graph is running but no frame callback was received."
                          << std::endl;
                last_wait_log_ = now;
            }
            return {};
        }
        const size_t required = format_ == "NV12"
            ? static_cast<size_t>(callback->CropSide()) * (callback->CropSide() + callback->CropSide() / 2)
            : format_ == "MJPG" ? 1u : static_cast<size_t>(stride_) * height_;
        if (stride_ <= 0 || height_ <= 0 || pixels.size() < required)
        {
            if (!bad_frame_logged_)
            {
                std::cerr << "[DirectShow] Sample size " << pixels.size()
                          << " is smaller than " << format_ << " frame "
                          << required << std::endl;
                bad_frame_logged_ = true;
            }
            return {};
        }
        if (!first_frame_logged_)
        {
            std::cout << "[DirectShow] First frame received: " << width_ << "x"
                      << height_ << ", buffer=" << pixels.size() << " bytes"
                      << std::endl;
            first_frame_logged_ = true;
        }
        const auto now = std::chrono::steady_clock::now();
        if (target_fps_ > 0 && source_fps_ > target_fps_ + std::max(2, target_fps_ / 100)
            && last_output_.time_since_epoch().count() != 0
            && now - last_output_ < std::chrono::microseconds(1000000 / target_fps_))
            return {};
        cv::Mat output;
        if (format_ == "NV12")
        {
            const int crop = callback->CropSide();
            if (nv12_gpu_ready_ && crop == side_)
            {
                GpuImage frame = ConvertNv12Gpu(pixels.data(), crop);
                if (!frame.empty())
                {
                    last_capture_ns_ = captured;
                    last_output_ = now;
                    return frame;
                }
                // Preserve capture if CUDA/NPP cannot process a frame.
                cudaStreamSynchronize(nv12_stream_);
                nv12_gpu_ready_ = false;
                std::cerr << "[DirectShow] NV12 GPU conversion unavailable; using CPU conversion."
                          << std::endl;
            }
            cv::Mat cropped(crop + crop / 2, crop, CV_8UC1, pixels.data());
            cv::cvtColor(cropped, converted_, cv::COLOR_YUV2BGR_NV12);
            output = converted_;
        }
        else if (format_ == "YUY2")
        {
            cv::Mat raw(height_, width_, CV_8UC2, pixels.data(), stride_);
            const int crop = std::min({width_, height_, side_}) & ~1;
            const cv::Rect center(((width_ - crop) / 2) & ~1,
                                  ((height_ - crop) / 2) & ~1, crop, crop);
            cv::cvtColor(raw(center), output, cv::COLOR_YUV2BGR_YUY2);
        }
        else if (format_ == "MJPG")
        {
            cv::Mat encoded(1, static_cast<int>(pixels.size()), CV_8UC1, pixels.data());
            cv::Mat decoded = cv::imdecode(encoded, cv::IMREAD_COLOR);
            if (decoded.empty()) return {};
            const int crop = std::min({decoded.cols, decoded.rows, side_});
            const cv::Rect center((decoded.cols - crop) / 2,
                                  (decoded.rows - crop) / 2, crop, crop);
            output = decoded(center).clone();
        }
        else
        {
            const int channels = format_ == "RGB32" ? 4 : 3;
            cv::Mat raw(height_, width_, CV_MAKETYPE(CV_8U, channels), pixels.data(), stride_);
            const int crop = std::min({width_, height_, side_});
            const int left = (width_ - crop) / 2;
            const int top = (height_ - crop) / 2;
            const cv::Rect center(left, bottom_up_ ? height_ - top - crop : top,
                                  crop, crop);
            cv::Mat upright;
            if (bottom_up_) cv::flip(raw(center), upright, 0);
            else upright = raw(center);
            if (channels == 4) cv::cvtColor(upright, output, cv::COLOR_BGRA2BGR);
            else output = upright;
        }
        if (output.empty()) return {};
        if (output.cols != side_ || output.rows != side_)
            cv::resize(output, output, cv::Size(side_, side_), 0, 0, cv::INTER_LINEAR);
        GpuImage& gpu = gpu_slots_[next_gpu_slot_++ % gpu_slots_.size()];
        if (!gpu.upload(output.data, output.rows, output.cols, 3, output.step))
        {
            if (!upload_error_logged_)
            {
                std::cerr << "[DirectShow] GPU frame upload failed." << std::endl;
                upload_error_logged_ = true;
            }
            return {};
        }
        last_capture_ns_ = captured;
        last_output_ = now;
        return gpu;
    }

    int GetSourceFpsEstimate() const override
    {
        const int observed = static_cast<const FrameCallback*>(callback_.Get())->SourceFpsEstimate();
        return observed > 0 ? observed : source_fps_;
    }
    int64_t GetLastFrameCaptureNs() const override { return last_capture_ns_; }
    void SetTargetFps(int fps) override { target_fps_ = std::max(0, fps); }
    bool HandlesTargetFps() const override { return true; }

private:
    struct PinnedNv12Slot
    {
        unsigned char* data = nullptr;
        cudaEvent_t upload_done = nullptr;
        bool pending = false;
    };

    void InitializeNv12Gpu()
    {
        const int crop = static_cast<FrameCallback*>(callback_.Get())->CropSide();
        if (crop != side_ || crop <= 0)
            return; // The existing resize path handles models larger than the source.
        if (cudaStreamCreateWithFlags(&nv12_stream_, cudaStreamNonBlocking) != cudaSuccess)
            return;
        if (!nv12_input_.create(crop + crop / 2, crop, 1))
            return;
        const size_t packed_bytes = static_cast<size_t>(crop) * (crop + crop / 2);
        for (auto& output : gpu_slots_)
            if (!output.create(crop, crop, 3))
                return;
        for (auto& host : nv12_host_slots_)
        {
            if (cudaHostAlloc(reinterpret_cast<void**>(&host.data), packed_bytes,
                              cudaHostAllocDefault) != cudaSuccess
                || cudaEventCreateWithFlags(&host.upload_done, cudaEventDisableTiming)
                    != cudaSuccess)
                return;
        }
        nv12_gpu_ready_ = true;
        std::cout << "[DirectShow] NV12 GPU conversion ready: " << crop << "x" << crop
                  << " (model input)" << std::endl;
    }

    GpuImage ConvertNv12Gpu(const unsigned char* pixels, int crop)
    {
        const size_t packed_bytes = static_cast<size_t>(crop) * (crop + crop / 2);
        auto& host = nv12_host_slots_[next_nv12_host_slot_++ % nv12_host_slots_.size()];
        if (host.pending)
        {
            if (cudaEventSynchronize(host.upload_done) != cudaSuccess) return {};
            host.pending = false;
        }
        std::memcpy(host.data, pixels, packed_bytes);
        if (!nv12_input_.upload(host.data, crop + crop / 2, crop, 1, crop, nv12_stream_))
            return {};
        if (cudaEventRecord(host.upload_done, nv12_stream_) != cudaSuccess)
            return {};
        host.pending = true;

        GpuImage& output = gpu_slots_[next_gpu_slot_++ % gpu_slots_.size()];
        if (!output.create(crop, crop, 3)) return {};
        launch_nv12_to_bgr_bt601_limited_u8(
            nv12_input_.data(), nv12_input_.step(),
            nv12_input_.data() + static_cast<size_t>(crop) * nv12_input_.step(),
            nv12_input_.step(), output.data(), output.step(), crop, crop, nv12_stream_);
        if (cudaGetLastError() != cudaSuccess)
            return {};
        auto ready = nv12_ready_events_.record(nv12_stream_);
        if (!ready && cudaStreamSynchronize(nv12_stream_) != cudaSuccess)
            return {};
        output.setReadyEvent(std::move(ready));
        return output;
    }

    void Open(int index, int width, int height, int fps, const std::string& format)
    {
        const auto require = [](HRESULT hr, const char* stage) {
            if (SUCCEEDED(hr)) return;
            char message[128];
            std::snprintf(message, sizeof(message), "%s failed (0x%08lX)",
                          stage, static_cast<unsigned long>(hr));
            throw std::runtime_error(message);
        };
        filter_ = FilterAtIndex(index);
        if (!filter_) throw std::runtime_error("DirectShow device index is no longer present");
        if (FAILED(CoCreateInstance(CLSID_FilterGraph, nullptr, CLSCTX_INPROC_SERVER,
                                    IID_PPV_ARGS(&graph_)))
            || FAILED(CoCreateInstance(CLSID_CaptureGraphBuilder2, nullptr, CLSCTX_INPROC_SERVER,
                                       IID_PPV_ARGS(&builder_)))
            || FAILED(builder_->SetFiltergraph(graph_.Get()))
            || FAILED(graph_->AddFilter(filter_.Get(), L"Capture device")))
            throw std::runtime_error("DirectShow graph creation failed");

        ComPtr<IAMStreamConfig> config;
        require(builder_->FindInterface(&PIN_CATEGORY_CAPTURE, &MEDIATYPE_Video,
                                        filter_.Get(), IID_PPV_ARGS(&config)),
                "FindInterface(IAMStreamConfig)");
        if (!SelectMode(config.Get(), width, height, fps, format))
            throw std::runtime_error("DirectShow refused selected video mode");

        if (FAILED(CoCreateInstance(kSampleGrabberClsid, nullptr, CLSCTX_INPROC_SERVER,
                                    IID_PPV_ARGS(&grabber_filter_)))
            || FAILED(grabber_filter_.As(&grabber_)))
            throw std::runtime_error("DirectShow Sample Grabber is unavailable");
        AM_MEDIA_TYPE wanted{};
        wanted.majortype = MEDIATYPE_Video;
        const GUID* wantedSubtype = SubtypeFor(format);
        if (!wantedSubtype) throw std::runtime_error("DirectShow pixel format is unsupported");
        wanted.subtype = *wantedSubtype;
        wanted.formattype = FORMAT_VideoInfo;
        if (FAILED(grabber_->SetMediaType(&wanted))
            || FAILED(graph_->AddFilter(grabber_filter_.Get(), L"Frame grabber"))
            || FAILED(CoCreateInstance(kNullRendererClsid, nullptr, CLSCTX_INPROC_SERVER,
                                       IID_PPV_ARGS(&null_renderer_)))
            || FAILED(graph_->AddFilter(null_renderer_.Get(), L"Null renderer")))
            throw std::runtime_error("DirectShow frame sink creation failed");
        require(builder_->RenderStream(&PIN_CATEGORY_CAPTURE, &MEDIATYPE_Video,
                                       filter_.Get(), grabber_filter_.Get(),
                                       null_renderer_.Get()), "RenderStream");
        AM_MEDIA_TYPE connected{};
        if (FAILED(grabber_->GetConnectedMediaType(&connected)))
            throw std::runtime_error("DirectShow cannot read output video mode");
        DWORD imageSize = 0;
        LONGLONG frameInterval = 0;
        if (connected.formattype == FORMAT_VideoInfo
            && connected.pbFormat && connected.cbFormat >= sizeof(VIDEOINFOHEADER))
        {
            const auto* header = reinterpret_cast<const VIDEOINFOHEADER*>(connected.pbFormat);
            width_ = std::abs(header->bmiHeader.biWidth);
            height_ = std::abs(header->bmiHeader.biHeight);
            bottom_up_ = header->bmiHeader.biHeight > 0;
            imageSize = header->bmiHeader.biSizeImage;
            frameInterval = header->AvgTimePerFrame;
        }
        if (connected.cbFormat) CoTaskMemFree(connected.pbFormat);
        if (connected.pUnk) connected.pUnk->Release();
        if (connected.subtype != *wantedSubtype || width_ <= 0 || height_ <= 0)
            throw std::runtime_error("DirectShow did not produce the selected pixel format");
        const int channels = format == "RGB32" ? 4 : format == "RGB24" ? 3
                           : format == "YUY2" ? 2 : 1;
        stride_ = format == "MJPG" ? 1 : ((width_ * channels + 3) & ~3);
        const int rows = format == "NV12" ? height_ + height_ / 2 : height_;
        if (imageSize > 0 && rows > 0 && imageSize % rows == 0
            && imageSize / rows >= static_cast<DWORD>(width_ * channels))
            stride_ = static_cast<int>(imageSize / rows);
        callback_.Attach(new FrameCallback(width_, height_, stride_, side_, format == "NV12"));
        require(grabber_->SetOneShot(FALSE), "SetOneShot");
        require(grabber_->SetBufferSamples(FALSE), "SetBufferSamples");
        require(grabber_->SetCallback(callback_.Get(), 1), "SetCallback");
        require(graph_.As(&control_), "QueryInterface(IMediaControl)");
        require(control_->Run(), "IMediaControl::Run");
        source_fps_ = frameInterval > 0
            ? static_cast<int>(std::lround(10000000.0 / frameInterval)) : fps;
        last_wait_log_ = std::chrono::steady_clock::now();
        std::cout << "[DirectShow] Opened device #" << index << " at "
                  << format_ << " " << width_ << "x" << height_
                  << " @ " << source_fps_ << " fps" << std::endl;
    }

    void Close()
    {
        if (control_) control_->Stop();
        if (grabber_) grabber_->SetCallback(nullptr, 1);
        callback_.Reset();
        control_.Reset();
        null_renderer_.Reset();
        grabber_.Reset();
        grabber_filter_.Reset();
        filter_.Reset();
        builder_.Reset();
        graph_.Reset();
        if (nv12_stream_)
            cudaStreamSynchronize(nv12_stream_);
        for (auto& host : nv12_host_slots_)
        {
            if (host.upload_done) cudaEventDestroy(host.upload_done);
            if (host.data) cudaFreeHost(host.data);
            host = {};
        }
        if (nv12_stream_)
        {
            cudaStreamDestroy(nv12_stream_);
            nv12_stream_ = nullptr;
        }
        if (com_initialized_) CoUninitialize();
        com_initialized_ = false;
    }

    ComPtr<IGraphBuilder> graph_;
    ComPtr<ICaptureGraphBuilder2> builder_;
    ComPtr<IBaseFilter> filter_;
    ComPtr<IBaseFilter> grabber_filter_;
    ComPtr<IBaseFilter> null_renderer_;
    ComPtr<ISampleGrabber> grabber_;
    ComPtr<ISampleGrabberCB> callback_;
    ComPtr<IMediaControl> control_;
    int side_ = 1;
    std::string format_;
    int target_fps_ = 0;
    int source_fps_ = 0;
    int width_ = 0;
    int height_ = 0;
    int stride_ = 0;
    int64_t last_capture_ns_ = 0;
    std::vector<BYTE> frame_pixels_;
    cv::Mat converted_;
    std::array<GpuImage, 8> gpu_slots_;
    size_t next_gpu_slot_ = 0;
    cudaStream_t nv12_stream_ = nullptr;
    bool nv12_gpu_ready_ = false;
    GpuImage nv12_input_;
    GpuReadyEventPool<8> nv12_ready_events_;
    std::array<PinnedNv12Slot, 8> nv12_host_slots_{};
    size_t next_nv12_host_slot_ = 0;
    bool bottom_up_ = false;
    bool com_initialized_ = false;
    bool first_frame_logged_ = false;
    bool bad_frame_logged_ = false;
    bool upload_error_logged_ = false;
    std::chrono::steady_clock::time_point last_output_{};
    std::chrono::steady_clock::time_point last_wait_log_{};
};
}

std::vector<Device> EnumerateDevices()
{
    std::vector<Device> result;
    std::thread worker([&] { result = EnumerateOnStaThread(); });
    worker.join();
    return result;
}

const Device* FindByName(const std::vector<Device>& devices, const std::string& name)
{
    for (const auto& device : devices)
        if (device.name == name) return &device;
    const auto normalized = [](std::string value) {
        std::transform(value.begin(), value.end(), value.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        const size_t last = value.find_last_not_of(" \t");
        if (last == std::string::npos) return std::string();
        value.resize(last + 1);
        size_t start = value.size();
        while (start > 0 && std::isdigit(static_cast<unsigned char>(value[start - 1])))
            --start;
        if (start > 0 && start < value.size() && value[start - 1] == ' ')
            value.resize(start - 1);
        return value;
    };
    const std::string wanted = normalized(name);
    const Device* match = nullptr;
    for (const auto& device : devices)
    {
        if (normalized(device.name) != wanted) continue;
        if (match) return nullptr;
        match = &device;
    }
    return match;
}

std::unique_ptr<IScreenCapture> Create(int index, int width, int height,
                                       int fps, const std::string& format,
                                       int output_side)
{
    return std::make_unique<Capture>(index, width, height, fps, format, output_side);
}
}
