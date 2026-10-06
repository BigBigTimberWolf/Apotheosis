#include "crosshair_runtime.h"
#include "am_centroid.h"
#include "centroid_cluster.h"
#include "color_lab.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <mutex>
#include <optional>
#include <vector>

#include "Apotheosis.h"
#include "config.h"
#include "laser_detector.h"
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
crosshair::LaserDetector g_laser_detector;

struct LaserSmoother {
    double x = 0.0, y = 0.0, dx = 0.0, dy = 0.0, last = -1.0;
    void reset() { last = -1.0; }
    cv::Point2f filter(cv::Point2f p, double t, double strength) {
        if (last < 0.0 || t - last > 0.15) {
            x = p.x; y = p.y; dx = dy = 0.0; last = t; return p;
        }
        const double dt = std::clamp(t - last, 1.0 / 1000.0, 0.1);
        last = t;
        const auto alpha = [dt](double cutoff) {
            return 1.0 / (1.0 + 1.0 / (2.0 * 3.14159265358979323846 * cutoff * dt));
        };
        const double da = alpha(1.0);
        dx += da * ((p.x - x) / dt - dx);
        dy += da * ((p.y - y) / dt - dy);
        const double cutoff = std::max(0.5, (1.0 - strength) * 8.0) + 0.04 * std::hypot(dx, dy);
        const double a = alpha(cutoff);
        x += a * (p.x - x); y += a * (p.y - y);
        return {static_cast<float>(x), static_cast<float>(y)};
    }
};
LaserSmoother g_laser_smoother;
std::mutex g_laser_mutex;
int g_laser_hotkey = -1;
int g_last_gpu_hotkey = -1;

// 与实验室共用同一个上限常量，避免两边各写一个数字。
constexpr int kMaxGpuBands = crosshair::kMaxColorBands;

