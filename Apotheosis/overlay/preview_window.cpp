#define WIN32_LEAN_AND_MEAN
#define _WINSOCKAPI_
#include <winsock2.h>
#include <Windows.h>

#include <algorithm>
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
#include "control/controller_contract.h"   // 预览叠加用: 状态码
#include "control/follow_compensator.h"
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
bool       g_pick_locked = false;
int        g_pick_active_token = 0;
int        g_pick_selected_x = -1;
int        g_pick_selected_y = -1;
cv::Rect   g_pick_magnifier_grid;
int        g_pick_magnifier_cell = 0;
int        g_pick_magnifier_origin_x = 0;
int        g_pick_magnifier_origin_y = 0;
cv::Rect   g_display_content;
std::string g_preview_title;
cv::Size g_view_source_size;

// Coordinates stay in capture pixels. Rasterize overlays only after scaling
// the background, so enlarging the window does not enlarge rasterized text.
struct PreviewCanvas
{
    cv::Mat image;
    int cols, rows;
    double scale;

    cv::Rect content;

    PreviewCanvas(cv::Size source, cv::Size display, cv::Scalar background)
        : image(display, CV_8UC3, cv::Scalar(0, 0, 0)), cols(source.width), rows(source.height),
          scale(std::min(double(display.width) / cols, double(display.height) / rows))
    {
        const cv::Size fitted(std::max(1, cvRound(cols * scale)), std::max(1, cvRound(rows * scale)));
        content = cv::Rect((display.width - fitted.width) / 2,
                           (display.height - fitted.height) / 2, fitted.width, fitted.height);
        image(content).setTo(background);
    }
    PreviewCanvas(const cv::Mat& source, cv::Size display)
        : PreviewCanvas(source.size(), display, cv::Scalar(0, 0, 0))
    {
        cv::Mat target = image(content);
        if (content.size() == source.size()) source.copyTo(target);
        else cv::resize(source, target, content.size(), 0, 0,
                        content.width < cols ? cv::INTER_AREA : cv::INTER_CUBIC);
    }

    bool empty() const { return image.empty(); }
    int type() const { return image.type(); }
    int length(int v) const { return std::max(1, cvRound(v * scale)); }
    int stroke(int v) const { return v < 0 ? v : length(v); }
    cv::Point point(cv::Point p) const { return {content.x + cvRound(p.x * scale), content.y + cvRound(p.y * scale)}; }
    cv::Rect rect(cv::Rect r) const {
        const auto a = point(r.tl()), b = point(r.br());
        return cv::Rect(a, b);
    }
};

namespace preview_draw {
void rectangle(PreviewCanvas& c, cv::Rect r, cv::Scalar color, int thickness, int type) {
    cv::rectangle(c.image, c.rect(r), color, c.stroke(thickness), type);
}
void line(PreviewCanvas& c, cv::Point a, cv::Point b, cv::Scalar color, int thickness, int type) {
    cv::line(c.image, c.point(a), c.point(b), color, c.stroke(thickness), type);
}
void circle(PreviewCanvas& c, cv::Point p, int radius, cv::Scalar color, int thickness, int type) {
    cv::circle(c.image, c.point(p), c.length(radius), color, c.stroke(thickness), type);
}
void ellipse(PreviewCanvas& c, cv::Point p, cv::Size axes, double angle,
             double start, double end, cv::Scalar color, int thickness, int type) {
    cv::ellipse(c.image, c.point(p), {c.length(axes.width), c.length(axes.height)},
                angle, start, end, color, c.stroke(thickness), type);
}
void drawMarker(PreviewCanvas& c, cv::Point p, cv::Scalar color, int marker,
                int size, int thickness, int type) {
    cv::drawMarker(c.image, c.point(p), color, marker, c.length(size), c.stroke(thickness), type);
}
}

cv::Size preview_display_size(cv::Size source, bool& initializeSize)
{
    if (initializeSize) {
        cv::resizeWindow(kWindowName, source.width, source.height);
        initializeSize = false;
        return source;
    }
    cv::Rect viewport;
    try { viewport = cv::getWindowImageRect(kWindowName); }
    catch (const cv::Exception&) { return source; }
    if (viewport.width <= 0 || viewport.height <= 0) return source;
    // Win32 HighGUI stretches the entire Mat to the client rectangle. Match
    // that rectangle exactly and letterbox inside the Mat to preserve aspect.
    const double limit = std::min(1.0, 4096.0 / std::max(viewport.width, viewport.height));
    return {std::max(1, cvRound(viewport.width * limit)),
            std::max(1, cvRound(viewport.height * limit))};
}

void show_preview(const PreviewCanvas& canvas)
{

    const auto title = std::string(kWindowName) + " | " + std::to_string(canvas.cols) + "x" +
        std::to_string(canvas.rows) + " | " + std::to_string(cvRound(canvas.scale * 100)) +
        "% | double-click: 1:1";
    if (title != g_preview_title) {
        cv::setWindowTitle(kWindowName, title);
        g_preview_title = title;
    }
    g_view_source_size = cv::Size(canvas.cols, canvas.rows);
    cv::imshow(kWindowName, canvas.image);
    cv::pollKey();
}

cv::Scalar bgr(int b, int g, int r) { return cv::Scalar(b, g, r); }

const char* followStateText(int state)
{
    using F = control::FollowCompensator;
    switch (state) {
    case F::Checking: return "CHECK";
    case F::Reversed: return "TURN";
    case F::Stopped: return "STOP";
    case F::Preset: return "SEED";
    case F::Remembered: return "READY";
    case F::Uncertain: return "UNKNOWN";
    case F::Burst: return "BURST";
    case F::ErrorLearning: return "ERR-LEARN";
    case F::ErrorHolding: return "ERR-HOLD";
    case F::ErrorUnwinding: return "ERR-REDUCE";
    case F::ErrorDisabled: return "OFF";
    case F::ErrorReversed: return "ERR-RESET";
    default: return "LEARN";
    }
}

