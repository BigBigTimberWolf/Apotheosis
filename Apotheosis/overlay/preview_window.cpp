#define WIN32_LEAN_AND_MEAN
#define _WINSOCKAPI_
#include <winsock2.h>
#include <Windows.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <opencv2/opencv.hpp>
#include <opencv2/highgui.hpp>

#include "Apotheosis.h"
#include "capture.h"
#include "config/config.h"
#include "control/aim_controller.h"   // 预览叠加用: StabilizerVerdict / IdleReason
#include "crosshair/color_picker.h"
#include "crosshair/crosshair_detector.h"
#include "crosshair/crosshair_runtime.h"
#include "detection_buffer.h"
#include "i_detector.h"
#include "preview_window.h"
#include "runtime/active_hotkey.h"
#include "runtime/inference_session.h"
#include "runtime/aim_telemetry.h"
#include "runtime/latency_probe.h"

namespace
{
constexpr const char* kWindowName = "Detection Preview";

std::thread g_thread;
std::atomic<bool> g_run{ false };

std::mutex g_clean_mutex;
cv::Mat    g_clean_frame;
int        g_pick_cursor_x = -1;
int        g_pick_cursor_y = -1;
bool       g_pick_cursor_inside = false;

cv::Scalar bgr(int b, int g, int r) { return cv::Scalar(b, g, r); }

void draw_text_with_bg(cv::Mat& img, const std::string& text, cv::Point org,
                       const cv::Scalar& fg, const cv::Scalar& bg)
{
    int baseline = 0;
    const int font = cv::FONT_HERSHEY_SIMPLEX;
    const double scale = 0.45;
    const int thickness = 1;
    cv::Size sz = cv::getTextSize(text, font, scale, thickness, &baseline);
    cv::Point tl(org.x, org.y - sz.height - 3);
    cv::Point br(org.x + sz.width + 4, org.y + 2);
    cv::rectangle(img, tl, br, bg, cv::FILLED);
    cv::putText(img, text, cv::Point(org.x + 2, org.y - 2), font, scale, fg, thickness, cv::LINE_AA);
}

bool window_visible()
{
    try {
        return cv::getWindowProperty(kWindowName, cv::WND_PROP_VISIBLE) >= 1.0;
    } catch (...) {
        return false;
    }
}

void destroy_window_safe()
{
    try { cv::destroyWindow(kWindowName); } catch (...) {}
}

struct PreviewConfigSnapshot
{
    bool   show_window = false;
    int    detection_resolution = 0;
    int    crosshair_rect_w = 0;
    int    crosshair_rect_h = 0;
    int    crosshair_min_pixel_count = 0;
    int    crosshair_close_radius = 0;
    std::vector<crosshair::CrosshairColorBand> crosshair_colors;
    bool   any_color_enabled = false;
    bool   crosshair_hotkey_enabled = false;

