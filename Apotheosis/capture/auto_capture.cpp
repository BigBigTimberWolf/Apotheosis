#define WIN32_LEAN_AND_MEAN
#define _WINSOCKAPI_
#include <winsock2.h>
#include <Windows.h>

#include "capture/auto_capture.h"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <locale>
#include <mutex>
#include <string>
#include <vector>

#include <opencv2/opencv.hpp>

#include "Apotheosis.h"
#include "capture/capture.h"
#include "config/config.h"
#include "detector/detection_buffer.h"
#include "keyboard/keyboard_listener.h"
#include "mem/gpu_image.h"
#include "runtime/frame_context.h"

extern std::atomic<bool> shouldExit;

namespace AutoCapture
{

std::atomic<int>  g_saved_total{0};
std::atomic<int>  g_saved_session{0};
std::atomic<bool> g_force_held{false};
std::atomic<bool> g_running{false};

void reset_session_counter()
{
    g_saved_session.store(0);
}

namespace
{

struct SavedFrame
{
    runtime::FrameContext context;
    GpuImage gpu;
    cv::Mat cpu;
};

std::mutex frame_cache_mutex;
std::deque<SavedFrame> frame_cache;
constexpr size_t kMaxCachedFrames = 16;
constexpr size_t kMaxCachedBytes = 64ull * 1024 * 1024;

size_t frame_bytes(const SavedFrame& frame)
{
    return !frame.gpu.empty()
        ? frame.gpu.step() * static_cast<size_t>(frame.gpu.rows())
        : frame.cpu.total() * frame.cpu.elemSize();
}

void trim_frame_cache()
{
    size_t bytes = 0;
    for (const auto& frame : frame_cache) bytes += frame_bytes(frame);
    while (frame_cache.size() > kMaxCachedFrames ||
           (frame_cache.size() > 1 && bytes > kMaxCachedBytes))
    {
        bytes -= frame_bytes(frame_cache.front());
        frame_cache.pop_front();
    }
}

bool same_frame(const runtime::FrameContext& a, const runtime::FrameContext& b)
{
    if (a.sequence && b.sequence) return a.sequence == b.sequence;
    return a.captured_ns > 0 && a.captured_ns == b.captured_ns;
}

bool find_frame(runtime::FrameContext context, bool force, SavedFrame& out)
{
    std::lock_guard<std::mutex> lk(frame_cache_mutex);
    for (auto it = frame_cache.rbegin(); it != frame_cache.rend(); ++it)
        if (same_frame(it->context, context)) { out = *it; return true; }
    if (force && !frame_cache.empty()) { out = frame_cache.back(); return false; }
    return false;
}

struct CfgSnap
{
    bool  enabled = false;
    bool  use_high = true;
    float high_conf = 0.85f;
    bool  use_low = false;
    float low_conf = 0.30f;
    bool  any_detection = false;
    int   cooldown_ms = 200;
    std::vector<std::string> force_keys;
    std::string out_dir = "screenshots/auto";
    bool  save_label = true;
};

CfgSnap snapshot_cfg()
{
    CfgSnap s;
    std::lock_guard<std::recursive_mutex> lk(configMutex);
    s.enabled       = config.auto_capture_enabled;
    s.use_high      = config.auto_capture_use_high;
    s.high_conf     = config.auto_capture_high_conf;
    s.use_low       = config.auto_capture_use_low;
    s.low_conf      = config.auto_capture_low_conf;
    s.any_detection = config.auto_capture_any_detection;
    s.cooldown_ms   = std::max(0, config.auto_capture_cooldown_ms);
    s.force_keys    = config.auto_capture_force_keys;
    s.out_dir       = config.auto_capture_output_dir.empty()
                          ? std::string("screenshots/auto")
                          : config.auto_capture_output_dir;
    s.save_label    = config.auto_capture_save_label;
    return s;
}

std::string make_filename_stem()
{
    using clock = std::chrono::system_clock;
    const auto now = clock::now();
    const std::time_t t = clock::to_time_t(now);
    std::tm tm{};
    localtime_s(&tm, &t);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()).count() % 1000;
    char buf[64];
    std::snprintf(buf, sizeof(buf),
                  "auto_%04d%02d%02d_%02d%02d%02d_%03lld",
                  tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                  tm.tm_hour, tm.tm_min, tm.tm_sec,
                  static_cast<long long>(ms));
    return buf;
}

bool any_in_zone(const std::vector<float>& confs,
                 bool use_high, float high,
                 bool use_low,  float low)
{
    if (!use_high && !use_low) return false;
    for (const float c : confs)
    {
        if (use_high && c >= high) return true;
        if (use_low  && c <= low && c > 0.0f) return true;
    }
    return false;
}

bool write_yolo_label(const std::filesystem::path& path,
                      const std::vector<cv::Rect>& boxes,
                      const std::vector<int>& classes,
                      double frame_w, double frame_h)
{
    if (frame_w <= 0 || frame_h <= 0) return false;
    std::ofstream file(path, std::ios::trunc);
    if (!file) return false;
    file.imbue(std::locale::classic());
    file << std::fixed << std::setprecision(6);

    const size_t n = std::min(boxes.size(), classes.size());
    for (size_t i = 0; i < n; ++i)
    {
        const cv::Rect& b = boxes[i];
        if (b.width <= 0 || b.height <= 0) continue;
        const double xc = (b.x + b.width  * 0.5) / frame_w;
        const double yc = (b.y + b.height * 0.5) / frame_h;
        const double w  =  b.width  / frame_w;
        const double h  =  b.height / frame_h;
        file << classes[i] << ' ' << xc << ' ' << yc << ' ' << w << ' ' << h << '\n';
    }
    file.close();
    return !file.fail();
}

bool write_jpeg(const std::filesystem::path& path, const cv::Mat& frame)
{
    std::vector<uchar> bytes;
    if (!cv::imencode(".jpg", frame, bytes, {cv::IMWRITE_JPEG_QUALITY, 92}))
        return false;
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) return false;
    file.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
    file.close();
    return !file.fail();
}

}

