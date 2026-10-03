#include "dxgi_capture.h"

#include "ndi/latest_slot.h" // the latest-frame slot is generic despite its namespace

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <iostream>
#include <iterator>
#include <thread>

#ifdef _WIN32

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <dxgi1_5.h>
#include <wrl/client.h>

namespace dxgi_capture {
namespace {

using Microsoft::WRL::ComPtr;

int64_t nowNs()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

std::string toUtf8(const wchar_t* wide)
{
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
    if (bytes <= 1) return {};
    std::string out(static_cast<size_t>(bytes - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide, -1, out.data(), bytes, nullptr, nullptr);
    return out;
}

std::string hrText(const char* what, HRESULT hr)
{
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "0x%08lX", static_cast<unsigned long>(hr));
    return std::string(what) + " failed (" + buffer + ")";
}

// A monitor together with the COM objects needed to duplicate it.
struct OutputHandle
{
    ComPtr<IDXGIAdapter1> adapter;
    ComPtr<IDXGIOutput> output;
    OutputInfo info;
};

std::vector<OutputHandle> enumerateHandles(std::string& error)
{
    std::vector<OutputHandle> list;
    ComPtr<IDXGIFactory1> factory;
    HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(factory.GetAddressOf()));
    if (FAILED(hr)) { error = hrText("CreateDXGIFactory1", hr); return list; }
    for (UINT a = 0;; ++a) {
        ComPtr<IDXGIAdapter1> adapter;
        if (factory->EnumAdapters1(a, adapter.GetAddressOf()) == DXGI_ERROR_NOT_FOUND) break;
        DXGI_ADAPTER_DESC1 adapterDesc{};
        adapter->GetDesc1(&adapterDesc);
        if (adapterDesc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue; // no software renderer
        for (UINT o = 0;; ++o) {
            ComPtr<IDXGIOutput> output;
            if (adapter->EnumOutputs(o, output.GetAddressOf()) == DXGI_ERROR_NOT_FOUND) break;
            DXGI_OUTPUT_DESC desc{};
            if (FAILED(output->GetDesc(&desc)) || !desc.AttachedToDesktop) continue;
            OutputHandle handle;
            handle.adapter = adapter;
            handle.output = output;
            handle.info.deviceName = toUtf8(desc.DeviceName);
            // The monitor's real current mode, independent of this process's DPI awareness.
            DEVMODEW mode{};
            mode.dmSize = sizeof(mode);
            if (EnumDisplaySettingsW(desc.DeviceName, ENUM_CURRENT_SETTINGS, &mode)) {
                handle.info.width = static_cast<int>(mode.dmPelsWidth);
                handle.info.height = static_cast<int>(mode.dmPelsHeight);
            } else {
                handle.info.width = desc.DesktopCoordinates.right - desc.DesktopCoordinates.left;
                handle.info.height = desc.DesktopCoordinates.bottom - desc.DesktopCoordinates.top;
            }
            MONITORINFO monitor{};
            monitor.cbSize = sizeof(monitor);
            handle.info.primary = GetMonitorInfoW(desc.Monitor, &monitor) && (monitor.dwFlags & MONITORINFOF_PRIMARY);
            handle.info.rotated = desc.Rotation == DXGI_MODE_ROTATION_ROTATE90 ||
                                  desc.Rotation == DXGI_MODE_ROTATION_ROTATE180 ||
                                  desc.Rotation == DXGI_MODE_ROTATION_ROTATE270;
            list.push_back(std::move(handle));
        }
    }
    return list;
}

// Everything tied to one successful duplication; dropped and rebuilt after a loss.
struct Session
{
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<IDXGIOutputDuplication> duplication;
    ComPtr<ID3D11Texture2D> staging; // the cropped region, readable by the CPU
    bool inSystemMemory = false;     // the desktop image is already in RAM
    int width = 0, height = 0;       // monitor size at open time
    CropGeometry crop;
    std::string name;
};

enum class OpenResult { Ok, Retry, Fatal };

OpenResult open(const std::string& wanted, int side, Session& session, std::string& message,
                bool& fellBack)
{
    std::string error;
    auto handles = enumerateHandles(error);
    std::vector<OutputInfo> infos;
    for (const auto& h : handles) infos.push_back(h.info);
    const OutputChoice choice = chooseOutput(infos, wanted);
    if (choice.index < 0) {
        message = error.empty() ? "no monitor is attached to the desktop right now" : error;
        return OpenResult::Retry; // a locked or disconnected session can come back
    }
    fellBack = choice.fellBack;
    OutputHandle& handle = handles[static_cast<size_t>(choice.index)];
    if (handle.info.rotated) {
        message = "monitor " + handle.info.deviceName + " is rotated; rotated monitors are not supported";
        return OpenResult::Fatal;
    }

    // The duplication must be created on the device of the GPU the monitor hangs off.
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
    HRESULT hr = D3D11CreateDevice(handle.adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, static_cast<UINT>(std::size(levels)), D3D11_SDK_VERSION,
        session.device.GetAddressOf(), nullptr, session.context.GetAddressOf());
    if (FAILED(hr)) { message = hrText("D3D11CreateDevice", hr); return OpenResult::Retry; }

    hr = E_FAIL;
    ComPtr<IDXGIOutput5> output5;
    if (SUCCEEDED(handle.output.As(&output5))) {
        // Asking for BGRA8 lets newer Windows convert HDR desktops for us.
        const DXGI_FORMAT formats[] = {DXGI_FORMAT_B8G8R8A8_UNORM};
        hr = output5->DuplicateOutput1(session.device.Get(), 0, static_cast<UINT>(std::size(formats)), formats,
                                       session.duplication.ReleaseAndGetAddressOf());
    }
    if (FAILED(hr)) {
        ComPtr<IDXGIOutput1> output1;
        hr = handle.output.As(&output1);
        if (SUCCEEDED(hr))
            hr = output1->DuplicateOutput(session.device.Get(), session.duplication.ReleaseAndGetAddressOf());
    }
    if (FAILED(hr)) {
        message = hrText("DuplicateOutput", hr);
        if (hr == DXGI_ERROR_NOT_CURRENTLY_AVAILABLE)
            message += ": too many applications are already duplicating the desktop";
        else if (hr == E_ACCESSDENIED)
            message += ": access denied (secure desktop or lock screen?)";
        else if (hr == DXGI_ERROR_UNSUPPORTED)
            message += ": desktop duplication is not supported here (remote session?)";
        const bool fatal = hr == DXGI_ERROR_UNSUPPORTED || hr == E_INVALIDARG;
        return fatal ? OpenResult::Fatal : OpenResult::Retry;
    }

    DXGI_OUTDUPL_DESC desc{};
    session.duplication->GetDesc(&desc);
    if (desc.ModeDesc.Format != DXGI_FORMAT_B8G8R8A8_UNORM) {
        message = "the desktop format is not BGRA8 (HDR display?); it is not supported";
        return OpenResult::Fatal;
    }
    session.width = static_cast<int>(desc.ModeDesc.Width);
    session.height = static_cast<int>(desc.ModeDesc.Height);
    session.inSystemMemory = desc.DesktopImageInSystemMemory != FALSE;
    session.crop = centerCrop(session.width, session.height, side);
    session.name = handle.info.deviceName;
    if (session.crop.size <= 0) { message = "the monitor reports an empty size"; return OpenResult::Retry; }

    if (!session.inSystemMemory) {
        D3D11_TEXTURE2D_DESC staging{};
        staging.Width = staging.Height = static_cast<UINT>(session.crop.size);
        staging.MipLevels = staging.ArraySize = 1;
        staging.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        staging.SampleDesc.Count = 1;
        staging.Usage = D3D11_USAGE_STAGING;
        staging.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        hr = session.device->CreateTexture2D(&staging, nullptr, session.staging.GetAddressOf());
        if (FAILED(hr)) { message = hrText("CreateTexture2D", hr); return OpenResult::Retry; }
    }
    return OpenResult::Ok;
}

struct Frame
{
    cv::Mat image;
    int64_t presentedNs = 0;
};

struct PumpResult
{
    Failure why = Failure::Timeout;
    bool delivered = false; // at least one frame arrived during this session
};

class DesktopCapture final : public IScreenCapture
{
public:
    DesktopCapture(std::string wanted, int side)
        : wanted_(std::move(wanted)), side_(std::clamp(side, 32, 2048))
    {
        LARGE_INTEGER frequency{};
        QueryPerformanceFrequency(&frequency);
        frequency_ = frequency.QuadPart;
        worker_ = std::thread([this] { run(); });
    }
    ~DesktopCapture() override
    {
        stop_ = true;
        frames_.close();
        if (worker_.joinable()) worker_.join();
    }

