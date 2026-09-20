#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <iostream>
#include <atomic>
#include <thread>
#include <mutex>
#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <timeapi.h>
#include <condition_variable>
#include <filesystem>
#include <memory>
#include <optional>
#include <queue>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "capture.h"
#include "crosshair/crosshair_runtime.h"
#include "tensorrt/nvinf.h"
#include "Apotheosis.h"
#include "keycodes.h"
#include "keyboard_listener.h"
#include "other_tools.h"
#include "mf_capture.h"
#include "capture_card_probe.h"
#include "runtime/active_hotkey.h"
#include "runtime/latency_probe.h"
#include "gpu_color_ops.h"
#include <cuda_runtime.h>
#include "capture_utils.h"

extern std::atomic<bool> detector_model_changed;

cv::Mat latestFrame;
std::mutex frameMutex;

int screenWidth = 0;
int screenHeight = 0;

std::atomic<int> captureFrameCount(0);
std::atomic<int> captureFps(0);
std::chrono::time_point<std::chrono::high_resolution_clock> captureFpsStartTime;

std::atomic<int> captureSourceFps(0);
std::atomic<int> captureSourceFrameCount(0);

std::atomic<int> captureSenderSpanFps(0);
std::atomic<int> captureWireLostFps(0);
std::atomic<int> capturePartialLostFps(0);
std::atomic<int> capturePcapKernelDroppedFps(0);
std::atomic<int> capturePcapIfDroppedFps(0);
std::chrono::time_point<std::chrono::high_resolution_clock> captureSourceFpsStartTime;

std::deque<cv::Mat> frameQueue;

namespace
{

struct CaptureThreadConfig
{
    std::string capture_device;
    std::string capture_format;
    int  capture_width  = 0;
    int  capture_height = 0;
    int  capture_fps    = 0;
    bool capture_gpu_decode = true;
    int  detection_resolution = 0;
    bool circle_mask = false;
    std::string backend;
    std::vector<std::string> screenshot_button;
    int  screenshot_delay = 0;
    bool show_window = false;
    bool verbose = false;

};

CaptureThreadConfig SnapshotCaptureConfig()
{
    std::lock_guard<std::recursive_mutex> cfgLock(configMutex);
    CaptureThreadConfig snapshot;
    snapshot.capture_device = config.capture_device;
    snapshot.capture_format = config.capture_format;
    snapshot.capture_width  = config.capture_width;
    snapshot.capture_height = config.capture_height;
    snapshot.capture_fps    = config.capture_fps;
    snapshot.capture_gpu_decode = config.capture_gpu_decode;
    snapshot.detection_resolution = config.detection_resolution;
    snapshot.circle_mask = config.circle_mask;
    snapshot.backend = config.backend;
    snapshot.screenshot_button = config.screenshot_button;
    snapshot.screenshot_delay = config.screenshot_delay;
    snapshot.show_window = config.show_window;
    snapshot.verbose = config.verbose;

    return snapshot;
}

class TimerResolutionGuard
{
public:
    void Enable()
    {
        if (!enabled_)
        {
            timeBeginPeriod(1);
            enabled_ = true;
        }
    }

    void Disable()
    {
        if (enabled_)
        {
            timeEndPeriod(1);
            enabled_ = false;
        }
    }

    ~TimerResolutionGuard()
    {
        Disable();
    }

private:
    bool enabled_{ false };
};

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

class PreciseSleeper
{
public:
    PreciseSleeper()
    {
        timer_ = CreateWaitableTimerExW(
            nullptr, nullptr,
            CREATE_WAITABLE_TIMER_HIGH_RESOLUTION | CREATE_WAITABLE_TIMER_MANUAL_RESET,
            TIMER_ALL_ACCESS);
        if (!timer_)
        {
            timer_ = CreateWaitableTimerExW(
                nullptr, nullptr,
                CREATE_WAITABLE_TIMER_MANUAL_RESET,
                TIMER_ALL_ACCESS);
        }
    }

    ~PreciseSleeper()
    {
        if (timer_) CloseHandle(timer_);
    }

    PreciseSleeper(const PreciseSleeper&) = delete;
    PreciseSleeper& operator=(const PreciseSleeper&) = delete;