void submit_frame(const GpuImage& frame, runtime::FrameContext context)
{
    if (frame.empty()) return;
    std::lock_guard<std::mutex> lk(frame_cache_mutex);
    const auto old = std::find_if(frame_cache.begin(), frame_cache.end(),
        [&](const SavedFrame& entry) { return same_frame(entry.context, context); });
    if (old != frame_cache.end()) frame_cache.erase(old);
    frame_cache.push_back({context, frame, {}});
    trim_frame_cache();
}

void submit_frame(const cv::Mat& frame, runtime::FrameContext context)
{
    if (frame.empty()) return;
    cv::Mat copy = frame.clone();
    std::lock_guard<std::mutex> lk(frame_cache_mutex);
    const auto old = std::find_if(frame_cache.begin(), frame_cache.end(),
        [&](const SavedFrame& entry) { return same_frame(entry.context, context); });
    if (old != frame_cache.end()) frame_cache.erase(old);
    frame_cache.push_back({context, {}, std::move(copy)});
    trim_frame_cache();
}

void clear_frames()
{
    std::lock_guard<std::mutex> lk(frame_cache_mutex);
    frame_cache.clear();
}

void auto_capture_thread()
{
    g_running.store(true);
    g_saved_session.store(0);

    int last_version = -1;
    auto last_save_ts = std::chrono::steady_clock::time_point::min();
    uint64_t file_sequence = 0;
    std::string last_error;
    auto report_error = [&](const std::string& error) {
        if (error != last_error)
            std::cerr << "[AutoCapture] " << error << std::endl;
        last_error = error;
    };

    while (!shouldExit.load())
    {
        bool fresh = false;
        std::vector<cv::Rect> boxes;
        std::vector<int> classes;
        std::vector<float> confidences;
        runtime::FrameContext detection_context;
        {
            std::unique_lock<std::mutex> lk(detectionBuffer.mutex);
            detectionBuffer.cv.wait_for(lk, std::chrono::milliseconds(50),
                [&] { return detectionBuffer.version > last_version
                              || shouldExit.load(); });
            if (shouldExit.load()) break;
            if (detectionBuffer.version > last_version)
            {
                last_version = detectionBuffer.version;
                fresh = true;
                boxes = detectionBuffer.boxes;
                classes = detectionBuffer.classes;
                confidences = detectionBuffer.confidences;
                detection_context = detectionBuffer.frame_context;
            }
        }

        const CfgSnap cfg = snapshot_cfg();
        if (!cfg.enabled)
        {
            g_force_held.store(false);
            clear_frames();
            continue;
        }

        const bool force_held = isAnyKeyPressed(cfg.force_keys)
                                && !cfg.force_keys.empty();
        g_force_held.store(force_held);
        // Force capture is checked on every timeout too: the detector may be
        // stopped, slow, or publishing no results while the key is held.
        if (!fresh && !force_held) continue;

        if (boxes.empty() && !force_held) continue;

        bool should_save = false;
        if (force_held)
            should_save = true;
        else if (!boxes.empty())
        {
            if (cfg.any_detection)
                should_save = true;
            else
                should_save = any_in_zone(confidences,
                                          cfg.use_high, cfg.high_conf,
                                          cfg.use_low,  cfg.low_conf);
        }
        if (!should_save) continue;

        const auto now = std::chrono::steady_clock::now();
        if (last_save_ts != std::chrono::steady_clock::time_point::min()
            && now - last_save_ts < std::chrono::milliseconds(cfg.cooldown_ms))
        {
            continue;
        }

        SavedFrame source;
        const bool matched = find_frame(detection_context, force_held, source);
        if (!matched && !force_held) continue;
        if (!matched) { boxes.clear(); classes.clear(); }

        cv::Mat frame;
        std::filesystem::path img_path;
        std::filesystem::path lbl_path;
        std::error_code ec;
        bool image_written = false;
        {
            try
            {
                if (!source.gpu.empty()) source.gpu.download(frame);
                else frame = source.cpu;
                if (frame.empty()) continue;

                const auto out_dir = std::filesystem::u8path(cfg.out_dir);
                std::filesystem::create_directories(out_dir, ec);
                if (ec) { report_error("create directory failed: " + ec.message()); continue; }

                const std::string stem = make_filename_stem() + "_" +
                    std::to_string(++file_sequence);
                img_path = out_dir / (stem + ".jpg");
                lbl_path = out_dir / (stem + ".txt");
                image_written = write_jpeg(img_path, frame);
                if (!image_written)
                {
                    std::filesystem::remove(img_path, ec);
                    report_error("image write failed: " + img_path.string());
                    continue;
                }
                if (cfg.save_label && !write_yolo_label(lbl_path, boxes, classes,
                        static_cast<double>(frame.cols), static_cast<double>(frame.rows)))
                {
                    std::filesystem::remove(img_path, ec);
                    std::filesystem::remove(lbl_path, ec);
                    report_error("label write failed: " + lbl_path.string());
                    continue;
                }
            }
            catch (const std::exception& e)
            {
                if (image_written) std::filesystem::remove(img_path, ec);
                if (cfg.save_label && !lbl_path.empty()) std::filesystem::remove(lbl_path, ec);
                report_error(std::string("save failed: ") + e.what());
                continue;
            }
            catch (...)
            {
                if (image_written) std::filesystem::remove(img_path, ec);
                if (cfg.save_label && !lbl_path.empty()) std::filesystem::remove(lbl_path, ec);
                report_error("save failed: unknown error");
                continue;
            }
        }

        last_save_ts = now;
        last_error.clear();
        g_saved_total.fetch_add(1);
        g_saved_session.fetch_add(1);
    }

    g_running.store(false);
    g_force_held.store(false);
}

}