struct GpuDetectorState
{
    cudaStream_t stream = nullptr;
    GpuHsvBand* device_bands = nullptr;
    int* device_result = nullptr;
    unsigned long long* device_key = nullptr;
    unsigned char* device_mask = nullptr;
    unsigned char* device_scratch = nullptr;
    int* device_labels = nullptr;
    crosshair::CentroidComponent* device_components = nullptr;
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
            || cudaMalloc(reinterpret_cast<void**>(&device_key), sizeof(unsigned long long)) != cudaSuccess
            || cudaMalloc(reinterpret_cast<void**>(&device_mask), 512 * 512) != cudaSuccess
            || cudaMalloc(reinterpret_cast<void**>(&device_scratch), 512 * 512) != cudaSuccess
            || cudaMalloc(reinterpret_cast<void**>(&device_labels), 512 * 512 * sizeof(int)) != cudaSuccess
            || cudaMalloc(reinterpret_cast<void**>(&device_components), 512 * 512 * sizeof(crosshair::CentroidComponent)) != cudaSuccess
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

PivotSnapshot process_frame(const cv::Mat& bgrFrame, int64_t captured_ns, bool crosshair_only)
{
    if (bgrFrame.empty() || bgrFrame.type() != CV_8UC3)
    {
        publish(PivotSnapshot{});
        return {};
    }

    const int active_idx = runtime::g_active_hotkey_index.load();
    {
        std::lock_guard<std::mutex> lock(g_laser_mutex);
        if (g_laser_hotkey != active_idx) {
            g_laser_smoother.reset();
            g_laser_hotkey = active_idx;
        }
    }
    if (active_idx < 0)
    {
        publish(PivotSnapshot{});
        return {};
    }

    bool cross_enabled = false;
    bool laser_enabled = false;
    bool cross_has_color = false;
    bool laser_has_color = false;

    crosshair::CrosshairDetectorSettings cross_settings;
    crosshair::LaserDetectorSettings laser_settings;
    float laser_smooth = 0.0f;

    {
        const auto snapshot = runtime_config::read();
        const auto& cfg = *snapshot;
        if (active_idx >= static_cast<int>(cfg.hotkeys.size()))
        {
            publish(PivotSnapshot{});
            return {};
        }
        const auto& hk = cfg.hotkeys[active_idx];
        cross_enabled = hk.crosshair_detect_enabled;
        if (crosshair_only && !cross_enabled) return {};
        laser_enabled = hk.laser_detect_enabled && !cross_enabled;
        if (!cross_enabled && !laser_enabled)
        {
            publish(PivotSnapshot{});
            return {};
        }

        if (laser_enabled) {
            laser_settings.enabled = true;
            laser_settings.rect_w = cfg.laser_rect_w;
            laser_settings.rect_h = cfg.laser_rect_h;
            laser_settings.center_x = cfg.laser_center_x;
            laser_settings.center_y = cfg.laser_center_y;
            laser_settings.target_center_x = cfg.laser_target_center_x;
            laser_settings.target_center_y = cfg.laser_target_center_y;
            laser_settings.target_rect_w = cfg.laser_target_rect_w;
            laser_settings.target_rect_h = cfg.laser_target_rect_h;
            laser_settings.min_pixel_count = cfg.laser_min_pixel_count;
            laser_settings.close_radius = cfg.laser_close_radius;
            laser_settings.min_elongation = cfg.laser_min_elongation;
            laser_smooth = cfg.laser_smooth;
            for (const auto& c : cfg.laser_colors) {
                crosshair::CrosshairColorBand b;
                b.name = c.name; b.enabled = c.enabled;
                b.exact_hsv = c.exact_hsv;
                b.h_low = c.h_low; b.h_high = c.h_high;
                b.s_min = c.s_min; b.s_max = c.s_max;
                b.v_min = c.v_min; b.v_max = c.v_max;
                laser_has_color = laser_has_color || b.enabled;
                laser_settings.colors.push_back(std::move(b));
            }
        }

        cross_settings.enabled         = cross_enabled;
        cross_settings.algorithm = cfg.crosshair_algorithm;
        cross_settings.rect_w          = cfg.crosshair_rect_w;
        cross_settings.rect_h          = cfg.crosshair_rect_h;
        cross_settings.offset_y        = cfg.crosshair_offset_y;
        cross_settings.min_pixel_count = cfg.crosshair_min_pixel_count;
        cross_settings.close_radius    = cfg.crosshair_close_radius;
        cross_settings.colors.reserve(cfg.crosshair_colors.size());
        for (const auto& c : cfg.crosshair_colors)
        {
            crosshair::CrosshairColorBand b;
            b.name = c.name; b.enabled = c.enabled;
            b.exact_hsv = c.exact_hsv;
            b.h_low = c.h_low; b.h_high = c.h_high;
            b.s_min = c.s_min; b.s_max = c.s_max;
            b.v_min = c.v_min; b.v_max = c.v_max;
            cross_has_color = cross_has_color || b.enabled;
            cross_settings.colors.push_back(std::move(b));
        }
    }

    // Only the legacy laser path depends on previous target visibility.
    if (laser_enabled) {
        std::lock_guard<std::mutex> lk(detectionBuffer.mutex);
        if (detectionBuffer.boxes.empty() || detectionBuffer.staleLocked()) {
            std::lock_guard<std::mutex> laserLock(g_laser_mutex);
            g_laser_smoother.reset();
            publish(PivotSnapshot{});
            return {};
        }
    }

    std::optional<cv::Point2f> hit;
    // 镭射路径同时取整条线段（枪口→激光点），预览要把这条线画出来。
    crosshair::LaserResult laserLine;
    if (cross_enabled && cross_has_color)
        hit = g_detector.detect(bgrFrame, cross_settings);
    else if (laser_enabled && laser_has_color) {
        laserLine = g_laser_detector.detectLine(bgrFrame, laser_settings);
        if (laserLine.found) hit = laserLine.tip;
    }

    {
        std::lock_guard<std::mutex> lock(g_laser_mutex);
        if (laser_enabled && hit && laser_smooth > 0.001f) {
            const double t = std::chrono::duration<double>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            *hit = g_laser_smoother.filter(*hit, t, laser_smooth);
        } else if (!laser_enabled || !hit) g_laser_smoother.reset();
    }

    PivotSnapshot snap;
    snap.active_hotkey = active_idx;
    snap.ts = captured_ns > 0 ? std::chrono::steady_clock::time_point(std::chrono::nanoseconds(captured_ns))
                              : std::chrono::steady_clock::now();
    snap.laser_valid = laserLine.found;
    snap.laser_muzzle = laserLine.muzzle;
    snap.laser_tip = laserLine.tip;
    snap.laser_visible_tip = laserLine.visible_tip;
    if (hit)
    {
        snap.x = static_cast<double>(hit->x);
        snap.y = static_cast<double>(hit->y);
        snap.valid = true;
    }
    publish(snap);
    return snap;
}

bool same_frame_crosshair_active()
{
    const int active_idx = runtime::g_active_hotkey_index.load();
    if (active_idx < 0) return false;
    const auto snapshot = runtime_config::read();
    if (active_idx >= static_cast<int>(snapshot->hotkeys.size())) return false;
    return snapshot->hotkeys[active_idx].crosshair_detect_enabled;
}

bool cpu_path_active()
{
    const int active_idx = runtime::g_active_hotkey_index.load();
    if (active_idx < 0) return false;
    const auto snapshot = runtime_config::read();
    if (!snapshot || active_idx >= static_cast<int>(snapshot->hotkeys.size())) return false;
    const auto& hk = snapshot->hotkeys[active_idx];
    return hk.laser_detect_enabled && !hk.crosshair_detect_enabled;
}

PivotSnapshot process_gpu_frame(const GpuImage& frame)
{
    if (frame.empty() || frame.channels() != 3 || !same_frame_crosshair_active())
    {
        g_last_gpu_hotkey = -1;
        publish(PivotSnapshot{});
        return {};
    }

    const int active_idx = runtime::g_active_hotkey_index.load();
    const auto snapshot = runtime_config::read();
    if (active_idx < 0 || active_idx >= static_cast<int>(snapshot->hotkeys.size()))
    {
        publish(PivotSnapshot{});
        return {};
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
        return {};
    }

    const int roi_w = std::min(frame.cols(), std::max(4, snapshot->crosshair_rect_w));
    const int roi_h = std::min(frame.rows(), std::max(4, snapshot->crosshair_rect_h));
    const int roi_x = std::clamp(frame.cols() / 2 - roi_w / 2, 0, frame.cols() - roi_w);
    const int roi_y = std::clamp(frame.rows() / 2 - roi_h + 10 + snapshot->crosshair_offset_y,
                                 0, frame.rows() - roi_h);

    int reference_x = frame.cols() / 2;
    int reference_y = frame.rows() / 2;
    const auto previous = read();
    const auto frame_time = frame.captureNs() > 0
        ? std::chrono::steady_clock::time_point(std::chrono::nanoseconds(frame.captureNs()))
        : std::chrono::steady_clock::now();
    const auto previous_age = frame_time - previous.ts;
    if (snapshot->crosshair_algorithm == 0 && g_last_gpu_hotkey == active_idx && previous.valid
        && previous_age >= std::chrono::steady_clock::duration::zero()
        && previous_age <= std::chrono::milliseconds(kFreshnessMs)
        && previous.x >= roi_x && previous.x < roi_x + roi_w
        && previous.y >= roi_y && previous.y < roi_y + roi_h)
    {
        reference_x = static_cast<int>(std::lround(previous.x));
        reference_y = static_cast<int>(std::lround(previous.y));
    }
    g_last_gpu_hotkey = active_idx;

    auto& state = gpu_state();
    if (!state.ensure())
    {
        publish(PivotSnapshot{});
        return {};
    }
    if (frame.readyEvent())
        cudaStreamWaitEvent(state.stream, frame.readyEvent(), 0);
    if (cudaMemcpyAsync(state.device_bands, bands.data(),
                        bands.size() * sizeof(GpuHsvBand), cudaMemcpyHostToDevice,
                        state.stream) != cudaSuccess
        || cudaMemsetAsync(state.device_result, 0, sizeof(int) * 4, state.stream) != cudaSuccess
        || cudaMemsetAsync(state.device_key, 0, sizeof(unsigned long long), state.stream) != cudaSuccess)
    {
        publish(PivotSnapshot{});
        return {};
    }

    launch_crosshair_hsv_reduce_bgr_u8(
        frame.data(), frame.step(), frame.cols(), frame.rows(),
        roi_x, roi_y, roi_w, roi_h,
        state.device_bands, static_cast<int>(bands.size()),
        state.device_result, state.device_key,
        state.device_mask, state.device_scratch,
        state.device_labels, state.device_components,
        std::clamp(snapshot->crosshair_close_radius, 0, 7),
        std::max(1, snapshot->crosshair_min_pixel_count),
        reference_x, reference_y, snapshot->crosshair_algorithm, state.stream);
    if (cudaGetLastError() != cudaSuccess
        || cudaMemcpyAsync(state.host_result, state.device_result, sizeof(int) * 4,
                           cudaMemcpyDeviceToHost, state.stream) != cudaSuccess
        || cudaStreamSynchronize(state.stream) != cudaSuccess)
    {
        publish(PivotSnapshot{});
        return {};
    }

    PivotSnapshot out;
    out.active_hotkey = active_idx;
    out.ts = frame.captureNs() > 0 ? std::chrono::steady_clock::time_point(std::chrono::nanoseconds(frame.captureNs()))
                                  : std::chrono::steady_clock::now();
    const int count = state.host_result[1];
    if (count >= std::max(snapshot->crosshair_algorithm == 1 ? 2 : 1, snapshot->crosshair_min_pixel_count))
    {
        cv::Point2f hit(
            static_cast<float>(state.host_result[2]) / static_cast<float>(count),
            static_cast<float>(state.host_result[3]) / static_cast<float>(count));
        if (snapshot->crosshair_algorithm == 1) {
            hit.x = static_cast<float>(crosshair::amCentroidCoordinate(state.host_result[2], count));
            hit.y = static_cast<float>(crosshair::amCentroidCoordinate(state.host_result[3], count));
        }
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
    publish(out);
    return out;
}

}