    int    fov_base_x = 0;
    int    fov_base_y = 0;
    bool   dynamic_fov_enabled = false;
    bool   hotkey_active = false;
    bool   show_fps = false;
    float  replay_playback_speed = 0.25f;
};

PreviewConfigSnapshot snapshot_config()
{
    PreviewConfigSnapshot s;
    std::lock_guard<std::recursive_mutex> lk(configMutex);
    s.show_window              = config.show_window;
    s.detection_resolution     = config.detection_resolution;
    s.crosshair_rect_w         = config.crosshair_rect_w;
    s.crosshair_rect_h         = config.crosshair_rect_h;
    s.crosshair_min_pixel_count = config.crosshair_min_pixel_count;
    s.crosshair_close_radius   = config.crosshair_close_radius;
    s.crosshair_colors.reserve(config.crosshair_colors.size());
    for (const auto& c : config.crosshair_colors)
    {
        crosshair::CrosshairColorBand b;
        b.name = c.name;
        b.enabled = c.enabled;
        b.h_low = c.h_low; b.h_high = c.h_high;
        b.s_min = c.s_min; b.s_max = c.s_max;
        b.v_min = c.v_min; b.v_max = c.v_max;
        s.any_color_enabled = s.any_color_enabled || b.enabled;
        s.crosshair_colors.push_back(std::move(b));
    }

    s.show_fps                     = config.show_fps;
    s.replay_playback_speed        = config.replay_playback_speed;

    const int idx_active = runtime::g_active_hotkey_index.load();
    const int idx = (idx_active >= 0 && idx_active < static_cast<int>(config.hotkeys.size()))
        ? idx_active
        : (config.hotkeys.empty() ? -1 : 0);
    s.hotkey_active = (idx_active >= 0);
    if (idx >= 0)
    {
        const auto& hk = config.hotkeys[idx];
        s.fov_base_x = hk.fovX;
        s.fov_base_y = hk.fovY;
        s.dynamic_fov_enabled = hk.dynamic_fov_enabled;
        s.crosshair_hotkey_enabled = hk.crosshair_detect_enabled;
    }
    return s;
}

void render_overlays(cv::Mat& canvas, const PreviewConfigSnapshot& cfg)
{
    if (canvas.empty()) return;

    std::vector<cv::Rect> boxes;
    std::vector<int> classes;
    int version = 0;
    {
        std::lock_guard<std::mutex> lk(detectionBuffer.mutex);
        boxes = detectionBuffer.boxes;
        classes = detectionBuffer.classes;
        version = detectionBuffer.version;
    }

    const cv::Scalar boxColor = bgr(110, 220, 80);
    const cv::Scalar textFg   = bgr(250, 245, 240);
    const cv::Scalar textBg   = bgr(0, 0, 0);

    for (size_t i = 0; i < boxes.size(); ++i)
    {
        const cv::Rect& r = boxes[i];
        const cv::Rect clipped = r & cv::Rect(0, 0, canvas.cols, canvas.rows);
        if (clipped.area() <= 0) continue;
        cv::rectangle(canvas, clipped, boxColor, 1, cv::LINE_AA);

        const int cls = (i < classes.size()) ? classes[i] : -1;
        char label[64];
        std::snprintf(label, sizeof(label), "#%d", cls);
        draw_text_with_bg(canvas, label,
                          cv::Point(clipped.x, std::max(12, clipped.y)),
                          textFg, textBg);
    }

    if (cfg.fov_base_x > 0 && cfg.fov_base_y > 0)
    {
        const cv::Point center(canvas.cols / 2, canvas.rows / 2);
        const cv::Size baseAxes(std::max(1, cfg.fov_base_x / 2),
                                std::max(1, cfg.fov_base_y / 2));

        const cv::Scalar baseCol = bgr(60, 200, 255);
        cv::ellipse(canvas, center, baseAxes, 0, 0, 360, baseCol, 1, cv::LINE_AA);
    }

    // ── 稳定【之后】的叠加 ────────────────────────────────────────────────
    //
    // 上面那些绿框是检测的【原始】框(稳定之前)。这一段画的是控制器实际锁定的
    // 结果: 稳定器放行的框 + α-β 滤波后的中心 + 最终瞄点。两者画在一起,
    // 才能看出稳定器到底把哪一帧的抖动压掉了。
    {
        const runtime::AimOverlayState ov = runtime::readAimOverlay();
        const auto age_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - ov.ts).count();
        const bool fresh = ov.ts.time_since_epoch().count() != 0 &&
                           age_ms >= 0 && age_ms <= runtime::kAimOverlayStaleMs;

        if (fresh)
        {
            // 判定颜色: 同一个目标=绿, 新目标/瞬移=橙, 被稳定器丢掉=红。
            const auto verdict = static_cast<control::StabilizerVerdict>(ov.verdict);
            cv::Scalar lockCol = bgr(90, 220, 90);
            const char* verdictText = "OK";
            switch (verdict)
            {
            case control::StabilizerVerdict::Ok:       lockCol = bgr(90, 220, 90);  verdictText = "OK";       break;
            case control::StabilizerVerdict::NoHistory:lockCol = bgr(80, 190, 255); verdictText = "NEW";      break;
            case control::StabilizerVerdict::Snap:     lockCol = bgr(60, 160, 255); verdictText = "SNAP";     break;
            case control::StabilizerVerdict::Rejected: lockCol = bgr(90, 90, 235);  verdictText = "REJECTED"; break;
            }

            if (ov.engaged && ov.box.width > 0 && ov.box.height > 0)
            {
                const cv::Rect r(ov.box.x, ov.box.y, ov.box.width, ov.box.height);
                const cv::Rect clipped = r & cv::Rect(0, 0, canvas.cols, canvas.rows);
                if (clipped.area() > 0)
                {
                    // ① 原始锁定框(稳定前): 粗框, 颜色 = 稳定器判定。
                    cv::rectangle(canvas, clipped, lockCol, 2, cv::LINE_AA);

                    const cv::Point rawC(clipped.x + clipped.width / 2,
                                         clipped.y + clipped.height / 2);
                    const cv::Point stabC(static_cast<int>(std::lround(ov.filtered_cx)),
                                          static_cast<int>(std::lround(ov.filtered_cy)));

                    // ② 稳定后的框: 尺寸还是检测的尺寸, 但【中心换成滤波后的中心】——
                    //    这就是"稳定之后"的框。抖的时候它会明显比原始框稳。
                    const cv::Rect stabRect(stabC.x - clipped.width / 2,
                                            stabC.y - clipped.height / 2,
                                            clipped.width, clipped.height);
                    const cv::Rect stabClip = stabRect & cv::Rect(0, 0, canvas.cols, canvas.rows);
                    if (stabClip.area() > 0)
                    {
                        cv::rectangle(canvas, stabClip, bgr(255, 120, 240), 1, cv::LINE_AA);
                        draw_text_with_bg(canvas, "STAB",
                                          cv::Point(stabClip.x,
                                                    std::min(canvas.rows - 2,
                                                             stabClip.y + stabClip.height + 12)),
                                          bgr(255, 200, 250), bgr(40, 0, 40));
                    }

                    // ③ 原始中心 → 滤波中心: 这条线的长度 = 这一拍压掉的抖动量。
                    if (rawC != stabC)
                        cv::line(canvas, rawC, stabC, bgr(255, 120, 240), 1, cv::LINE_AA);

                    char label[96];
                    std::snprintf(label, sizeof(label), "LOCK #%d id=%d %s",
                                  ov.target_class_id, ov.target_id, verdictText);
                    draw_text_with_bg(canvas, label,
                                      cv::Point(clipped.x, std::max(12, clipped.y)),
                                      bgr(250, 245, 240), bgr(0, 0, 0));

                    // ④ 最终瞄点(锚点): 橙点。
                    const cv::Point aim(static_cast<int>(std::lround(ov.anchor_x)),
                                        static_cast<int>(std::lround(ov.anchor_y)));
                    cv::circle(canvas, aim, 4, bgr(0, 140, 255), 2, cv::LINE_AA);
                }
            }

            // 状态行: 一屏说清"锁没锁、锁的是谁、稳定器怎么判、哪一步丢的"。
            {
                char line[240];
                if (ov.engaged)
                {
                    const double dx = ov.filtered_cx - (ov.box.x + ov.box.width * 0.5);
                    const double dy = ov.filtered_cy - (ov.box.y + ov.box.height * 0.5);
                    std::snprintf(line, sizeof(line),
                                  "Stab: %s | id=%d #%d | raw->filtered %+.1f,%+.1f px%s",
                                  verdictText, ov.target_id, ov.target_class_id, dx, dy,
                                  ov.scope_params ? " | SCOPE-PARAMS" : "");
                }
                else
                {
                    using Idle = control::ControlOutput::IdleReason;
                    const char* why = "idle";
                    switch (static_cast<Idle>(ov.idle_reason))
                    {
                    case Idle::NoCandidates:          why = "no-candidate";   break;
                    case Idle::StaleDetection:        why = "stale-detection";break;
                    case Idle::StaleCrosshair:        why = "stale-crosshair";break;
                    case Idle::RejectedByStabilizer:  why = "STAB-REJECTED (wide/tall)"; break;
                    case Idle::BadDt:                 why = "bad-dt";         break;
                    case Idle::None:                  why = "idle";           break;
                    }
                    std::snprintf(line, sizeof(line), "Stab: not locked | %s%s",
                                  why, ov.scope_params ? " | SCOPE-PARAMS" : "");
                }
                draw_text_with_bg(canvas, line, cv::Point(6, canvas.rows - 30),
                                  bgr(245, 245, 245), bgr(0, 0, 0));
            }
        }
    }