    bool sleep_until(std::chrono::steady_clock::time_point tp)
    {
        if (!timer_) return false;
        auto now = std::chrono::steady_clock::now();
        if (now >= tp) return true;
        const long long ns =
            std::chrono::duration_cast<std::chrono::nanoseconds>(tp - now).count();
        LARGE_INTEGER due;
        due.QuadPart = -(ns / 100);
        if (due.QuadPart == 0) due.QuadPart = -1;
        if (!SetWaitableTimer(timer_, &due, 0, nullptr, nullptr, FALSE))
            return false;
        WaitForSingleObject(timer_, INFINITE);
        return true;
    }

private:
    HANDLE timer_{ nullptr };
};

class ScreenshotWriter
{
public:
    ScreenshotWriter()
    {
        writerThread_ = std::thread([this]() { Run(); });
    }

    ~ScreenshotWriter()
    {
        Stop();
    }

    void Enqueue(const std::string& filename, cv::Mat frame)
    {
        if (filename.empty() || frame.empty())
            return;

        std::lock_guard<std::mutex> lock(mutex_);
        if (queue_.size() >= maxPendingFrames_)
            queue_.pop();
        queue_.emplace(filename, std::move(frame));
        cv_.notify_one();
    }

private:
    void Stop()
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stop_ = true;
        }
        cv_.notify_one();

        if (writerThread_.joinable())
            writerThread_.join();
    }

    void Run()
    {
        std::error_code ec;
        std::filesystem::create_directories("screenshots", ec);

        while (true)
        {
            std::pair<std::string, cv::Mat> job;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                cv_.wait(lock, [this]() { return stop_ || !queue_.empty(); });
                if (stop_ && queue_.empty())
                    break;

                job = std::move(queue_.front());
                queue_.pop();
            }

            try
            {
                const std::filesystem::path outputPath = std::filesystem::path("screenshots") / job.first;
                cv::imwrite(outputPath.string(), job.second);
            }
            catch (const std::exception& e)
            {
                std::cerr << "[Capture] Screenshot save failed: " << e.what() << std::endl;
            }
            catch (...)
            {
                std::cerr << "[Capture] Screenshot save failed: unknown exception." << std::endl;
            }
        }
    }

private:
    static constexpr size_t maxPendingFrames_ = 8;

    std::mutex mutex_;
    std::condition_variable cv_;
    std::queue<std::pair<std::string, cv::Mat>> queue_;
    std::thread writerThread_;
    bool stop_{ false };
};

class HostCopyWorker
{
public:
    HostCopyWorker()
    {
        thread_ = std::thread([this]() { Run(); });
    }

    ~HostCopyWorker()
    {
        Stop();
    }

    void Submit(GpuImage gpu, bool needCrosshair)
    {
        if (gpu.empty()) return;
        {
            std::lock_guard<std::mutex> lk(mutex_);
            if (stop_) return;
            pending_ = std::move(gpu);
            pendingCrosshair_ = needCrosshair;
            hasPending_ = true;
        }
        cv_.notify_one();
    }

private:
    void Stop()
    {
        {
            std::lock_guard<std::mutex> lk(mutex_);
            stop_ = true;
        }
        cv_.notify_one();
        if (thread_.joinable())
            thread_.join();
    }

    void Run()
    {
        while (true)
        {
            GpuImage gpu;
            bool needCrosshair = false;
            {
                std::unique_lock<std::mutex> lk(mutex_);
                cv_.wait(lk, [this]() { return stop_ || hasPending_; });
                if (stop_ && !hasPending_) break;
                gpu = std::move(pending_);
                pending_.release();
                needCrosshair = pendingCrosshair_;
                hasPending_ = false;
            }
            if (gpu.empty()) continue;

            try
            {
                cv::Mat host;
                gpu.download(host);
                const auto capture_ns = gpu.captureNs();
                gpu.release();

                if (host.empty()) continue;

                {
                    std::lock_guard<std::mutex> lk(frameMutex);
                    latestFrame = host;
                    if (frameQueue.size() >= 1)
                        frameQueue.pop_front();
                    frameQueue.push_back(latestFrame);
                }
                frameCV.notify_one();

                if (needCrosshair)
                    crosshair_runtime::process_frame(host, capture_ns);
            }
            catch (const std::exception& e)
            {
                std::cerr << "[HostCopyWorker] " << e.what() << std::endl;
            }
            catch (...)
            {
                std::cerr << "[HostCopyWorker] unknown exception" << std::endl;
            }
        }
    }

