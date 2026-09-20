#include "crosshair_runtime.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <mutex>
#include <optional>
#include <vector>

#include "Apotheosis.h"
#include "config.h"
#include "runtime/config_snapshot.h"
#include "runtime/active_hotkey.h"
#include "capture/gpu_color_ops.h"
#include "mem/gpu_image.h"

#include <cuda_runtime.h>

namespace crosshair_runtime
{

namespace
{

std::mutex g_mtx;
PivotSnapshot g_snap{};
PivotSnapshot g_static_ref{};

crosshair::CrosshairDetector  g_detector;

constexpr int kMaxGpuBands = 16;

struct GpuDetectorState
{
    cudaStream_t stream = nullptr;
    GpuHsvBand* device_bands = nullptr;
    int* device_result = nullptr;
    int* host_result = nullptr;
    bool initialized = false;

    bool ensure()
    {
        if (initialized) return true;
        if (cudaStreamCreateWithFlags(&stream, cudaStreamNonBlocking) != cudaSuccess)
            return false;
        if (cudaMalloc(reinterpret_cast<void**>(&device_bands),
                       sizeof(GpuHsvBand) * kMaxGpuBands) != cudaSuccess
            || cudaMalloc(reinterpret_cast<void**>(&device_result), sizeof(int) * 4) != cudaSuccess
            || cudaMallocHost(reinterpret_cast<void**>(&host_result), sizeof(int) * 4) != cudaSuccess)
            return false;
        initialized = true;
        return true;
    }
};

GpuDetectorState& gpu_state()
{
    static GpuDetectorState* state = new GpuDetectorState();
    return *state;
}

bool target_gate_open()
{
    std::lock_guard<std::mutex> lk(detectionBuffer.mutex);
    return !detectionBuffer.boxes.empty() && !detectionBuffer.staleLocked();
}

}

PivotSnapshot read()
{
    std::lock_guard<std::mutex> lk(g_mtx);
    return g_snap;
}

void publish(const PivotSnapshot& snap)
{
    std::lock_guard<std::mutex> lk(g_mtx);
    if (snap.ts.time_since_epoch().count() && g_snap.ts.time_since_epoch().count() && snap.ts < g_snap.ts) return;
    g_snap = snap;
}

PivotSnapshot read_static_ref()
{
    std::lock_guard<std::mutex> lk(g_mtx);
    return g_static_ref;
}

void publish_static_ref(const PivotSnapshot& ref)
{
    std::lock_guard<std::mutex> lk(g_mtx);
    g_static_ref = ref;
}

void process_frame(const cv::Mat& bgrFrame, int64_t captured_ns)
{
    if (bgrFrame.empty() || bgrFrame.type() != CV_8UC3)
    {
        publish(PivotSnapshot{});
        return;
    }

    const int active_idx = runtime::g_active_hotkey_index.load();
    if (active_idx < 0)
    {
        publish(PivotSnapshot{});
        return;
    }

    {
        std::lock_guard<std::mutex> lk(detectionBuffer.mutex);
        if (detectionBuffer.boxes.empty())
        {
            publish(PivotSnapshot{});
            return;
        }

        if (detectionBuffer.staleLocked())
        {
            publish(PivotSnapshot{});
            return;
        }
    }

    bool cross_enabled = false;
    bool cross_has_color = false;

    crosshair::CrosshairDetectorSettings cross_settings;

    {
        const auto snapshot = runtime_config::read();
        const auto& cfg = *snapshot;
        if (active_idx >= static_cast<int>(cfg.hotkeys.size()))
        {
            publish(PivotSnapshot{});
            return;
        }
        const auto& hk = cfg.hotkeys[active_idx];
        cross_enabled = hk.crosshair_detect_enabled;
        if (!cross_enabled)
        {
            publish(PivotSnapshot{});
            return;
        }

        cross_settings.enabled         = true;
        cross_settings.rect_w          = cfg.crosshair_rect_w;
        cross_settings.rect_h          = cfg.crosshair_rect_h;
        cross_settings.min_pixel_count = cfg.crosshair_min_pixel_count;
        cross_settings.close_radius    = cfg.crosshair_close_radius;
        cross_settings.colors.reserve(cfg.crosshair_colors.size());
        for (const auto& c : cfg.crosshair_colors)
        {
            crosshair::CrosshairColorBand b;
            b.name = c.name; b.enabled = c.enabled;
            b.h_low = c.h_low; b.h_high = c.h_high;
            b.s_min = c.s_min; b.s_max = c.s_max;
            b.v_min = c.v_min; b.v_max = c.v_max;
            cross_has_color = cross_has_color || b.enabled;
            cross_settings.colors.push_back(std::move(b));
        }
    }

    std::optional<cv::Point2f> hit;
    if (cross_enabled && cross_has_color)
        hit = g_detector.detect(bgrFrame, cross_settings);

    PivotSnapshot snap;
    snap.ts = captured_ns > 0 ? std::chrono::steady_clock::time_point(std::chrono::nanoseconds(captured_ns))
                              : std::chrono::steady_clock::now();
    static int s_lost_frames = 0;
    static cv::Point2f s_last_valid_hit(0, 0);

    if (hit)
    {
        snap.x = static_cast<double>(hit->x);
        snap.y = static_cast<double>(hit->y);
        snap.valid = true;
        s_last_valid_hit = *hit;
        s_lost_frames = 0;
    }
    else
    {
        if (s_lost_frames < 3 && s_last_valid_hit.x > 1.0f)
        {
            snap.x = static_cast<double>(s_last_valid_hit.x);
            snap.y = static_cast<double>(s_last_valid_hit.y);
            snap.valid = true;
            s_lost_frames++;
        }
        else
        {
            snap.valid = false;
        }
    }
    publish(snap);
}

bool gpu_path_active()
{
    const int active_idx = runtime::g_active_hotkey_index.load();
    if (active_idx < 0) return false;
    const auto snapshot = runtime_config::read();
    if (active_idx >= static_cast<int>(snapshot->hotkeys.size())) return false;
    return snapshot->hotkeys[active_idx].crosshair_detect_enabled;
}

bool cpu_path_active()
{
    return false;
}

void process_gpu_frame(const GpuImage& frame)
{
    if (frame.empty() || frame.channels() != 3 || !gpu_path_active()
        || !target_gate_open())
    {
        publish(PivotSnapshot{});
        return;
    }

    const int active_idx = runtime::g_active_hotkey_index.load();
    const auto snapshot = runtime_config::read();
    if (active_idx < 0 || active_idx >= static_cast<int>(snapshot->hotkeys.size()))
    {
        publish(PivotSnapshot{});
        return;
    }

    std::vector<GpuHsvBand> bands;
    bands.reserve(std::min<size_t>(snapshot->crosshair_colors.size(), kMaxGpuBands));
    for (const auto& c : snapshot->crosshair_colors)
    {
        if (!c.enabled || bands.size() >= kMaxGpuBands) continue;
        bands.push_back(GpuHsvBand{
            std::clamp(c.h_low, 0, 179), std::clamp(c.h_high, 0, 179),
            std::clamp(c.s_min, 0, 255), std::clamp(c.s_max, 0, 255),
            std::clamp(c.v_min, 0, 255), std::clamp(c.v_max, 0, 255) });
    }
    if (bands.empty())
    {
        publish(PivotSnapshot{});
        return;
    }

    const int roi_w = std::min(frame.cols(), std::max(4, snapshot->crosshair_rect_w));
    const int roi_h = std::min(frame.rows(), std::max(4, snapshot->crosshair_rect_h));
    const int roi_x = std::clamp(frame.cols() / 2 - roi_w / 2, 0, frame.cols() - roi_w);
    const int roi_y = std::clamp(frame.rows() / 2 - roi_h + 10, 0, frame.rows() - roi_h);

    auto& state = gpu_state();
    if (!state.ensure())
    {
        publish(PivotSnapshot{});
        return;
    }
    if (frame.readyEvent())
        cudaStreamWaitEvent(state.stream, frame.readyEvent(), 0);
    if (cudaMemcpyAsync(state.device_bands, bands.data(),
                        bands.size() * sizeof(GpuHsvBand), cudaMemcpyHostToDevice,
                        state.stream) != cudaSuccess
        || cudaMemsetAsync(state.device_result, 0, sizeof(int) * 4, state.stream) != cudaSuccess)
    {
        publish(PivotSnapshot{});
        return;
    }

    launch_crosshair_hsv_reduce_bgr_u8(
        frame.data(), frame.step(), frame.cols(), frame.rows(),
        roi_x, roi_y, roi_w, roi_h,
        state.device_bands, static_cast<int>(bands.size()),
        state.device_result, state.stream);
    if (cudaGetLastError() != cudaSuccess
        || cudaMemcpyAsync(state.host_result, state.device_result, sizeof(int) * 4,
                           cudaMemcpyDeviceToHost, state.stream) != cudaSuccess
        || cudaStreamSynchronize(state.stream) != cudaSuccess)
    {
        publish(PivotSnapshot{});
        return;
    }

    PivotSnapshot out;
    out.ts = frame.captureNs() > 0 ? std::chrono::steady_clock::time_point(std::chrono::nanoseconds(frame.captureNs()))
                                  : std::chrono::steady_clock::now();
    const int count = state.host_result[1];
    if (count >= std::max(1, snapshot->crosshair_min_pixel_count))
    {
        cv::Point2f hit(
            static_cast<float>(state.host_result[2]) / static_cast<float>(count),
            static_cast<float>(state.host_result[3]) / static_cast<float>(count));
        const float roi_left   = static_cast<float>(roi_x);
        const float roi_top    = static_cast<float>(roi_y);
        const float roi_right  = roi_left + static_cast<float>(roi_w);
        const float roi_bottom = roi_top + static_cast<float>(roi_h);
        if (hit.x >= roi_left && hit.x <= roi_right
            && hit.y >= roi_top && hit.y <= roi_bottom)
        {
            out.x = hit.x;
            out.y = hit.y;
            out.valid = true;
        }
    }
    if (!out.valid)
    {
        const PivotSnapshot previous = read();
        if (previous.valid
            && out.ts - previous.ts <= std::chrono::milliseconds(kFreshnessMs))
        {
            publish(previous);
            return;
        }
    }
    publish(out);
}

}