    cv::Mat GetNextFrameCpu() override
    {
        auto frame = frames_.take();
        if (!frame) return {};
        deliveredNs_ = frame->presentedNs;
        ageUs_ = ageUs(nowNs(), frame->presentedNs);
        return std::move(frame->image);
    }
    bool WaitFrame(int ms) override { return frames_.wait(std::max(ms, 0)); }
    bool SupportsEventWait() const override { return true; }
    int GetSourceFpsEstimate() const override { return fps_; }
    int64_t GetLastFrameCaptureNs() const override { return deliveredNs_; }
    int GetDeviceFrameAgeUs() const override { return ageUs_; }
    bool HasStopped() const override { return failed_; }

private:
    void pause(int ms)
    {
        for (int waited = 0; waited < ms && !stop_; waited += 20)
            std::this_thread::sleep_for(std::chrono::milliseconds(std::min(20, ms - waited)));
    }

    // Copies the centred square of the acquired desktop image into a BGR matrix and
    // gives the image back to DXGI. Returns an empty matrix when the copy failed.
    cv::Mat grab(Session& s, IDXGIResource* resource)
    {
        const int size = s.crop.size;
        cv::Mat bgr(size, size, CV_8UC3);
        bool ok = false;
        if (s.inSystemMemory) {
            DXGI_MAPPED_RECT rect{};
            if (SUCCEEDED(s.duplication->MapDesktopSurface(&rect))) {
                const uint8_t* origin = rect.pBits + static_cast<size_t>(s.crop.y) * rect.Pitch +
                                        static_cast<size_t>(s.crop.x) * 4;
                ok = bgraToBgr(origin, static_cast<size_t>(rect.Pitch), size, size, bgr.data, bgr.step);
                s.duplication->UnMapDesktopSurface();
            }
            s.duplication->ReleaseFrame();
        } else {
            ComPtr<ID3D11Texture2D> texture;
            bool queued = false;
            if (SUCCEEDED(resource->QueryInterface(IID_PPV_ARGS(texture.GetAddressOf())))) {
                D3D11_TEXTURE2D_DESC desc{};
                texture->GetDesc(&desc);
                if (static_cast<int>(desc.Width) == s.width && static_cast<int>(desc.Height) == s.height) {
                    const D3D11_BOX box{static_cast<UINT>(s.crop.x), static_cast<UINT>(s.crop.y), 0,
                                        static_cast<UINT>(s.crop.x + size), static_cast<UINT>(s.crop.y + size), 1};
                    s.context->CopySubresourceRegion(s.staging.Get(), 0, 0, 0, 0, texture.Get(), 0, &box);
                    queued = true;
                }
            }
            // The copy is queued on the GPU, so the desktop image can go back right away.
            s.duplication->ReleaseFrame();
            D3D11_MAPPED_SUBRESOURCE mapped{};
            if (queued && SUCCEEDED(s.context->Map(s.staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
                ok = bgraToBgr(static_cast<const uint8_t*>(mapped.pData), mapped.RowPitch, size, size,
                               bgr.data, bgr.step);
                s.context->Unmap(s.staging.Get(), 0);
            }
        }
        if (!ok) return {};
        if (s.crop.scale) cv::resize(bgr, bgr, cv::Size(s.crop.side, s.crop.side), 0, 0, cv::INTER_LINEAR);
        return bgr;
    }

    PumpResult pump(Session& s)
    {
        PumpResult result;
        while (!stop_) {
            DXGI_OUTDUPL_FRAME_INFO info{};
            ComPtr<IDXGIResource> resource;
            const HRESULT hr = s.duplication->AcquireNextFrame(8, &info, resource.GetAddressOf());
            if (hr == DXGI_ERROR_WAIT_TIMEOUT) { // the screen did not change
                meter_.idle(std::chrono::steady_clock::now());
                fps_ = meter_.fps();
                continue;
            }
            if (hr == DXGI_ERROR_ACCESS_LOST || hr == DXGI_ERROR_DEVICE_REMOVED ||
                hr == DXGI_ERROR_DEVICE_RESET || hr == DXGI_ERROR_SESSION_DISCONNECTED) {
                result.why = Failure::AccessLost;
                return result;
            }
            if (FAILED(hr)) { result.why = Failure::Unavailable; return result; }
            if (info.LastPresentTime.QuadPart == 0) { // only the pointer moved
                s.duplication->ReleaseFrame();
                continue;
            }
            cv::Mat image = grab(s, resource.Get());
            if (image.empty()) { result.why = Failure::AccessLost; return result; }
            const int64_t presented = ticksToNs(info.LastPresentTime.QuadPart, frequency_);
            const int64_t now = nowNs();
            frames_.publish(Frame{std::move(image), presented > 0 && presented <= now ? presented : now});
            fps_ = meter_.tick(std::chrono::steady_clock::now());
            result.delivered = true;
        }
        return result;
    }

    void run() noexcept
    {
        try {
            int consecutive = 0;
            bool reportedFallback = false, reportedOpen = false;
            while (!stop_) {
                Session session;
                std::string message;
                bool fellBack = false;
                const OpenResult opened = open(wanted_, side_, session, message, fellBack);
                Failure why = Failure::Unavailable;
                if (opened == OpenResult::Fatal) {
                    std::cerr << "[DXGI] " << message << std::endl;
                    failed_ = true;
                    break;
                }
                if (opened == OpenResult::Ok) {
                    if (fellBack && !reportedFallback) {
                        std::cerr << "[DXGI] The selected monitor is not available; capturing the primary monitor."
                                  << std::endl;
                        reportedFallback = true;
                    }
                    if (!reportedOpen) {
                        std::cout << "[DXGI] Capturing " << session.name << " " << session.width << "x"
                                  << session.height << ", centre " << session.crop.size << "px" << std::endl;
                        reportedOpen = true;
                    }
                    const PumpResult pumped = pump(session);
                    if (pumped.delivered) consecutive = 0;
                    why = pumped.why;
                    fps_ = 0;
                    meter_ = FpsMeter{};
                } else if (!message.empty() && consecutive == 0) {
                    std::cerr << "[DXGI] " << message << "; retrying" << std::endl;
                }
                if (stop_) break;
                ++consecutive;
                const RecoveryDecision decision = decideRecovery(why, consecutive);
                if (decision.giveUp) {
                    std::cerr << "[DXGI] Desktop capture keeps failing; giving up." << std::endl;
                    failed_ = true;
                    break;
                }
                pause(decision.delayMs);
            }
        } catch (const std::exception& e) {
            std::cerr << "[DXGI] Capture failed: " << e.what() << std::endl;
            failed_ = true;
        }
        if (failed_) { fps_ = 0; frames_.close(); }
    }

    std::string wanted_;
    int side_;
    int64_t frequency_ = 0;
    ndi::LatestSlot<Frame> frames_;
    FpsMeter meter_; // touched by the worker thread only
    std::atomic<bool> stop_{false}, failed_{false};
    std::atomic<int> fps_{0};
    std::atomic<int> ageUs_{-1};
    std::atomic<int64_t> deliveredNs_{0};
    std::thread worker_;
};

} // namespace

std::vector<OutputInfo> EnumerateOutputs(std::string& error)
{
    std::vector<OutputInfo> infos;
    for (const auto& handle : enumerateHandles(error)) infos.push_back(handle.info);
    return infos;
}

std::unique_ptr<IScreenCapture> Create(const std::string& outputName, int outputSide)
{
    try {
        return std::make_unique<DesktopCapture>(outputName, outputSide);
    } catch (const std::exception& e) {
        std::cerr << "[DXGI] Cannot start desktop capture: " << e.what() << std::endl;
        return {};
    }
}

} // namespace dxgi_capture

#else // _WIN32

namespace dxgi_capture {
std::vector<OutputInfo> EnumerateOutputs(std::string& error)
{
    error = "DXGI desktop capture is only available on Windows";
    return {};
}
std::unique_ptr<IScreenCapture> Create(const std::string&, int)
{
    std::cerr << "[DXGI] Desktop capture is only available on Windows." << std::endl;
    return {};
}
} // namespace dxgi_capture

#endif