    std::mutex mutex_;
    std::condition_variable cv_;
    GpuImage pending_;
    bool pendingCrosshair_{ false };
    bool hasPending_{ false };
    bool stop_{ false };
    std::thread thread_;
};

class GpuCrosshairWorker
{
public:
    GpuCrosshairWorker() : thread_([this]() { Run(); }) {}
    ~GpuCrosshairWorker() { Stop(); }

    void Submit(GpuImage gpu)
    {
        if (gpu.empty()) return;
        {
            std::lock_guard<std::mutex> lk(mutex_);
            if (stop_) return;
            pending_ = std::move(gpu);
            hasPending_ = true;
        }
        cv_.notify_one();
    }

private:
    void Stop()
    {
        {
            std::lock_guard<std::mutex> lk(mutex_);
            stop_ = true;
        }
        cv_.notify_one();
        if (thread_.joinable()) thread_.join();
    }

    void Run()
    {
        while (true)
        {
            GpuImage gpu;
            {
                std::unique_lock<std::mutex> lk(mutex_);
                cv_.wait(lk, [this]() { return stop_ || hasPending_; });
                if (stop_ && !hasPending_) break;
                gpu = std::move(pending_);
                pending_.release();
                hasPending_ = false;
            }
            if (!gpu.empty())
                crosshair_runtime::process_gpu_frame(gpu);
        }
    }

    std::mutex mutex_;
    std::condition_variable cv_;
    GpuImage pending_;
    bool hasPending_{ false };
    bool stop_{ false };
    std::thread thread_;
};
}

std::vector<cv::Mat> getBatchFromQueue(int batch_size)
{
    std::vector<cv::Mat> batch;
    std::lock_guard<std::mutex> lk(frameMutex);
    const size_t target_size = (batch_size > 0) ? static_cast<size_t>(batch_size) : 0;
    const size_t n = std::min(frameQueue.size(), target_size);

    for (size_t i = 0; i < n; ++i)
        batch.push_back(frameQueue[frameQueue.size() - n + i]);

    while (batch.size() < target_size && !batch.empty())
        batch.push_back(batch.back().clone());
    return batch;
}

