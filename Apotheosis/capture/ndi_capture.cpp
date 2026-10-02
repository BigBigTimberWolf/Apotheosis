#include "ndi_capture.h"
#include "ndi/ndi_runtime.h"
#include "ndi/latest_slot.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <iostream>
#include <thread>

namespace ndi_capture {
namespace {
int64_t nowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
struct Frame { cv::Mat image; int64_t receivedNs = 0; };
class Receiver final : public IScreenCapture {
public:
    Receiver(std::shared_ptr<ndi::Runtime> rt, std::string name, int side)
        : rt_(std::move(rt)), name_(std::move(name)), side_(std::clamp(side, 32, 2048)) {
        NDIlib_find_create_t find;
        finder_ = rt_->find_create_v2(&find);
        NDIlib_recv_create_v3_t settings;
        settings.color_format = NDIlib_recv_color_format_BGRX_BGRA;
        settings.bandwidth = NDIlib_recv_bandwidth_highest;
        settings.allow_video_fields = false;
        settings.p_ndi_recv_name = "Apotheosis Receiver";
        receiver_ = rt_->recv_create_v3(&settings);
        if (!finder_ || !receiver_) {
            if (finder_) rt_->find_destroy(finder_);
            if (receiver_) rt_->recv_destroy(receiver_);
            throw std::runtime_error("Cannot create NDI finder/receiver");
        }
        try { worker_ = std::thread([this] { run(); }); }
        catch (...) { rt_->recv_destroy(receiver_); rt_->find_destroy(finder_); throw; }
    }
    ~Receiver() override {
        stop_ = true; frames_.close();
        if (worker_.joinable()) worker_.join();
        rt_->recv_destroy(receiver_); rt_->find_destroy(finder_);
    }
    cv::Mat GetNextFrameCpu() override {
        auto frame = frames_.take();
        if (!frame) return {};
        deliveredNs_ = frame->receivedNs;
        return std::move(frame->image);
    }
    bool WaitFrame(int ms) override { return frames_.wait(std::max(ms, 0)); }
    bool SupportsEventWait() const override { return true; }
    bool HandlesTargetFps() const override { return true; }
    int GetSourceFpsEstimate() const override { return fps_; }
    int64_t GetLastFrameCaptureNs() const override { return deliveredNs_; }
    bool HasStopped() const override { return failed_; }
private:
    void connectSelected() {
        uint32_t count = 0;
        const auto* sources = rt_->find_get_current_sources(finder_, &count);
        for (uint32_t i = 0; i < count; ++i) {
            if (sources[i].p_ndi_name && name_ == sources[i].p_ndi_name) {
                rt_->recv_connect(receiver_, &sources[i]);
                return;
            }
        }
    }
    cv::Mat convert(const NDIlib_video_frame_v2_t& v) const {
        if (!v.p_data || v.xres <= 0 || v.yres <= 0 || v.xres > 16384 || v.yres > 16384 ||
            v.line_stride_in_bytes < int64_t(v.xres) * 4 ||
            (v.FourCC != NDIlib_FourCC_video_type_BGRX && v.FourCC != NDIlib_FourCC_video_type_BGRA)) return {};
        const int crop = std::min({v.xres, v.yres, side_});
        cv::Mat image(v.yres, v.xres, CV_8UC4, v.p_data, v.line_stride_in_bytes);
        cv::Mat bgr;
        cv::cvtColor(image(cv::Rect((v.xres-crop)/2, (v.yres-crop)/2, crop, crop)), bgr, cv::COLOR_BGRA2BGR);
        if (crop != side_) cv::resize(bgr, bgr, cv::Size(side_, side_));
        return bgr;
    }
    void run() noexcept {
        try {
            auto retry = std::chrono::steady_clock::time_point{};
            auto lastVideo = std::chrono::steady_clock::now();
            auto fpsStart = lastVideo;
            int count = 0;
            while (!stop_) {
                const auto now = std::chrono::steady_clock::now();
                if (now >= retry && rt_->recv_get_no_connections(receiver_) == 0) {
                    connectSelected(); retry = now + std::chrono::seconds(3);
                }
                NDIlib_video_frame_v2_t video;
                const auto type = rt_->recv_capture_v3(receiver_, &video, nullptr, nullptr, 20);
                if (type == NDIlib_frame_type_error) {
                    rt_->recv_connect(receiver_, nullptr); fps_ = 0;
                    frames_.take();
                    std::this_thread::sleep_for(std::chrono::milliseconds(20));
                    continue;
                }
                if (type != NDIlib_frame_type_video) {
                    if (now - lastVideo > std::chrono::seconds(1)) fps_ = 0;
                    continue;
                }
                int64_t received = nowNs();
                // Dequeue stale frames before any conversion. Bound the drain
                // so continuous high-rate sources cannot starve delivery/stop.
                for (int n = 0; n < 16 && !stop_; ++n) {
                    NDIlib_video_frame_v2_t newer;
                    if (rt_->recv_capture_v3(receiver_, &newer, nullptr, nullptr, 0) != NDIlib_frame_type_video) break;
                    rt_->recv_free_video_v2(receiver_, &video);
                    video = newer; received = nowNs();
                }
                struct FreeVideo {
                    Receiver* self; NDIlib_video_frame_v2_t* video;
                    ~FreeVideo() { self->rt_->recv_free_video_v2(self->receiver_, video); }
                } free{this, &video};
                auto bgr = convert(video);
                if (bgr.empty()) continue;
                frames_.publish(Frame{std::move(bgr), received});
                lastVideo = std::chrono::steady_clock::now();
                ++count;
                const double elapsed = std::chrono::duration<double>(lastVideo - fpsStart).count();
                if (elapsed >= 1) { fps_ = int(count / elapsed); count = 0; fpsStart = lastVideo; }
            }
        } catch (const std::exception& e) {
            std::cerr << "[NDI] Receiver failed: " << e.what() << std::endl;
            failed_ = true; fps_ = 0; frames_.close();
        }
    }
    std::shared_ptr<ndi::Runtime> rt_;
    std::string name_;
    int side_;
    NDIlib_find_instance_t finder_ = nullptr;
    NDIlib_recv_instance_t receiver_ = nullptr;
    ndi::LatestSlot<Frame> frames_;
    std::atomic<bool> stop_{false}, failed_{false};
    std::atomic<int> fps_{0};
    std::atomic<int64_t> deliveredNs_{0};
    std::thread worker_;
};
}
std::vector<std::string> Discover(int timeoutMs, std::string& error) {
    auto rt = ndi::Runtime::Load(error);
    if (!rt) return {};
    NDIlib_find_create_t settings;
    auto finder = rt->find_create_v2(&settings);
    if (!finder) { error = "Cannot create NDI finder"; return {}; }
    struct Cleanup { std::shared_ptr<ndi::Runtime> rt; NDIlib_find_instance_t finder;
        ~Cleanup() { rt->find_destroy(finder); } } cleanup{rt, finder};
    // An early discovery event need not contain all senders; allow the full
    // bounded discovery interval, while never blocking the GUI thread.
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(std::clamp(timeoutMs, 0, 5000));
    while (std::chrono::steady_clock::now() < end) rt->find_wait_for_sources(finder, 100);
    uint32_t count = 0;
    const auto* sources = rt->find_get_current_sources(finder, &count);
    std::vector<std::string> names;
    for (uint32_t i=0; i<count; ++i) if (sources[i].p_ndi_name) names.emplace_back(sources[i].p_ndi_name);
    std::sort(names.begin(), names.end());
    return names;
}
std::unique_ptr<IScreenCapture> Create(const std::string& name, int side) {
    if (name.empty()) { std::cerr << "[NDI] Select a source in capture settings." << std::endl; return {}; }
    std::string error;
    auto rt = ndi::Runtime::Load(error);
    if (!rt) { std::cerr << "[NDI] " << error << std::endl; return {}; }
    return std::make_unique<Receiver>(std::move(rt), name, side);
}
}