    if (cfg.crosshair_rect_w > 0 && cfg.crosshair_rect_h > 0)
    {
        const int rw = std::max(4, cfg.crosshair_rect_w);
        const int rh = std::max(4, cfg.crosshair_rect_h);
        constexpr int kVerticalOffset = 10;
        const cv::Rect roi(canvas.cols / 2 - rw / 2, canvas.rows / 2 - rh + kVerticalOffset, rw, rh);
        const cv::Rect clipped = roi & cv::Rect(0, 0, canvas.cols, canvas.rows);
        if (clipped.area() > 0)
            cv::rectangle(canvas, clipped, bgr(255, 190, 0), 1, cv::LINE_AA);
    }

    {
        const auto snap = crosshair_runtime::read();
        const auto ref  = crosshair_runtime::read_static_ref();
        const auto ref_age_ms = ref.valid
            ? std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::steady_clock::now() - ref.ts).count()
            : -1;
        const bool ref_fresh = ref_age_ms >= 0 && ref_age_ms <= 250;
        const long long age_ms = snap.valid
            ? std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::steady_clock::now() - snap.ts).count()
            : -1;

        char ref_text[48];
        if (ref_fresh)
            std::snprintf(ref_text, sizeof(ref_text), "ref (%d,%d)",
                          static_cast<int>(std::lround(ref.x)),
                          static_cast<int>(std::lround(ref.y)));
        else
            std::snprintf(ref_text, sizeof(ref_text), "ref -");

        char line[200];
        if (!cfg.any_color_enabled || !cfg.crosshair_hotkey_enabled)
        {
            std::snprintf(line, sizeof(line), "Xhair: OFF (hotkey/palette off)");
        }
        else if (snap.valid)
        {
            std::snprintf(line, sizeof(line),
                          "Xhair: HIT (%d,%d) age=%lldms%s | %s",
                          static_cast<int>(std::lround(snap.x)),
                          static_cast<int>(std::lround(snap.y)),
                          age_ms,
                          (age_ms > crosshair_runtime::kFreshnessMs) ? " STALE" : "",
                          ref_text);
        }
        else
        {
            std::snprintf(line, sizeof(line), "Xhair: MISS | %s", ref_text);
        }

        const cv::Scalar col = snap.valid ? bgr(80, 255, 80) : bgr(150, 150, 150);
        draw_text_with_bg(canvas, line, cv::Point(6, canvas.rows - 6),
                          bgr(245, 245, 245), bgr(0, 0, 0));

        if (snap.valid)
        {
            const cv::Point p(static_cast<int>(std::lround(snap.x)),
                              static_cast<int>(std::lround(snap.y)));
            cv::drawMarker(canvas, p, col, cv::MARKER_CROSS, 14, 1, cv::LINE_AA);
            cv::circle(canvas, p, 6, col, 1, cv::LINE_AA);
        }

        if (ref_fresh)
        {
            const cv::Point r(static_cast<int>(std::lround(ref.x)),
                              static_cast<int>(std::lround(ref.y)));
            cv::rectangle(canvas, cv::Rect(r.x - 7, r.y - 7, 15, 15),
                          bgr(255, 120, 240), 1, cv::LINE_AA);
        }

        cv::drawMarker(canvas, cv::Point(canvas.cols / 2, canvas.rows / 2),
                       bgr(0, 200, 255), cv::MARKER_TILTED_CROSS, 10, 1, cv::LINE_AA);
    }

    if (!cfg.show_fps)
        return;
    const auto timing = runtime::latency::snapshot();
    const float infer_ms = static_cast<float>(timing.engine_inference_ms);

    static int    s_last_version  = -1;
    static int    s_frames_seen   = 0;
    static auto   s_window_start  = std::chrono::steady_clock::now();
    static float  s_inference_fps = 0.0f;
    const auto now = std::chrono::steady_clock::now();
    if (version != s_last_version)
    {
        if (s_last_version >= 0)
            s_frames_seen += std::max(0, version - s_last_version);
        s_last_version = version;
    }
    const double elapsed_s =
        std::chrono::duration<double>(now - s_window_start).count();
    if (elapsed_s >= 0.5)
    {
        s_inference_fps = static_cast<float>(s_frames_seen / elapsed_s);
        s_frames_seen = 0;
        s_window_start = now;
    }

    float displayed_fps = s_inference_fps;
    if (displayed_fps <= 0.01f && infer_ms > 0.01f)
        displayed_fps = 1000.0f / infer_ms;

    char buf[96];
    std::snprintf(buf, sizeof(buf), "Infer %.1f FPS | Lat %.1f ms",
                  displayed_fps, infer_ms);
    draw_text_with_bg(canvas, buf, cv::Point(6, 16), bgr(245, 245, 245), bgr(0, 0, 0));

    int y = 34;
    for (const auto& line : runtime::latency::formatLinesAscii(true))
    {
        draw_text_with_bg(canvas, line, cv::Point(6, y),
                          bgr(120, 255, 160), bgr(0, 0, 0));
        y += 16;
    }
}

