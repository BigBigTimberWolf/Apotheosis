#include "magewell_capture.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <utility>

#ifdef APOTHEOSIS_HAS_MAGEWELL_SDK
#include <LibMWCapture/MWCapture.h>
#endif

namespace magewell
{

bool IsDeviceKey(const std::string& key)
{
    return key.rfind("magewell:", 0) == 0;
}

#ifdef APOTHEOSIS_HAS_MAGEWELL_SDK
namespace
{

// Keep the runtime loaded for the process lifetime: the SDK owns a monitoring thread.
bool Ready()
{
    static const bool ready = [] {
        if (!LoadLibraryW(L"LibMWCapture.dll"))
            return false;
        return MWCaptureInitInstance() != FALSE;
    }();
    return ready;
}

std::string KeyForPath(const WCHAR* path)
{
    // The SDK's device path is stable across enumeration order changes. Persist
    // its hash so INI files never need to contain a Windows device path.
    uint64_t hash = UINT64_C(14695981039346656037);
    for (const WCHAR* p = path; *p; ++p)
    {
        const uint16_t code = static_cast<uint16_t>(*p);
        hash = (hash ^ static_cast<uint8_t>(code)) * UINT64_C(1099511628211);
        hash = (hash ^ static_cast<uint8_t>(code >> 8)) * UINT64_C(1099511628211);
    }
    std::ostringstream out;
    out << "magewell:" << std::hex << std::setw(16) << std::setfill('0') << hash;
    return out.str();
}

struct Channel
{
    Device device;
    std::wstring path;
};

std::vector<Channel> Channels()
{
    std::vector<Channel> out;
    if (!Ready()) return out;
    MWRefreshDevice();
    const int count = MWGetChannelCount();
    for (int i = 0; i < count; ++i)
    {
        MWCAP_CHANNEL_INFO info{};
        if (MWGetChannelInfoByIndex(i, &info) != MW_SUCCEEDED
            || info.wFamilyID != MW_FAMILY_ID_PRO_CAPTURE)
            continue;
        WCHAR path[128]{};
        if (MWGetDevicePath(i, path) != MW_SUCCEEDED || !path[0])
            continue;

        Channel item;
        item.path = path;
        item.device.key = KeyForPath(path);
        item.device.name = std::string("Magewell Pro Capture SDK: ") + info.szProductName;
        if (info.byChannelIndex > 0)
            item.device.name += " #" + std::to_string(info.byChannelIndex + 1);

        HCHANNEL handle = MWOpenChannelByPath(path);
        if (handle)
        {
            MWCAP_VIDEO_SIGNAL_STATUS signal{};
            if (MWGetVideoSignalStatus(handle, &signal) == MW_SUCCEEDED
                && signal.state == MWCAP_VIDEO_SIGNAL_LOCKED)
            {
                item.device.signal_width = signal.cx;
                item.device.signal_height = signal.cy;
                if (signal.dwFrameDuration > 0)
                    item.device.signal_fps = static_cast<int>(
                        (signal.bInterlaced ? 20000000.0 : 10000000.0)
                        / signal.dwFrameDuration + 0.5);
            }
            MWCloseChannel(handle);
        }
        out.push_back(std::move(item));
    }
    return out;
}

int64_t NowNs()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

class Capture final : public IScreenCapture
{
public:
    Capture(const std::wstring& path, int side, int fps)
        : side_(std::max(1, side)), target_fps_(std::max(0, fps))
    {
        handle_ = MWOpenChannelByPath(path.c_str());
        if (!handle_) throw std::runtime_error("Magewell SDK could not open selected channel");
        capture_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        notify_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!capture_event_ || !notify_event_)
        {
            Close();
            throw std::runtime_error("Magewell SDK event creation failed");
        }
        if (MWStartVideoCapture(handle_, capture_event_) != MW_SUCCEEDED)
        {
            Close();
            throw std::runtime_error("Magewell SDK video capture failed to start");
        }
        started_ = true;
        notify_ = MWRegisterNotify(handle_, notify_event_,
                                   MWCAP_NOTIFY_VIDEO_FRAME_BUFFERED
                                   | MWCAP_NOTIFY_VIDEO_SIGNAL_CHANGE);
        if (!notify_)
        {
            Close();
            throw std::runtime_error("Magewell SDK frame notification failed");
        }
        stride_ = FOURCC_CalcMinStride(MWFOURCC_BGR24, side_, 2);
        const DWORD size = FOURCC_CalcImageSize(MWFOURCC_BGR24, side_, side_, stride_);
        if (stride_ < static_cast<DWORD>(side_ * 3) || size == 0)
        {
            Close();
            throw std::runtime_error("Magewell SDK output size is invalid");
        }
        pixels_.resize(size);
        if (MWPinVideoBuffer(handle_, pixels_.data(), size) != MW_SUCCEEDED)
        {
            Close();
            throw std::runtime_error("Magewell SDK could not pin output buffer");
        }
        pinned_ = true;
    }

    ~Capture() override { Close(); }

    cv::Mat GetNextFrameCpu() override { return {}; }