const char* triggerReasonText(runtime::TriggerOverlayReason reason)
{
    using R = runtime::TriggerOverlayReason;
    switch (reason) {
    case R::Disabled: return "disabled";
    case R::NoTarget: return "no trigger target";
    case R::OutsideZone: return "outside range";
    case R::Ready: return "ready";
    case R::ScopeWait: return "waiting for scope";
    case R::FirstShotDelay: return "first shot delay";
    case R::Cooldown: return "shot interval";
    case R::AutoStopWait: return "auto stop unavailable";
    case R::SwitchBusy: return "weapon switch busy";
    case R::DriverRejected: return "left button send failed";
    case R::Pressed: return "left press sent";
    case R::NoDriver: return "mouse driver unavailable";
    }
    return "unknown";
}

void draw_text_with_bg(PreviewCanvas& canvas, const std::string& text, cv::Point org,
                       const cv::Scalar& fg, const cv::Scalar& bg)
{
    cv::Mat& img = canvas.image;
    org = canvas.point(org);
    int baseline = 0;
    const int font = cv::FONT_HERSHEY_SIMPLEX;
    const double scale = 0.45 * canvas.scale;
    const int thickness = canvas.stroke(1);
    cv::Size sz = cv::getTextSize(text, font, scale, thickness, &baseline);
    cv::Point tl(org.x, org.y - sz.height - canvas.length(3));
    cv::Point br(org.x + sz.width + canvas.length(4), org.y + canvas.length(2));
    cv::rectangle(img, tl, br, bg, cv::FILLED);
    cv::putText(img, text, cv::Point(org.x + canvas.length(2), org.y - canvas.length(2)),
                font, scale, fg, thickness, cv::LINE_AA);
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
    int    crosshair_offset_y = 0;
    int    crosshair_min_pixel_count = 0;
    int    crosshair_close_radius = 0;
    std::vector<crosshair::CrosshairColorBand> crosshair_colors;
    bool   any_color_enabled = false;
    bool   crosshair_hotkey_enabled = false;
    bool   laser_hotkey_enabled = false;
    bool   laser_color_enabled = false;
    int    laser_rect_w = 0, laser_rect_h = 0;
    int    laser_center_x = 0, laser_center_y = 0;
    int    laser_target_center_x = 0, laser_target_center_y = 0;
    int    laser_target_rect_w = 0, laser_target_rect_h = 0;

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
    s.crosshair_offset_y       = config.crosshair_offset_y;
    s.crosshair_min_pixel_count = config.crosshair_min_pixel_count;
    s.crosshair_close_radius   = config.crosshair_close_radius;
    s.laser_rect_w = config.laser_rect_w;
    s.laser_rect_h = config.laser_rect_h;
    s.laser_center_x = config.laser_center_x;
    s.laser_center_y = config.laser_center_y;
    s.laser_target_center_x = config.laser_target_center_x;
    s.laser_target_center_y = config.laser_target_center_y;
    s.laser_target_rect_w = config.laser_target_rect_w;
    s.laser_target_rect_h = config.laser_target_rect_h;
    for (const auto& c : config.laser_colors)
        s.laser_color_enabled = s.laser_color_enabled || c.enabled;
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
        s.laser_hotkey_enabled = hk.laser_detect_enabled && !hk.crosshair_detect_enabled;
    }
    return s;
}