void on_mouse(int event, int x, int y, int  , void*  )
{
    if (event == cv::EVENT_MOUSEMOVE)
    {
        g_pick_cursor_x = x;
        g_pick_cursor_y = y;
        g_pick_cursor_inside = true;
        return;
    }

    if (!crosshair::IsColorPickArmed())
        return;

    if (event == cv::EVENT_RBUTTONDOWN)
    {
        crosshair::CancelColorPick();
        return;
    }

    if (event == cv::EVENT_LBUTTONDOWN)
    {
        cv::Mat clean;
        {
            std::lock_guard<std::mutex> lk(g_clean_mutex);
            if (!g_clean_frame.empty()) g_clean_frame.copyTo(clean);
        }
        int h = 0, s = 0, v = 0;
        if (crosshair::SampleRegionHSV(clean, x, y, crosshair::PickHalf(), h, s, v))
            crosshair::SubmitPickedColor(h, s, v);
    }
}

void draw_pick_overlay(cv::Mat& canvas)
{
    if (canvas.empty() || canvas.type() != CV_8UC3) return;
    if (!crosshair::IsColorPickArmed()) return;

    draw_text_with_bg(canvas, "PICK: click=sample  right-click=cancel",
                      cv::Point(6, canvas.rows - 8), bgr(245, 245, 245), bgr(0, 0, 0));

    if (!g_pick_cursor_inside) return;

    const int cx = g_pick_cursor_x;
    const int cy = g_pick_cursor_y;
    const int half = crosshair::PickHalf();
    const int ringR = half + 4;

    cv::Rect rr(cx - ringR, cy - ringR, 2 * ringR + 1, 2 * ringR + 1);
    rr &= cv::Rect(0, 0, canvas.cols, canvas.rows);
    if (rr.area() > 0)
    {
        cv::Mat patch = canvas(rr).clone();
        cv::circle(patch, cv::Point(cx - rr.x, cy - rr.y), ringR,
                   bgr(0, 220, 255), cv::FILLED, cv::LINE_AA);
        cv::addWeighted(patch, 0.25, canvas(rr), 0.75, 0.0, canvas(rr));
    }

    cv::circle(canvas, cv::Point(cx, cy), ringR, bgr(0, 0, 0), 2, cv::LINE_AA);
    cv::circle(canvas, cv::Point(cx, cy), ringR, bgr(0, 220, 255), 1, cv::LINE_AA);

    cv::Rect foot(cx - half, cy - half, 2 * half + 1, 2 * half + 1);
    foot &= cv::Rect(0, 0, canvas.cols, canvas.rows);
    if (foot.area() > 0)
        cv::rectangle(canvas, foot, bgr(255, 255, 255), 1, cv::LINE_AA);
}