void captureThread(int CAPTURE_WIDTH, int CAPTURE_HEIGHT)
{
    try
    {
        detection_resolution_changed.exchange(false);
        capture_method_changed.exchange(false);
        CaptureThreadConfig currentCfg = SnapshotCaptureConfig();
        if (currentCfg.verbose)
            std::cout << "[Capture] OpenCV version: " << CV_VERSION << std::endl;

        int captureWidth = std::max(1, CAPTURE_WIDTH);
        int captureHeight = std::max(1, CAPTURE_HEIGHT);
        if (currentCfg.detection_resolution > 0)
        {
            captureWidth = currentCfg.detection_resolution;
            captureHeight = currentCfg.detection_resolution;
        }

        auto createCapturer = [&](const CaptureThreadConfig& cfg, int width, int height) -> std::unique_ptr<IScreenCapture>
        {
            (void)width; (void)height;
            try
            {
                const bool crop_enabled = true;
                const int  out_side = std::max(1, cfg.detection_resolution);

                const auto devices = MFCapture::EnumerateDevices();
                int device_index = -1;
                for (const auto& d : devices)
                    if (d.friendly_name == cfg.capture_device) { device_index = d.index; break; }

                if (device_index < 0)
                {
                    std::cerr << "[Capture] Selected capture card \"" << cfg.capture_device
                              << "\" is NOT present (" << devices.size()
                              << " device(s) found). No substitution is performed; "
                                 "open the capture settings and pick a connected card."
                              << std::endl;
                    return nullptr;
                }

                if (cfg.verbose)
                    std::cout << "[Capture] Capture card: " << cfg.capture_device
                              << " | " << cfg.capture_format
                              << " " << cfg.capture_width << "x" << cfg.capture_height
                              << "@" << cfg.capture_fps << "fps"
                              << " | crop " << out_side << "x" << out_side
                              << " | " << (cfg.capture_gpu_decode ? "GPU" : "CPU") << std::endl;

                return std::make_unique<MFCapture>(
                    cfg.capture_width,
                    cfg.capture_height,
                    out_side,
                    crop_enabled,
                    cfg.capture_fps,
                    cfg.capture_format,
                    device_index,
                    cfg.capture_gpu_decode);
            }
            catch (const std::exception& e)
            {
                std::cerr << "[Capture] Failed to initialize capture card: " << e.what() << std::endl;
                return nullptr;
            }
        };

        std::unique_ptr<IScreenCapture> capturer = createCapturer(currentCfg, captureWidth, captureHeight);
        if (capturer)
            capturer->SetTargetFps(currentCfg.capture_fps);
        auto lastCapturerCreateAttempt = std::chrono::steady_clock::now();

        auto clearCaptureFrames = [&]()
        {
            std::lock_guard<std::mutex> lock(frameMutex);
            latestFrame.release();
            frameQueue.clear();
        };

        auto clearDetections = [&]()
        {
            std::lock_guard<std::mutex> lock(detectionBuffer.mutex);
            detectionBuffer.boxes.clear();
            detectionBuffer.precise_boxes.clear();
            detectionBuffer.classes.clear();
            detectionBuffer.confidences.clear();
            detectionBuffer.bumpVersionLocked();
            detectionBuffer.cv.notify_all();
        };

        auto markCaptureUnavailable = [&]()
        {
            clearCaptureFrames();
            clearDetections();
            captureFps.store(0);
            captureSourceFps.store(0);
            runtime::latency::noteDeviceFrameAgeUs(-1);
            frameCV.notify_one();
        };

        bool captureUnavailable = false;
        auto setCaptureUnavailable = [&]()
        {
            if (captureUnavailable)
                return;
            captureUnavailable = true;
            markCaptureUnavailable();
        };
        auto setCaptureAvailable = [&]()
        {
            captureUnavailable = false;
        };

        setCaptureUnavailable();

        TimerResolutionGuard timerResolution;
        PreciseSleeper preciseSleeper;
        std::optional<std::chrono::steady_clock::duration> frameDuration;
        bool eventSourceAboveLimit = false;
        auto updateFrameDuration = [&](int captureFpsSetting)
        {
            eventSourceAboveLimit = false;
            if (captureFpsSetting > 0)
            {
                timerResolution.Enable();
                const auto frameMs = std::chrono::duration<double, std::milli>(1000.0 / captureFpsSetting);
                frameDuration = std::chrono::duration_cast<std::chrono::steady_clock::duration>(frameMs);
            }
            else
            {
                timerResolution.Disable();
                frameDuration.reset();
            }
        };
        updateFrameDuration(currentCfg.capture_fps);

        captureFpsStartTime = std::chrono::high_resolution_clock::now();
        captureSourceFpsStartTime = captureFpsStartTime;

        auto frameStartTime = std::chrono::steady_clock::now();
        auto applyFrameLimiter = [&]()
        {
            if (frameDuration.has_value())
            {
                const auto target = frameStartTime + frameDuration.value();
                const auto now = std::chrono::steady_clock::now();
                if (now < target)
                {
                    constexpr auto kSpinMargin = std::chrono::microseconds(200);
                    if (target - now > kSpinMargin)
                    {
                        if (!preciseSleeper.sleep_until(target - kSpinMargin))
                        {
                            std::this_thread::sleep_until(target - kSpinMargin);
                        }
                    }
                    while (std::chrono::steady_clock::now() < target)
                    {
                    }
                }
                frameStartTime = target;
                const auto post = std::chrono::steady_clock::now();
                if (post - frameStartTime > frameDuration.value() * 4)
                    frameStartTime = post;
            }
            else
            {
                frameStartTime = std::chrono::steady_clock::now();
            }
        };

        ScreenshotWriter screenshotWriter;
        auto lastSaveTime = std::chrono::steady_clock::now();
        auto lastSuccessfulFrameTime = std::chrono::steady_clock::now();
        constexpr auto staleFrameTimeout = std::chrono::milliseconds(500);

        auto lastPreviewSubmitTime = std::chrono::steady_clock::now();
        bool needFirstPreviewSubmit = true;
        constexpr auto kPreviewSubmitInterval = std::chrono::microseconds(16000);

        struct CaptureCudaGuard
        {
            cudaStream_t stream{ nullptr };
            GpuReadyEventPool<8> maskEvents;
            CaptureCudaGuard()
            {
                cudaStreamCreateWithFlags(&stream, cudaStreamNonBlocking);
            }
            ~CaptureCudaGuard()
            {
                if (stream) cudaStreamDestroy(stream);
            }
        };
        CaptureCudaGuard captureCuda;

        HostCopyWorker hostCopyWorker;
        GpuCrosshairWorker gpuCrosshairWorker;

        while (!shouldExit && !session_stop_requested.load())
        {
            try
            {
                const bool resolutionChanged = detection_resolution_changed.exchange(false);
                const bool captureChanged = capture_method_changed.exchange(false);
                const bool fpsChanged = capture_fps_changed.exchange(false);
                currentCfg = SnapshotCaptureConfig();

            if (fpsChanged)
            {
                updateFrameDuration(currentCfg.capture_fps);
                if (capturer) capturer->SetTargetFps(currentCfg.capture_fps);
            }

            const bool needsReinit = resolutionChanged || captureChanged;

            if (needsReinit)
            {
                eventSourceAboveLimit = false;
                setCaptureUnavailable();

                if (currentCfg.detection_resolution > 0)
                {
                    captureWidth = currentCfg.detection_resolution;
                    captureHeight = currentCfg.detection_resolution;
                }

                capturer.reset();
                capturer = createCapturer(currentCfg, captureWidth, captureHeight);
                if (capturer)
                    capturer->SetTargetFps(currentCfg.capture_fps);
                lastCapturerCreateAttempt = std::chrono::steady_clock::now();
                if (currentCfg.verbose)
                    std::cout << "[Capture] Reinitialized capture backend." << std::endl;
            }

            if (capturer && capturer->HasStopped())
            {
                capturer.reset();
                setCaptureUnavailable();
            }
            if (!capturer)
            {
                const auto now = std::chrono::steady_clock::now();
                if (now - lastCapturerCreateAttempt >= std::chrono::seconds(1))
                {
                    capturer = createCapturer(currentCfg, captureWidth, captureHeight);
                    if (capturer)
                        capturer->SetTargetFps(currentCfg.capture_fps);
                    lastCapturerCreateAttempt = now;

                    if (capturer)
                    {
                        lastSuccessfulFrameTime = now;
                        if (currentCfg.verbose)
                            std::cout << "[Capture] Capture backend recovered." << std::endl;
                    }
                }

                setCaptureUnavailable();
                if (!frameDuration.has_value())
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                applyFrameLimiter();
                continue;
            }

            const bool screenshotEnabled =
                !currentCfg.screenshot_button.empty() && currentCfg.screenshot_button[0] != "None";
            const auto screenshotNow = std::chrono::steady_clock::now();
            const auto screenshotElapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                screenshotNow - lastSaveTime
            ).count();
            const bool screenshotRequested =
                screenshotEnabled &&
                isAnyKeyPressed(currentCfg.screenshot_button) &&
                screenshotElapsedMs >= currentCfg.screenshot_delay;

            cv::Mat screenshotCpu;
            cv::Mat detectionFrame;

            GpuImage screenshotGpu = capturer->GetNextFrameGpu();
            screenshotGpu.setCaptureNs(capturer->GetLastFrameCaptureNs());
            bool freshCpuFrameThisIter = false;
            bool gpuMaskApplied = false;

            if (!screenshotGpu.empty())
                runtime::latency::noteCaptureForStats(runtime::latency::markCapture(capturer->GetLastFrameCaptureNs()));

            if (currentCfg.circle_mask && !screenshotGpu.empty())
            {
                cudaEvent_t srcEvent = screenshotGpu.readyEvent();
                if (srcEvent)
                    cudaStreamWaitEvent(captureCuda.stream, srcEvent, 0);
                launch_circle_mask_bgr_u8(
                    screenshotGpu.data(), screenshotGpu.step(),
                    screenshotGpu.cols(), screenshotGpu.rows(),
                    captureCuda.stream);
                auto maskEvent = captureCuda.maskEvents.record(captureCuda.stream);
                if (!maskEvent && cudaStreamSynchronize(captureCuda.stream) != cudaSuccess)
                    throw std::runtime_error("CUDA mask completion failed");
                screenshotGpu.setReadyEvent(std::move(maskEvent));
                gpuMaskApplied = true;
            }

            const bool detectorNeedsCpu = !g_detector
                || g_detector->backend() != DetectorBackend::TensorRT;
            const bool needCpuStrict = screenshotRequested
                || detectorNeedsCpu;
            const bool gpuCrosshairActive = crosshair_runtime::gpu_path_active();
            const bool cpuColourActive = crosshair_runtime::cpu_path_active();
            const bool needCpuAsync = currentCfg.show_window || cpuColourActive;

            if (!screenshotGpu.empty())
            {
                if (gpuCrosshairActive)
                    gpuCrosshairWorker.Submit(screenshotGpu);

                if (needCpuStrict)
                {
                    screenshotGpu.download(screenshotCpu);
                    freshCpuFrameThisIter = true;
                }
                if (needCpuAsync)
                {
                    const auto now = std::chrono::steady_clock::now();
                    auto interval = kPreviewSubmitInterval;
                    if (cpuColourActive)
                        interval = std::chrono::microseconds(8000);
                    if (needFirstPreviewSubmit
                        || now - lastPreviewSubmitTime >= interval)
                    {
                        hostCopyWorker.Submit(screenshotGpu, cpuColourActive);
                        lastPreviewSubmitTime = now;
                        needFirstPreviewSubmit = false;
                    }
                }
            }
            else
            {
                screenshotCpu = capturer->GetNextFrameCpu();
                freshCpuFrameThisIter = !screenshotCpu.empty();
                if (freshCpuFrameThisIter)
                    runtime::latency::noteCaptureForStats(runtime::latency::markCapture(capturer->GetLastFrameCaptureNs()));
            }

            if (screenshotGpu.empty() && screenshotCpu.empty())
            {
                const auto now = std::chrono::steady_clock::now();
                if (now - lastSuccessfulFrameTime >= staleFrameTimeout)
                    setCaptureUnavailable();

                if (capturer->SupportsEventWait()) capturer->WaitFrame(4);
                else std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }

            if (currentCfg.circle_mask && !gpuMaskApplied && !screenshotCpu.empty())
                screenshotCpu = apply_circle_mask(screenshotCpu);

            if (screenshotGpu.empty() && (gpuCrosshairActive || cpuColourActive)
                && !screenshotCpu.empty())
                crosshair_runtime::process_frame(screenshotCpu, capturer->GetLastFrameCaptureNs());

            detectionFrame = screenshotCpu;

            if (g_detector)
            {
                const bool usedCpuDetectionOverride = false;
                const bool detectorAcceptsGpu =
                    g_detector->backend() == DetectorBackend::TensorRT;
                if (!screenshotGpu.empty() && !usedCpuDetectionOverride && detectorAcceptsGpu)
                {
                    const runtime::FrameContext context{runtime::latency::loadCaptureSeq(), capturer->GetLastFrameCaptureNs(),
                                                        screenshotGpu.cols(), screenshotGpu.rows()};
                    g_detector->processFrameGpu(std::move(screenshotGpu), context);
                }
                else
                {
                    if (detectionFrame.empty() && !screenshotGpu.empty())
                    {
                        screenshotGpu.download(detectionFrame);
                    }
                    if (!detectionFrame.empty())
                        g_detector->processFrame(detectionFrame, {runtime::latency::loadCaptureSeq(), capturer->GetLastFrameCaptureNs(),
                                                                detectionFrame.cols, detectionFrame.rows});
                }
            }

            lastSuccessfulFrameTime = std::chrono::steady_clock::now();
            setCaptureAvailable();

            if (freshCpuFrameThisIter && !screenshotCpu.empty())
            {
                {
                    std::lock_guard<std::mutex> lock(frameMutex);
                    latestFrame = screenshotCpu;
                    if (frameQueue.size() >= 1)
                        frameQueue.pop_front();
                    frameQueue.push_back(latestFrame);
                }
                frameCV.notify_one();
            }

            if (screenshotRequested && !screenshotCpu.empty())
            {
                cv::Mat saveMat = screenshotCpu.clone();
                if (!saveMat.empty())
                {
                    auto epoch_time = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::system_clock::now().time_since_epoch()
                    ).count();
                    std::string filename = std::to_string(epoch_time) + ".jpg";
                    screenshotWriter.Enqueue(filename, std::move(saveMat));
                    lastSaveTime = screenshotNow;
                }
            }

            captureFrameCount++;
            captureSourceFrameCount++;
            auto currentTime = std::chrono::high_resolution_clock::now();
            std::chrono::duration<double> elapsedTime = currentTime - captureFpsStartTime;
            if (elapsedTime.count() >= 1.0)
            {
                captureFps = static_cast<int>(captureFrameCount / elapsedTime.count());
                captureFrameCount = 0;
                captureFpsStartTime = currentTime;
            }

            std::chrono::duration<double> sourceElapsed = currentTime - captureSourceFpsStartTime;
            if (sourceElapsed.count() >= 1.0)
            {
                const int producerFps = capturer ? capturer->GetSourceFpsEstimate() : 0;
                captureSourceFps = producerFps > 0
                    ? producerFps
                    : static_cast<int>(captureSourceFrameCount / sourceElapsed.count());
                captureSourceFrameCount = 0;
                captureSourceFpsStartTime = currentTime;

                if (capturer)
                {
                    runtime::latency::noteDeviceFrameAgeUs(capturer->GetDeviceFrameAgeUs());
                    captureSenderSpanFps.store(capturer->GetSenderSpanFps());
                    captureWireLostFps.store(capturer->GetWireLostFps());
                    capturePartialLostFps.store(capturer->GetPartialLostFps());
                    capturePcapKernelDroppedFps.store(capturer->GetPcapKernelDroppedFps());
                    capturePcapIfDroppedFps.store(capturer->GetPcapIfDroppedFps());
                }
                else
                {
                    runtime::latency::noteDeviceFrameAgeUs(-1);
                    captureSenderSpanFps.store(0);
                    captureWireLostFps.store(0);
                    capturePartialLostFps.store(0);
                    capturePcapKernelDroppedFps.store(0);
                    capturePcapIfDroppedFps.store(0);
                }
            }

                bool shouldLimit = !capturer->SupportsEventWait();
                if (!shouldLimit
                    && !capturer->HandlesTargetFps()
                    && frameDuration.has_value()
                    && currentCfg.capture_fps > 0)
                {
                    int sourceEstimate = capturer->GetSourceFpsEstimate();
                    if (sourceEstimate <= 0)
                        sourceEstimate = captureSourceFps.load();
                    const int tolerance = std::max(2, currentCfg.capture_fps / 100);
                    if (sourceEstimate > currentCfg.capture_fps + tolerance)
                        eventSourceAboveLimit = true;
                    shouldLimit = eventSourceAboveLimit;
                }

                if (shouldLimit)
                    applyFrameLimiter();
                else
                    frameStartTime = std::chrono::steady_clock::now();
            }
            catch (const std::exception& e)
            {
                std::cerr << "[Capture] Loop exception: " << e.what() << std::endl;
                capturer.reset();
                setCaptureUnavailable();
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            catch (...)
            {
                std::cerr << "[Capture] Loop exception: unknown." << std::endl;
                capturer.reset();
                setCaptureUnavailable();
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
        }
    }
    catch (const std::exception& e)
    {
        std::cerr << "[Capture] Unhandled exception: " << e.what() << std::endl;
        throw;
    }
}