    GpuImage GetNextFrameGpu() override
    {
        if (failed_) return {};
        if (WaitForSingleObject(notify_event_, 4) != WAIT_OBJECT_0)
            return {};
        ULONGLONG bits = 0;
        if (MWGetNotifyStatus(handle_, notify_, &bits) != MW_SUCCEEDED)
            return {};
        if (bits & MWCAP_NOTIFY_VIDEO_SIGNAL_CHANGE)
            signal_valid_ = false;
        if (!(bits & MWCAP_NOTIFY_VIDEO_FRAME_BUFFERED))
            return {};
        const int64_t frame_event_ns = NowNs();

        if (!signal_valid_)
        {
            if (MWGetVideoSignalStatus(handle_, &signal_) != MW_SUCCEEDED
                || signal_.state != MWCAP_VIDEO_SIGNAL_LOCKED
                || signal_.cx <= 0 || signal_.cy <= 0)
                return {};
            signal_valid_ = true;
            if (signal_.dwFrameDuration > 0)
                source_fps_ = static_cast<int>(
                    (signal_.bInterlaced ? 20000000.0 : 10000000.0)
                    / signal_.dwFrameDuration + 0.5);
        }

        const auto now = std::chrono::steady_clock::now();
        if (target_fps_ > 0 && last_output_.time_since_epoch().count() != 0
            && now - last_output_ < std::chrono::microseconds(1000000 / target_fps_))
            return {};

        const int crop_side = std::min(signal_.cx, signal_.cy);
        const int left = (signal_.cx - crop_side) / 2;
        const int top = (signal_.cy - crop_side) / 2;
        RECT source{left, top, left + crop_side, top + crop_side};
        ResetEvent(capture_event_);
        const auto result = MWCaptureVideoFrameToVirtualAddressEx(
            handle_, MWCAP_VIDEO_FRAME_ID_NEWEST_BUFFERED,
            pixels_.data(), static_cast<DWORD>(pixels_.size()), stride_, FALSE,
            nullptr, MWFOURCC_BGR24, side_, side_, 0, 0, nullptr, nullptr,
            0, 100, 0, 100, 0, MWCAP_VIDEO_DEINTERLACE_WEAVE,
            MWCAP_VIDEO_ASPECT_RATIO_IGNORE, &source, nullptr,
            0, 0, MWCAP_VIDEO_COLOR_FORMAT_UNKNOWN,
            MWCAP_VIDEO_QUANTIZATION_UNKNOWN,
            MWCAP_VIDEO_SATURATION_UNKNOWN);
        if (result != MW_SUCCEEDED)
            return {};
        if (WaitForSingleObject(capture_event_, 100) != WAIT_OBJECT_0)
        {
            failed_ = true;
            return {};
        }
        MWCAP_VIDEO_CAPTURE_STATUS status{};
        if (MWGetVideoCaptureStatus(handle_, &status) != MW_SUCCEEDED
            || !status.bFrameCompleted)
            return {};

        GpuImage frame;
        if (!frame.upload(pixels_.data(), side_, side_, 3, stride_))
            return {};
        last_output_ = now;
        last_capture_ns_ = frame_event_ns;
        return frame;
    }

    int GetSourceFpsEstimate() const override { return source_fps_; }
    int64_t GetLastFrameCaptureNs() const override { return last_capture_ns_; }
    bool HasStopped() const override { return failed_; }
    void SetTargetFps(int fps) override { target_fps_ = std::max(0, fps); }
    bool HandlesTargetFps() const override { return true; }

private:
    void Close()
    {
        if (notify_) MWUnregisterNotify(handle_, notify_);
        notify_ = nullptr;
        if (started_) MWStopVideoCapture(handle_);
        started_ = false;
        if (pinned_) MWUnpinVideoBuffer(handle_, pixels_.data());
        pinned_ = false;
        if (notify_event_) CloseHandle(notify_event_);
        notify_event_ = nullptr;
        if (capture_event_) CloseHandle(capture_event_);
        capture_event_ = nullptr;
        if (handle_) MWCloseChannel(handle_);
        handle_ = nullptr;
    }

    HCHANNEL handle_ = nullptr;
    HNOTIFY notify_ = nullptr;
    HANDLE capture_event_ = nullptr;
    HANDLE notify_event_ = nullptr;
    bool started_ = false;
    bool pinned_ = false;
    bool failed_ = false;
    bool signal_valid_ = false;
    MWCAP_VIDEO_SIGNAL_STATUS signal_{};
    int side_ = 0;
    int target_fps_ = 0;
    int source_fps_ = 0;
    int64_t last_capture_ns_ = 0;
    DWORD stride_ = 0;
    std::vector<uint8_t> pixels_;
    std::chrono::steady_clock::time_point last_output_{};
};

} // namespace
#endif

bool SdkEnabled()
{
#ifdef APOTHEOSIS_HAS_MAGEWELL_SDK
    return Ready();
#else
    return false;
#endif
}

std::vector<Device> EnumerateDevices()
{
#ifdef APOTHEOSIS_HAS_MAGEWELL_SDK
    std::vector<Device> out;
    for (auto& channel : Channels()) out.push_back(std::move(channel.device));
    return out;
#else
    return {};
#endif
}

std::unique_ptr<IScreenCapture> Create(const std::string& key, int output_side,
                                       int target_fps)
{
#ifdef APOTHEOSIS_HAS_MAGEWELL_SDK
    if (!IsDeviceKey(key) || !Ready()) return nullptr;
    for (const auto& channel : Channels())
        if (channel.device.key == key)
            return std::make_unique<Capture>(channel.path, output_side, target_fps);
#else
    (void)key; (void)output_side; (void)target_fps;
#endif
    return nullptr;
}

} // namespace magewell