void render_replay_frame(cv::Mat& canvas,
                         const std::vector<runtime::ReplayFrame>& frames,
                         size_t frame_index,
                         float playback_speed)
{
    if (canvas.empty() || frames.empty()) return;
    frame_index = std::min(frame_index, frames.size() - 1);
    const auto& frame = frames[frame_index];

    for (size_t i = 0; i < frame.boxes.size(); ++i)
    {
        const cv::Rect clipped = frame.boxes[i] & cv::Rect(0, 0, canvas.cols, canvas.rows);
        if (clipped.area() <= 0) continue;
        const int track_id = i < frame.track_ids.size() ? frame.track_ids[i] : -1;
        const bool locked = track_id >= 0 && track_id == frame.locked_track_id;
        const cv::Scalar color = locked ? bgr(70, 90, 255) : bgr(110, 220, 80);
        cv::rectangle(canvas, clipped, color, locked ? 3 : 1, cv::LINE_AA);
        const int cls = i < frame.class_ids.size() ? frame.class_ids[i] : -1;
        char label[64];
        std::snprintf(label, sizeof(label), locked ? "LOCK #%d" : "#%d", cls);
        draw_text_with_bg(canvas, label, cv::Point(clipped.x, std::max(12, clipped.y)),
                          bgr(250, 245, 240), bgr(0, 0, 0));
    }

    std::vector<cv::Point> trail;
    const size_t first = frame_index > 90 ? frame_index - 90 : 0;
    trail.reserve(frame_index - first + 1);
    for (size_t i = first; i <= frame_index; ++i)
    {
        if (frames[i].locked_track_id >= 0)
            trail.emplace_back(static_cast<int>(std::lround(frames[i].pivot_x)),
                               static_cast<int>(std::lround(frames[i].pivot_y)));
    }
    if (trail.size() >= 2)
        cv::polylines(canvas, trail, false, bgr(255, 190, 70), 2, cv::LINE_AA);
    if (!trail.empty())
    {
        const cv::Point p = trail.back();
        cv::circle(canvas, p, 5, bgr(0, 0, 0), 3, cv::LINE_AA);
        cv::circle(canvas, p, 5, bgr(255, 220, 80), 1, cv::LINE_AA);
    }

    char banner[128];
    std::snprintf(banner, sizeof(banner), "REPLAY %.2fx | %zu / %zu",
                  playback_speed, frame_index + 1, frames.size());
    draw_text_with_bg(canvas, banner, cv::Point(6, 18),
                      bgr(245, 245, 245), bgr(0, 0, 0));
}