void render_overlays(PreviewCanvas& canvas, const PreviewConfigSnapshot& cfg)
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
        preview_draw::rectangle(canvas, clipped, boxColor, 1, cv::LINE_AA);

        const int cls = (i < classes.size()) ? classes[i] : -1;
        char label[64];
        std::snprintf(label, sizeof(label), "#%d", cls);
        draw_text_with_bg(canvas, label,
                          cv::Point(clipped.x, std::max(12, clipped.y)),
                          textFg, textBg);
    }

    const runtime::AimOverlayState ov = runtime::readAimOverlay();
    const auto age_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - ov.ts).count();
    const bool fresh = ov.ts.time_since_epoch().count() != 0 &&
                       age_ms >= 0 && age_ms <= runtime::kAimOverlayStaleMs;
    if (cfg.fov_base_x > 0 && cfg.fov_base_y > 0)
    {
        const cv::Point center = cfg.hotkey_active && fresh
            ? cv::Point(static_cast<int>(std::lround(ov.cross_x)), static_cast<int>(std::lround(ov.cross_y)))
            : cv::Point(canvas.cols / 2, canvas.rows / 2);
        const cv::Size baseAxes(std::max(1, cfg.fov_base_x / 2),
                                std::max(1, cfg.fov_base_y / 2));

        const cv::Scalar baseCol = bgr(60, 200, 255);
        preview_draw::ellipse(canvas, center, baseAxes, 0, 0, 360, baseCol, 1, cv::LINE_AA);
    }

    // 跟踪器锁定的目标框和最终瞄点；上方绿框是检测器的原始输出。
    {
        if (fresh)
        {
            if (cfg.hotkey_active && ov.fov_radius_x > 0.0 && ov.fov_radius_y > 0.0) {
                preview_draw::ellipse(canvas,
                    cv::Point(static_cast<int>(std::lround(ov.cross_x)), static_cast<int>(std::lround(ov.cross_y))),
                    cv::Size(std::max(1, static_cast<int>(std::lround(ov.fov_radius_x))),
                             std::max(1, static_cast<int>(std::lround(ov.fov_radius_y)))),
                    0, 0, 360, bgr(255, 180, 80), 1, cv::LINE_AA);
                if (ov.mask_x || ov.mask_y || ov.unlock_x || ov.unlock_y) {
                    draw_text_with_bg(canvas, std::string("Input mask requested: ") +
                        (ov.mask_x ? "X " : "") + (ov.mask_y ? "Y " : "") + " | Aim unlock: " +
                        (ov.unlock_x ? "X " : "") + (ov.unlock_y ? "Y" : ""),
                        cv::Point(6, 108), bgr(80, 180, 255), bgr(0, 0, 0));
                }
            }
            const auto verdict = static_cast<control::TrackLockState>(ov.verdict);
            cv::Scalar lockCol = bgr(90, 220, 90);
            const char* verdictText = "OK";
            switch (verdict)
            {
            case control::TrackLockState::Existing: lockCol = bgr(90, 220, 90);  verdictText = "TRACKED"; break;
            case control::TrackLockState::New:      lockCol = bgr(80, 190, 255); verdictText = "NEW";     break;
            }

            if (ov.engaged && ov.box.width > 0 && ov.box.height > 0)
            {
                const cv::Rect r(ov.box.x, ov.box.y, ov.box.width, ov.box.height);
                const cv::Rect clipped = r & cv::Rect(0, 0, canvas.cols, canvas.rows);
                if (clipped.area() > 0)
                {
                    // 跟踪框和瞄点。
                    preview_draw::rectangle(canvas, clipped, lockCol, 2, cv::LINE_AA);

                    char label[96];
                    std::snprintf(label, sizeof(label), "LOCK #%d id=%d %s",
                                  ov.target_class_id, ov.target_id, verdictText);
                    draw_text_with_bg(canvas, label,
                                      cv::Point(clipped.x, std::max(12, clipped.y)),
                                      bgr(250, 245, 240), bgr(0, 0, 0));

                    // 青色十字是原瞄点，橙色圆是实际交给 PID 的补偿瞄点。
                    const cv::Point aim(static_cast<int>(std::lround(ov.anchor_x)),
                                        static_cast<int>(std::lround(ov.anchor_y)));
                    const cv::Point base(static_cast<int>(std::lround(ov.base_anchor_x)),
                                         static_cast<int>(std::lround(ov.base_anchor_y)));
                    if (ov.follow_strength_x > 0.0 || ov.follow_strength_y > 0.0)
                    {
                        preview_draw::line(canvas, base, aim, bgr(255, 220, 0), 1, cv::LINE_AA);
                        preview_draw::drawMarker(canvas, base, bgr(255, 220, 0),
                                       cv::MARKER_CROSS, 10, 1, cv::LINE_AA);
                    }
                    preview_draw::circle(canvas, aim, 4, bgr(0, 140, 255), 2, cv::LINE_AA);
                }
            }

            if (ov.trigger_valid && ov.trigger_half_width > 0.0 && ov.trigger_half_height > 0.0)
            {
                const cv::Rect zone(
                    static_cast<int>(std::lround(ov.trigger_point_x - ov.trigger_half_width)),
                    static_cast<int>(std::lround(ov.trigger_point_y - ov.trigger_half_height)),
                    std::max(1, static_cast<int>(std::lround(2.0 * ov.trigger_half_width))),
                    std::max(1, static_cast<int>(std::lround(2.0 * ov.trigger_half_height))));
                const cv::Rect clipped = zone & cv::Rect(0, 0, canvas.cols, canvas.rows);
                if (clipped.area() > 0) {
                    preview_draw::rectangle(canvas, clipped, bgr(255, 80, 220), 2, cv::LINE_AA);
                    preview_draw::drawMarker(canvas,
                        cv::Point(static_cast<int>(std::lround(ov.trigger_point_x)),
                                  static_cast<int>(std::lround(ov.trigger_point_y))),
                        bgr(255, 80, 220), cv::MARKER_CROSS, 12, 2, cv::LINE_AA);
                    char triggerLabel[96];
                    std::snprintf(triggerLabel, sizeof(triggerLabel), "TRIGGER #%d  %.0f x %.0f px",
                        ov.trigger_class_id, 2.0 * ov.trigger_half_width,
                        2.0 * ov.trigger_half_height);
                    draw_text_with_bg(canvas, triggerLabel,
                        cv::Point(clipped.x, std::max(12, clipped.y)),
                        bgr(255, 80, 220), bgr(0, 0, 0));
                }
                // This marker is the point used by TriggerTarget::contains().
                // The pink box alone only shows the selected target range.
                preview_draw::drawMarker(canvas,
                    cv::Point(static_cast<int>(std::lround(ov.cross_x)),
                              static_cast<int>(std::lround(ov.cross_y))),
                    ov.trigger_in_zone ? bgr(70, 230, 70) : bgr(60, 80, 255),
                    cv::MARKER_TILTED_CROSS, 14, 2, cv::LINE_AA);
            }

            if (ov.trigger_reason != runtime::TriggerOverlayReason::Disabled)
            {
                char triggerStatus[160];
                std::snprintf(triggerStatus, sizeof(triggerStatus), "TRIGGER: %s | %s",
                    ov.trigger_in_zone ? "IN" : "OUT", triggerReasonText(ov.trigger_reason));
                draw_text_with_bg(canvas, triggerStatus, cv::Point(6, canvas.rows - 12),
                    ov.trigger_in_zone ? bgr(70, 230, 70) : bgr(60, 80, 255), bgr(0, 0, 0));
            }

            // 状态行说明锁定身份和未输出原因。
            {
                char line[240];
                if (ov.engaged)
                {
                    std::snprintf(line, sizeof(line), "Error rate(px/s) X/Y: %+.1f / %+.1f",
                                  ov.follow_motion_x, ov.follow_motion_y);
                    draw_text_with_bg(canvas, line, cv::Point(6, canvas.rows - 138),
                                      bgr(245, 245, 245), bgr(0, 0, 0));
                    std::snprintf(line, sizeof(line), "Follow X/Y: %s / %s",
                                  followStateText(ov.follow_state_x), followStateText(ov.follow_state_y));
                    draw_text_with_bg(canvas, line, cv::Point(6, canvas.rows - 120),
                                      bgr(100, 255, 180), bgr(0, 0, 0));
                    std::snprintf(line, sizeof(line), "Follow mode: error only");
                    draw_text_with_bg(canvas, line, cv::Point(6, canvas.rows - 102),
                                      bgr(100, 255, 180), bgr(0, 0, 0));
                    std::snprintf(line, sizeof(line), "Follow gain X/Y: %.1f / %.1f",
                                  ov.follow_strength_x, ov.follow_strength_y);
                    draw_text_with_bg(canvas, line, cv::Point(6, canvas.rows - 84),
                                      bgr(245, 245, 245), bgr(0, 0, 0));
                    std::snprintf(line, sizeof(line), "Shift(px) X/Y: %+.1f / %+.1f",
                                  ov.anchor_x - ov.base_anchor_x,
                                  ov.anchor_y - ov.base_anchor_y);
                    draw_text_with_bg(canvas, line, cv::Point(6, canvas.rows - 66),
                                      bgr(0, 180, 255), bgr(0, 0, 0));
                    std::snprintf(line, sizeof(line), "Base err(px) X/Y: %+.1f / %+.1f",
                                  ov.base_error_x, ov.base_error_y);
                    draw_text_with_bg(canvas, line, cv::Point(6, canvas.rows - 48),
                                      bgr(255, 220, 0), bgr(0, 0, 0));
                    std::snprintf(line, sizeof(line),
                                  "Track: %s | id=%d #%d%s",
                                  verdictText, ov.target_id, ov.target_class_id,
                                  ov.scope_params ? " | SCOPE-PARAMS" :
                                  ov.secondary_params ? " | SECONDARY" : " | DEFAULT");
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
                    case Idle::BadDt:                 why = "bad-dt";         break;
                    case Idle::None:                  why = "idle";           break;
                    }
                    std::snprintf(line, sizeof(line), "Track: not locked | %s%s",
                                  why, ov.scope_params ? " | SCOPE-PARAMS" : "");
                }
                draw_text_with_bg(canvas, line, cv::Point(6, canvas.rows - 30),
                                  bgr(245, 245, 245), bgr(0, 0, 0));
            }
        }
    }

    if (cfg.crosshair_hotkey_enabled && cfg.crosshair_rect_w > 0 && cfg.crosshair_rect_h > 0)
    {
        const int rw = std::max(4, cfg.crosshair_rect_w);
        const int rh = std::max(4, cfg.crosshair_rect_h);
        const int x = std::clamp(canvas.cols / 2 - rw / 2, 0, std::max(0, canvas.cols - rw));
        const int y = std::clamp(canvas.rows / 2 - rh + 10 + cfg.crosshair_offset_y,
                                 0, std::max(0, canvas.rows - rh));
        const cv::Rect roi(x, y, rw, rh);
        const cv::Rect clipped = roi & cv::Rect(0, 0, canvas.cols, canvas.rows);
        if (clipped.area() > 0)
            preview_draw::rectangle(canvas, clipped, bgr(255, 190, 0), 1, cv::LINE_AA);
    }
    if (cfg.laser_hotkey_enabled) {
        const cv::Rect roi(cfg.laser_center_x - cfg.laser_rect_w / 2,
                           cfg.laser_center_y - cfg.laser_rect_h / 2,
                           cfg.laser_rect_w, cfg.laser_rect_h);
        const cv::Rect target(cfg.laser_target_center_x - cfg.laser_target_rect_w / 2,
                              cfg.laser_target_center_y - cfg.laser_target_rect_h / 2,
                              cfg.laser_target_rect_w, cfg.laser_target_rect_h);
        const cv::Rect bounds(0, 0, canvas.cols, canvas.rows);
        if ((roi & bounds).area() > 0)
            preview_draw::rectangle(canvas, roi & bounds, bgr(0, 150, 255), 1, cv::LINE_AA);
        if ((target & bounds).area() > 0)
            preview_draw::rectangle(canvas, target & bounds, bgr(255, 100, 230), 1, cv::LINE_AA);
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
        const bool using_laser = cfg.laser_hotkey_enabled;
        const char* mode = using_laser ? "Laser" : "Xhair";
        const bool palette_enabled = using_laser ? cfg.laser_color_enabled : cfg.any_color_enabled;
        if (!palette_enabled || (!cfg.crosshair_hotkey_enabled && !using_laser))
        {
            std::snprintf(line, sizeof(line), "%s: OFF (hotkey/palette off)", mode);
        }
        else if (snap.valid)
        {
            std::snprintf(line, sizeof(line),
                          "%s: HIT (%d,%d) age=%lldms%s | %s", mode,
                          static_cast<int>(std::lround(snap.x)),
                          static_cast<int>(std::lround(snap.y)),
                          age_ms,
                          (age_ms > crosshair_runtime::kFreshnessMs) ? " STALE" : "",
                          ref_text);
        }
        else
        {
            std::snprintf(line, sizeof(line), "%s: MISS | %s", mode, ref_text);
        }

        const cv::Scalar col = snap.valid ? bgr(80, 255, 80) : bgr(150, 150, 150);
        draw_text_with_bg(canvas, line, cv::Point(6, canvas.rows - 6),
                          bgr(245, 245, 245), bgr(0, 0, 0));

        if (snap.valid)
        {
            const cv::Point p(static_cast<int>(std::lround(snap.x)),
                              static_cast<int>(std::lround(snap.y)));
            preview_draw::drawMarker(canvas, p, col, cv::MARKER_CROSS, 14, 1, cv::LINE_AA);
            preview_draw::circle(canvas, p, 6, col, 1, cv::LINE_AA);
        }

        if (ref_fresh)
        {
            const cv::Point r(static_cast<int>(std::lround(ref.x)),
                              static_cast<int>(std::lround(ref.y)));
            preview_draw::rectangle(canvas, cv::Rect(r.x - 7, r.y - 7, 15, 15),
                          bgr(255, 120, 240), 1, cv::LINE_AA);
        }

        preview_draw::drawMarker(canvas, cv::Point(canvas.cols / 2, canvas.rows / 2),
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
    const int pickToken = crosshair::ArmedToken();
    if (pickToken != g_pick_active_token) {
        g_pick_active_token = pickToken;
        g_pick_locked = false;
        g_pick_cursor_inside = false;
        g_pick_magnifier_grid = {};
        g_pick_magnifier_cell = 0;
    }
    if (event == cv::EVENT_LBUTTONDBLCLK && !crosshair::IsColorPickArmed() &&
        g_view_source_size.width > 0 && g_view_source_size.height > 0) {
        cv::resizeWindow(kWindowName, g_view_source_size.width, g_view_source_size.height);
        return;
    }
    if (event == cv::EVENT_RBUTTONDOWN && crosshair::IsColorPickArmed()) {
        crosshair::CancelColorPick();
        return;
    }
    int sourceX = -1;
    int sourceY = -1;
    bool overMagnifier = false;
    {
        std::lock_guard<std::mutex> lk(g_clean_mutex);
        if (g_clean_frame.empty() || g_display_content.width <= 0 || g_display_content.height <= 0) {
            g_pick_cursor_inside = false;
            return;
        }
        overMagnifier = crosshair::IsColorPickArmed() &&
            g_pick_magnifier_cell > 0 &&
            g_pick_magnifier_grid.contains(cv::Point(x, y));
        if (overMagnifier) {
            const int cellX = (x - g_pick_magnifier_grid.x) / g_pick_magnifier_cell;
            const int cellY = (y - g_pick_magnifier_grid.y) / g_pick_magnifier_cell;
            sourceX = std::clamp(g_pick_magnifier_origin_x + cellX, 0, g_clean_frame.cols - 1);
            sourceY = std::clamp(g_pick_magnifier_origin_y + cellY, 0, g_clean_frame.rows - 1);
        } else {
            sourceX = cvFloor(double(x - g_display_content.x) * g_clean_frame.cols / g_display_content.width);
            sourceY = cvFloor(double(y - g_display_content.y) * g_clean_frame.rows / g_display_content.height);
        }
        if (sourceX < 0 || sourceY < 0 ||
            sourceX >= g_clean_frame.cols || sourceY >= g_clean_frame.rows) {
            g_pick_cursor_inside = false;
            return;
        }
    }
    if (event == cv::EVENT_MOUSEMOVE)
    {
        if (!overMagnifier && !g_pick_locked) {
            g_pick_cursor_x = sourceX;
            g_pick_cursor_y = sourceY;
        }
        if (overMagnifier || !g_pick_locked) {
            g_pick_selected_x = sourceX;
            g_pick_selected_y = sourceY;
        }
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
        if (!overMagnifier) {
            g_pick_cursor_x = sourceX;
            g_pick_cursor_y = sourceY;
            g_pick_selected_x = sourceX;
            g_pick_selected_y = sourceY;
            g_pick_cursor_inside = true;
            g_pick_locked = true;
            return;
        }
        if (!g_pick_locked) return;
        g_pick_selected_x = sourceX;
        g_pick_selected_y = sourceY;
        cv::Mat clean;
        {
            std::lock_guard<std::mutex> lk(g_clean_mutex);
            if (!g_clean_frame.empty()) g_clean_frame.copyTo(clean);
        }
        int h = 0, s = 0, v = 0;
        if (crosshair::SampleRegionHSV(clean, sourceX, sourceY, 0, h, s, v))
            crosshair::SubmitPickedColor(h, s, v);
    }
}

void draw_pick_overlay(PreviewCanvas& canvas)
{
    if (canvas.empty() || canvas.type() != CV_8UC3) return;
    if (!crosshair::IsColorPickArmed()) {
        std::lock_guard<std::mutex> lk(g_clean_mutex);
        g_pick_active_token = 0;
        g_pick_locked = false;
        g_pick_magnifier_grid = {};
        g_pick_magnifier_cell = 0;
        return;
    }

    if (g_pick_active_token != crosshair::ArmedToken()) {
        g_pick_active_token = crosshair::ArmedToken();
        g_pick_locked = false;
        g_pick_cursor_inside = false;
        g_pick_magnifier_grid = {};
        g_pick_magnifier_cell = 0;
    }

    draw_text_with_bg(canvas, "PICK: click image to lock; right-click cancel",
                      cv::Point(6, canvas.rows - 8), bgr(245, 245, 245), bgr(0, 0, 0));

    if (!g_pick_cursor_inside) {
        std::lock_guard<std::mutex> lk(g_clean_mutex);
        g_pick_magnifier_grid = {};
        g_pick_magnifier_cell = 0;
        return;
    }

    const int cx = g_pick_cursor_x;
    const int cy = g_pick_cursor_y;
    preview_draw::rectangle(canvas, cv::Rect(cx, cy, 1, 1), bgr(0, 220, 255), 1, cv::LINE_8);

    constexpr int kPixels = 11;
    constexpr int kHalf = kPixels / 2;
    const int cell = std::clamp((std::min(canvas.image.cols, canvas.image.rows) - 48) / kPixels, 6, 16);
    const int gridSize = cell * kPixels;
    const cv::Rect grid(canvas.image.cols - gridSize - 8, 24, gridSize, gridSize);
    if (grid.x < 2 || grid.y + grid.height + 20 >= canvas.image.rows) return;

    cv::Mat pixels(kPixels, kPixels, CV_8UC3);
    {
        std::lock_guard<std::mutex> lk(g_clean_mutex);
        if (g_clean_frame.empty() || cx < 0 || cy < 0 ||
            cx >= g_clean_frame.cols || cy >= g_clean_frame.rows) return;
        for (int py = 0; py < kPixels; ++py) {
            const int sy = std::clamp(cy + py - kHalf, 0, g_clean_frame.rows - 1);
            for (int px = 0; px < kPixels; ++px) {
                const int sx = std::clamp(cx + px - kHalf, 0, g_clean_frame.cols - 1);
                pixels.at<cv::Vec3b>(py, px) = g_clean_frame.at<cv::Vec3b>(sy, sx);
            }
        }
        g_pick_magnifier_grid = grid;
        g_pick_magnifier_cell = cell;
        g_pick_magnifier_origin_x = cx - kHalf;
        g_pick_magnifier_origin_y = cy - kHalf;
    }

    cv::Mat gridView = canvas.image(grid);
    cv::resize(pixels, gridView, grid.size(), 0, 0, cv::INTER_NEAREST);
    for (int i = 1; i < kPixels; ++i) {
        cv::line(canvas.image, {grid.x + i * cell, grid.y},
                 {grid.x + i * cell, grid.y + gridSize}, bgr(75, 75, 75), 1);
        cv::line(canvas.image, {grid.x, grid.y + i * cell},
                 {grid.x + gridSize, grid.y + i * cell}, bgr(75, 75, 75), 1);
    }
    cv::rectangle(canvas.image, grid, bgr(255, 255, 255), 2);
    const int selectedX = g_pick_selected_x - (cx - kHalf);
    const int selectedY = g_pick_selected_y - (cy - kHalf);
    if (selectedX >= 0 && selectedX < kPixels && selectedY >= 0 && selectedY < kPixels) {
        const cv::Rect selected(grid.x + selectedX * cell, grid.y + selectedY * cell, cell, cell);
        cv::rectangle(canvas.image, selected, bgr(0, 220, 255), 2);
    }
    draw_text_with_bg(canvas, "11x 1-pixel picker", {grid.x, grid.y - 5},
                      bgr(245, 245, 245), bgr(0, 0, 0));
    draw_text_with_bg(canvas, g_pick_locked ? "Click a square to sample" : "Click image to lock view",
                      {grid.x, grid.y + gridSize + 15},
                      bgr(245, 245, 245), bgr(0, 0, 0));
}

void draw_color_lab_preview(PreviewCanvas& canvas)
{
    const auto bands = crosshair::ColorLabPreviewBands();
    if (bands.empty() || canvas.empty() || canvas.type() != CV_8UC3) return;
    cv::Mat background = canvas.image(canvas.content);
    cv::Mat hsv, combined = cv::Mat::zeros(background.size(), CV_8UC1);
    cv::cvtColor(background, hsv, cv::COLOR_BGR2HSV);
    for (const auto& band : bands) {
        cv::Mat match;
        cv::inRange(hsv, cv::Scalar(band.h_low, band.s_min, band.v_min),
                    cv::Scalar(band.h_high, band.s_max, band.v_max), match);
        cv::bitwise_or(combined, match, combined);
    }
    cv::Mat highlighted = background.clone();
    highlighted.setTo(bgr(45, 220, 70), combined);
    cv::addWeighted(background, 0.55, highlighted, 0.45, 0, background);
    draw_text_with_bg(canvas, "COLOR LAB: green = matched pixel", {6, canvas.rows - 28},
                      bgr(120, 255, 140), bgr(0, 0, 0));
}

void render_replay_frame(PreviewCanvas& canvas,
                         const std::vector<runtime::ReplayFrame>& frames,
                         size_t frame_index,
                         float playback_speed)
{
    if (canvas.empty() || frames.empty()) return;
    frame_index = std::min(frame_index, frames.size() - 1);
    const auto& frame = frames[frame_index];
    if (frame.fov_radius_x > 0.0 && frame.fov_radius_y > 0.0) {
        preview_draw::ellipse(canvas,
            cv::Point(static_cast<int>(std::lround(frame.cross_x)), static_cast<int>(std::lround(frame.cross_y))),
            cv::Size(std::max(1, static_cast<int>(std::lround(frame.fov_radius_x))),
                     std::max(1, static_cast<int>(std::lround(frame.fov_radius_y)))),
            0, 0, 360, bgr(255, 180, 80), 1, cv::LINE_AA);
    }
    if (frame.mask_x || frame.mask_y || frame.unlock_x || frame.unlock_y) {
        draw_text_with_bg(canvas, std::string("Input mask requested: ") +
            (frame.mask_x ? "X " : "") + (frame.mask_y ? "Y " : "") + " | Aim unlock: " +
            (frame.unlock_x ? "X " : "") + (frame.unlock_y ? "Y" : ""),
            cv::Point(6, 108), bgr(80, 180, 255), bgr(0, 0, 0));
    }

    // Draw raw detections first so the selected track remains visible on top.
    for (int pass = 0; pass < 2; ++pass)
    for (size_t i = 0; i < frame.boxes.size(); ++i)
    {
        const int track_id = i < frame.track_ids.size() ? frame.track_ids[i] : -1;
        const bool locked = track_id >= 0 && track_id == frame.locked_track_id;
        if (locked != (pass == 1)) continue;
        const cv::Rect clipped = frame.boxes[i] & cv::Rect(0, 0, canvas.cols, canvas.rows);
        if (clipped.area() <= 0) continue;
        const cv::Scalar color = locked ? bgr(70, 90, 255) : bgr(110, 220, 80);
        preview_draw::rectangle(canvas, clipped, color, locked ? 3 : 1, cv::LINE_AA);
        const int cls = i < frame.class_ids.size() ? frame.class_ids[i] : -1;
        char label[64];
        std::snprintf(label, sizeof(label), locked ? "LOCK #%d" : "#%d", cls);
        draw_text_with_bg(canvas, label, cv::Point(clipped.x, std::max(12, clipped.y)),
                          bgr(250, 245, 240), bgr(0, 0, 0));
    }

    const size_t first = frame_index > 90 ? frame_index - 90 : 0;
    for (size_t i = first + 1; i <= frame_index; ++i)
    {
        if (frames[i].locked_track_id < 0 ||
            frames[i].locked_track_id != frames[i - 1].locked_track_id)
            continue;
        preview_draw::line(canvas,
                 cv::Point(static_cast<int>(std::lround(frames[i - 1].pivot_x)),
                           static_cast<int>(std::lround(frames[i - 1].pivot_y))),
                 cv::Point(static_cast<int>(std::lround(frames[i].pivot_x)),
                           static_cast<int>(std::lround(frames[i].pivot_y))),
                 bgr(0, 140, 255), 2, cv::LINE_AA);
    }
    const cv::Point cross(static_cast<int>(std::lround(frame.cross_x)),
                          static_cast<int>(std::lround(frame.cross_y)));
    preview_draw::drawMarker(canvas, cross, bgr(255, 255, 255), cv::MARKER_CROSS,
                   12, 1, cv::LINE_AA);
    if (frame.locked_track_id >= 0)
    {
        const cv::Point p(static_cast<int>(std::lround(frame.pivot_x)),
                          static_cast<int>(std::lround(frame.pivot_y)));
        preview_draw::line(canvas, cross, p, bgr(100, 130, 180), 1, cv::LINE_AA);
        const cv::Point base(static_cast<int>(std::lround(frame.base_anchor_x)),
                             static_cast<int>(std::lround(frame.base_anchor_y)));
        if (frame.follow_strength_x > 0.0 || frame.follow_strength_y > 0.0)
        {
            preview_draw::line(canvas, base, p, bgr(255, 220, 0), 1, cv::LINE_AA);
            preview_draw::drawMarker(canvas, base, bgr(255, 220, 0),
                           cv::MARKER_CROSS, 10, 1, cv::LINE_AA);
        }
        preview_draw::circle(canvas, p, 5, bgr(0, 0, 0), 3, cv::LINE_AA);
        preview_draw::circle(canvas, p, 5, bgr(0, 140, 255), 2, cv::LINE_AA);
    }

    char banner[128];
    std::snprintf(banner, sizeof(banner), "REPLAY %.2fx | %zu / %zu",
                  playback_speed, frame_index + 1, frames.size());
    draw_text_with_bg(canvas, banner, cv::Point(6, 18),
                      bgr(245, 245, 245), bgr(0, 0, 0));
    char diagnostics[160];
    std::snprintf(diagnostics, sizeof(diagnostics), "PID err %+.1f/%+.1f  OUT %d/%d",
                  frame.error_x, frame.error_y, frame.requested_dx, frame.requested_dy);
    draw_text_with_bg(canvas, diagnostics, cv::Point(6, 36),
                      bgr(245, 245, 245), bgr(0, 0, 0));
    std::snprintf(diagnostics, sizeof(diagnostics), "SENT(prev) %d/%d  ID %d",
                  frame.mouse_dx, frame.mouse_dy, frame.locked_track_id);
    draw_text_with_bg(canvas, diagnostics, cv::Point(6, 54),
                      bgr(245, 245, 245), bgr(0, 0, 0));
    std::snprintf(diagnostics, sizeof(diagnostics), "D raw %+.2f/%+.2f",
                  frame.derivative_raw_x, frame.derivative_raw_y);
    draw_text_with_bg(canvas, diagnostics, cv::Point(6, 72),
                      bgr(245, 245, 245), bgr(0, 0, 0));
    std::snprintf(diagnostics, sizeof(diagnostics), "Params: %s%s",
                  frame.scope_params ? "SCOPE" : frame.secondary_params ? "SECONDARY" : "DEFAULT",
                  frame.locked_track_id < 0 ? " | NO TARGET" : "");
    draw_text_with_bg(canvas, diagnostics, cv::Point(6, 90),
                      bgr(245, 245, 245), bgr(0, 0, 0));
    if (frame.locked_track_id >= 0)
    {
        // All values come from this recorded frame, never the live overlay or
        // current settings (which may have changed since recording).
        const bool errorOnly = frame.follow_state_x >= control::FollowCompensator::ErrorLearning;
        std::snprintf(diagnostics, sizeof(diagnostics), errorOnly
                      ? "Error rate(px/s) X/Y: %+.1f / %+.1f"
                      : "Motion(est) X/Y: %+.1f / %+.1f",
                      frame.follow_motion_x, frame.follow_motion_y);
        draw_text_with_bg(canvas, diagnostics, cv::Point(6, canvas.rows - 138),
                          bgr(245, 245, 245), bgr(0, 0, 0));
        std::snprintf(diagnostics, sizeof(diagnostics), "Follow X/Y: %s / %s",
                      followStateText(frame.follow_state_x), followStateText(frame.follow_state_y));
        draw_text_with_bg(canvas, diagnostics, cv::Point(6, canvas.rows - 120),
                          bgr(100, 255, 180), bgr(0, 0, 0));
        std::snprintf(diagnostics, sizeof(diagnostics), errorOnly
                      ? "Follow mode: error only" : "Seed(px) X/Y: %+.1f / %+.1f",
                      frame.follow_preset_x, frame.follow_preset_y);
        draw_text_with_bg(canvas, diagnostics, cv::Point(6, canvas.rows - 102),
                          bgr(100, 255, 180), bgr(0, 0, 0));
        std::snprintf(diagnostics, sizeof(diagnostics), "Follow gain X/Y: %.1f / %.1f",
                      frame.follow_strength_x, frame.follow_strength_y);
        draw_text_with_bg(canvas, diagnostics, cv::Point(6, canvas.rows - 84),
                          bgr(245, 245, 245), bgr(0, 0, 0));
        std::snprintf(diagnostics, sizeof(diagnostics), "Shift(px) X/Y: %+.1f / %+.1f",
                      frame.pivot_x - frame.base_anchor_x,
                      frame.pivot_y - frame.base_anchor_y);
        draw_text_with_bg(canvas, diagnostics, cv::Point(6, canvas.rows - 66),
                          bgr(0, 180, 255), bgr(0, 0, 0));
        std::snprintf(diagnostics, sizeof(diagnostics), "Base err(px) X/Y: %+.1f / %+.1f",
                      frame.base_anchor_x - frame.cross_x,
                      frame.base_anchor_y - frame.cross_y);
        draw_text_with_bg(canvas, diagnostics, cv::Point(6, canvas.rows - 48),
                          bgr(255, 220, 0), bgr(0, 0, 0));
    }
}

void preview_loop()
{
    bool window_open = false;
    bool initializeSize = true;
    bool replay_was_active = false;
    unsigned int replay_request_seen = 0;
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
                initializeSize = true;
                g_preview_title.clear();
                window_open = true;
            } catch (...) {
                window_open = false;
                std::this_thread::sleep_for(std::chrono::milliseconds(300));
                continue;
            }
        }

        bool replay_active = g_replay_playback_active.load();
        const unsigned int replay_request = g_replay_playback_request.load();
        if (replay_active && (!replay_was_active || replay_request != replay_request_seen))
        {
            replay_request_seen = replay_request;
            replay_frames = runtime::ReplayBuffer::instance().snapshot();
            replay_index = 0;
            g_replay_playback_frame.store(0);
            g_replay_playback_total.store(static_cast<int>(replay_frames.size()));
            if (replay_frames.empty())
            {
                replay_active = false;
                g_replay_playback_active.store(false);
            }
            else
            {
                const auto now = std::chrono::steady_clock::now();
                replay_next_tick = now + std::chrono::milliseconds(250);
                if (replay_frames.size() > 1)
                {
                    const float speed = std::clamp(cfg.replay_playback_speed, 0.05f, 2.0f);
                    const auto delta = std::chrono::duration_cast<std::chrono::microseconds>(
                        replay_frames[1].ts - replay_frames[0].ts);
                    replay_next_tick = now + std::chrono::microseconds(
                        std::clamp<long long>(static_cast<long long>(delta.count() / speed),
                                              1000, 500000));
                }
            }
        }
        replay_was_active = replay_active;
        if (!replay_active && !replay_frames.empty())
        {
            replay_frames.clear();
            g_replay_playback_total.store(0);
        }

        if (replay_active && !replay_frames.empty())
        {
            const float speed = std::clamp(cfg.replay_playback_speed, 0.05f, 2.0f);
            const auto now = std::chrono::steady_clock::now();
            while (replay_index + 1 < replay_frames.size() && now >= replay_next_tick)
            {
                ++replay_index;
                if (replay_index + 1 < replay_frames.size())
                {
                    const auto delta = std::chrono::duration_cast<std::chrono::microseconds>(
                        replay_frames[replay_index + 1].ts - replay_frames[replay_index].ts);
                    replay_next_tick += std::chrono::microseconds(
                        std::clamp<long long>(static_cast<long long>(delta.count() / speed),
                                              1000, 500000));
                }
                else
                    replay_next_tick = now + std::chrono::milliseconds(250);
            }
            g_replay_playback_frame.store(static_cast<int>(replay_index));

            const int dr = std::max(64, replay_frames[replay_index].resolution);
            const cv::Size source(dr, dr);
            PreviewCanvas replayCanvas(source, preview_display_size(source, initializeSize), cv::Scalar(18, 20, 24));
            {
                std::lock_guard<std::mutex> lk(g_clean_mutex);
                g_clean_frame.release();
                g_display_content = {};
                g_pick_cursor_inside = false;
            }
            render_replay_frame(replayCanvas, replay_frames, replay_index, speed);
            show_preview(replayCanvas);

            if (replay_index + 1 >= replay_frames.size() && now >= replay_next_tick)
            {
                g_replay_playback_active.store(false);
                replay_was_active = false;
                replay_frames.clear();
                g_replay_playback_total.store(0);
            }
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
            const cv::Size source(dr, dr);
            PreviewCanvas placeholder(source, preview_display_size(source, initializeSize), cv::Scalar(20, 20, 20));
            {
                std::lock_guard<std::mutex> lk(g_clean_mutex);
                g_clean_frame.release();
                g_display_content = {};
            }
            draw_text_with_bg(placeholder, "Waiting for capture...",
                              cv::Point(10, dr / 2),
                              bgr(220, 220, 220), bgr(0, 0, 0));
            show_preview(placeholder);
        }
        else
        {
            PreviewCanvas canvas(frameCopy, preview_display_size(frameCopy.size(), initializeSize));
            {
                std::lock_guard<std::mutex> lk(g_clean_mutex);
                frameCopy.copyTo(g_clean_frame);
                g_display_content = canvas.content;
            }
            draw_color_lab_preview(canvas);
            render_overlays(canvas, cfg);
            draw_pick_overlay(canvas);
            show_preview(canvas);
        }

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