void preview_loop()
{
    bool window_open = false;
    bool replay_was_active = false;
    std::vector<runtime::ReplayFrame> replay_frames;
    size_t replay_index = 0;
    auto replay_next_tick = std::chrono::steady_clock::now();

    while (g_run.load())
    {
        const PreviewConfigSnapshot cfg = snapshot_config();

        if (!cfg.show_window)
        {
            if (window_open)
            {
                destroy_window_safe();
                window_open = false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(150));
            continue;
        }

        const bool visible_now = window_open && window_visible();
        if (!visible_now)
        {
            destroy_window_safe();
            try {
                cv::namedWindow(kWindowName, cv::WINDOW_NORMAL | cv::WINDOW_KEEPRATIO);
                cv::setWindowProperty(kWindowName, cv::WND_PROP_TOPMOST, 0);
                cv::setMouseCallback(kWindowName, on_mouse, nullptr);
                g_pick_cursor_inside = false;
                window_open = true;
            } catch (...) {
                window_open = false;
                std::this_thread::sleep_for(std::chrono::milliseconds(300));
                continue;
            }
        }

        bool replay_active = g_replay_playback_active.load();
        if (replay_active && !replay_was_active)
        {
            replay_frames = runtime::ReplayBuffer::instance().snapshot();
            replay_index = 0;
            replay_next_tick = std::chrono::steady_clock::now();
            if (replay_frames.empty())
            {
                replay_active = false;
                g_replay_playback_active.store(false);
            }
        }
        replay_was_active = replay_active;

        if (replay_active && !replay_frames.empty())
        {
            const float speed = std::clamp(cfg.replay_playback_speed, 0.05f, 2.0f);
            const auto now = std::chrono::steady_clock::now();
            while (replay_index + 1 < replay_frames.size() && now >= replay_next_tick)
            {
                const auto source_delta = std::chrono::duration_cast<std::chrono::microseconds>(
                    replay_frames[replay_index + 1].ts - replay_frames[replay_index].ts);
                const auto scaled_us = std::clamp<long long>(
                    static_cast<long long>(source_delta.count() / speed), 1000, 500000);
                replay_next_tick = now + std::chrono::microseconds(scaled_us);
                ++replay_index;
            }
            g_replay_playback_frame.store(static_cast<int>(replay_index));

            const int dr = std::max(64, cfg.detection_resolution);
            cv::Mat replayCanvas(dr, dr, CV_8UC3, cv::Scalar(18, 20, 24));
            render_replay_frame(replayCanvas, replay_frames, replay_index, speed);
            cv::imshow(kWindowName, replayCanvas);

            if (replay_index + 1 >= replay_frames.size() && now >= replay_next_tick)
            {
                g_replay_playback_active.store(false);
                replay_was_active = false;
            }
            cv::pollKey();
            std::this_thread::sleep_for(std::chrono::milliseconds(8));
            continue;
        }

        cv::Mat frameCopy;
        {
            std::lock_guard<std::mutex> lk(frameMutex);
            if (!latestFrame.empty()) latestFrame.copyTo(frameCopy);
        }

        if (frameCopy.empty())
        {
            const int dr = std::max(64, cfg.detection_resolution);
            cv::Mat placeholder(dr, dr, CV_8UC3, cv::Scalar(20, 20, 20));
            draw_text_with_bg(placeholder, "Waiting for capture...",
                              cv::Point(10, dr / 2),
                              bgr(220, 220, 220), bgr(0, 0, 0));
            cv::imshow(kWindowName, placeholder);
        }
        else
        {
            {
                std::lock_guard<std::mutex> lk(g_clean_mutex);
                frameCopy.copyTo(g_clean_frame);
            }
            render_overlays(frameCopy, cfg);
            draw_pick_overlay(frameCopy);
            cv::imshow(kWindowName, frameCopy);
        }

        cv::pollKey();
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }

    if (window_open)
        destroy_window_safe();
}
}

void PreviewWindow_Start()
{
    if (g_run.exchange(true))
        return;
    g_thread = std::thread(preview_loop);
}

void PreviewWindow_Stop()
{
    if (!g_run.exchange(false))
        return;
    if (g_thread.joinable())
        g_thread.join();
}
