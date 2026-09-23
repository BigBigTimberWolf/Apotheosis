#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define _WINSOCKAPI_
#include <winsock2.h>
#include <Windows.h>
#endif

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "config.h"
#define SI_NO_CONVERSION
#include "modules/SimpleIni.h"

namespace
{

std::string to_bool_str(bool v)
{
    return v ? "true" : "false";
}

std::string bucket_to_str(ClassBucket b)
{
    switch (b)
    {
    case ClassBucket::Filter: return "filter";
    case ClassBucket::Aim:    return "aim";
    case ClassBucket::Delete:
    default:                  return "delete";
    }
}

ClassBucket bucket_from_str(const std::string& s, ClassBucket fallback = ClassBucket::Delete)
{
    if (s == "filter" || s == "1") return ClassBucket::Filter;
    if (s == "aim"    || s == "2") return ClassBucket::Aim;
    if (s == "delete" || s == "0") return ClassBucket::Delete;
    return fallback;
}

void apply_default_hotkey(HotkeyProfile& hk)
{
    hk.name = "Aim";
    hk.group = u8"默认";
    hk.keys = { "RightMouseButton" };
    hk.aim_classes.clear();
}

std::string serialize_aim_classes(const std::vector<HotkeyAimClass>& classes)
{
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(3);
    for (size_t i = 0; i < classes.size(); ++i)
    {
        if (i) oss << ';';
        oss << classes[i].class_id
            << ':' << classes[i].y_offset
            << ':' << classes[i].y_offset_max
            << ':' << classes[i].min_conf;
    }
    return oss.str();
}

std::vector<HotkeyAimClass> parse_aim_classes(const std::string& raw)
{
    std::vector<HotkeyAimClass> out;
    std::stringstream ss(raw);
    std::string tok;
    while (std::getline(ss, tok, ';'))
    {
        auto p1 = tok.find(':');
        if (p1 == std::string::npos) continue;
        try
        {
            HotkeyAimClass c;
            c.class_id = std::stoi(tok.substr(0, p1));
            std::vector<float> values;
            std::stringstream value_stream(tok.substr(p1 + 1));
            std::string value;
            while (std::getline(value_stream, value, ':'))
                values.push_back(std::stof(value));
            if (values.empty()) continue;

            c.y_offset = values[0];
            if (values.size() >= 3)
            {
                c.y_offset_max = values[1];
                c.min_conf = values[2];
            }
            else
            {
                c.y_offset_max = c.y_offset;
                c.min_conf = (values.size() == 2) ? values[1] : 0.0f;
            }
            c.y_offset = std::clamp(c.y_offset, 0.0f, 1.0f);
            c.y_offset_max = std::clamp(c.y_offset_max, 0.0f, 1.0f);
            if (c.y_offset > c.y_offset_max)
                std::swap(c.y_offset, c.y_offset_max);
            c.min_conf = std::clamp(c.min_conf, 0.0f, 1.0f);
            out.push_back(c);
        }
        catch (...) {   }
    }
    return out;
}

// ── 自定义手绘曲线采样点 ─────────────────────────────────────────────────
// 格式: 逗号分隔的定点整数, 每个值 = round(y * 10000)，y ∈ [-1, 1]。
// ★ 为什么用定点整数而不是浮点文本:
//   32768 个点, 写成 "-0.1234" 是 8 字符/点 ≈ 256KB, 写成整数 "1234" 是 5 字符
//   ≈ 160KB, 且没有小数点解析开销。精度 1e-4 对"手绘路径"完全够 ——
//   曲线的 Y 是【横向偏移比例】, 1e-4 远小于一个像素在框上的比例。
// ★ 存到 ini 时走 SimpleIni 的多行值 (见 Config::save), 不让单行超长。
std::string serialize_custom_samples(const std::vector<float>& samples)
{
    std::string out;
    out.reserve(samples.size() * 5);
    char buf[16];
    for (size_t i = 0; i < samples.size(); ++i)
    {
        if (i) out.push_back(',');
        const int v = static_cast<int>(std::lround(
            std::clamp(static_cast<double>(samples[i]), -1.0, 1.0) * 10000.0));
        std::snprintf(buf, sizeof(buf), "%d", v);
        out += buf;
    }
    return out;
}

std::vector<float> parse_custom_samples(const std::string& raw)
{
    std::vector<float> out;
    // ★ 预留: 手绘曲线的规范长度就是 kCustomSamples, 但这里不强制 ——
    //   读到的点数由文件决定, aim_path.h 会按 size() 插值。
    out.reserve(4096);
    size_t i = 0;
    const size_t n = raw.size();
    while (i < n)
    {
        // 跳过空白与分隔符 (SimpleIni 的多行值里可能有换行/空格)
        while (i < n && (raw[i] == ',' || raw[i] == ' ' || raw[i] == '\t' ||
                         raw[i] == '\r' || raw[i] == '\n'))
            ++i;
        if (i >= n) break;
        bool neg = false;
        if (raw[i] == '-') { neg = true; ++i; }
        else if (raw[i] == '+') { ++i; }
        long v = 0;
        bool any = false;
        while (i < n && raw[i] >= '0' && raw[i] <= '9')
        {
            v = v * 10 + (raw[i] - '0');
            if (v > 1000000) v = 1000000;   // 防溢出: 超范围的值反正会被夹到 ±1
            ++i;
            any = true;
        }
        if (!any) { ++i; continue; }        // 非法字符, 跳过
        double d = static_cast<double>(v) / 10000.0;
        if (neg) d = -d;
        out.push_back(static_cast<float>(std::clamp(d, -1.0, 1.0)));
    }
    return out;
}

}

std::vector<std::string> Config::splitString(const std::string& str, char delimiter) const
{
    std::vector<std::string> tokens;
    std::stringstream ss(str);
    std::string item;
    while (std::getline(ss, item, delimiter))
    {
        while (!item.empty() && (item.front() == ' ' || item.front() == '\t'))
            item.erase(item.begin());
        while (!item.empty() && (item.back() == ' ' || item.back() == '\t'))
            item.pop_back();
        tokens.push_back(item);
    }
    return tokens;
}

std::string Config::joinStrings(const std::vector<std::string>& vec, const std::string& delimiter) const
{
    std::ostringstream oss;
    for (size_t i = 0; i < vec.size(); ++i)
    {
        if (i != 0) oss << delimiter;
        oss << vec[i];
    }
    return oss.str();
}

void Config::writeDefaultsInPlace()
{
    capture_device = "";
    capture_format = "";
    capture_width = 0;
    capture_height = 0;
    capture_fps = 0;
    capture_gpu_decode = true;
    detection_resolution = 320;
    circle_mask = true;

    backend = "TRT";
    ai_model = "sunxds_0.5.6.engine";
    confidence_threshold = 0.10f;
    nms_threshold = 0.50f;
    max_detections = kFixedMaxDetections;
    small_target_enabled = false;
    small_target_area_frac = 0.012f;
    small_target_confidence = 0.06f;
    fixed_input_size = false;
    engine_precision = "fp16";
    int8_calib_dir = "calib";
    int8_calib_images = 200;

    use_cuda_graph = true;
    // 自旋等待 GPU 事件会把 CPU 空转掉。实测 (spin_vs_block_bench, i5-4590 4 核,
    // 5ms 等待): 自旋 98% CPU/墙钟 -> 240fps 下约 1.2 个核; 阻塞 2% -> 0.03 个核。
    // 代价仅墙钟 +0.1ms/次。本机解码已占约 2 个核, 这 1.2 个核更值钱, 故默认关闭。
    use_spin_wait_sync = false;
    spin_wait_timeout_ms = 50;
    use_process_boost = true;
    use_mmcss = true;
    mmcss_task_name = "Games";
    gpuMemoryReserveMB = 2048;
    enableGpuExclusiveMode = true;

    cpuCoreReserveCount = 4;
    systemMemoryReserveMB = 2048;

    screenshot_button = splitString("None");
    screenshot_delay = 500;

    class_filters.clear();

    hotkeys.clear();
    HotkeyProfile hk;
    apply_default_hotkey(hk);
    hotkeys.push_back(std::move(hk));
    active_hotkey_group = u8"\xe9\xbb\x98\xe8\xae\xa4";

    macro_enabled = false;
    macro_script_path.clear();
    macro_primary_button_events = false;
}

bool Config::loadConfig(const std::string& filename)
{
    std::string target = filename.empty() ? "config.ini" : filename;
    std::error_code absEc;
    std::filesystem::path absPath = std::filesystem::absolute(std::filesystem::u8path(target), absEc);
    config_path = absEc ? target : absPath.u8string();

    if (!std::filesystem::exists(std::filesystem::u8path(target)))
    {
        std::cerr << "[Config] Config file does not exist, creating default config: " << target << std::endl;
        writeDefaultsInPlace();
        saveConfig(target);
        return true;
    }

    CSimpleIniA ini;
    SI_Error rc = ini.LoadFile(std::filesystem::u8path(target).wstring().c_str());
    if (rc < 0)
    {
        std::cerr << "[Config] Error parsing INI file: " << target << std::endl;
        return false;
    }

    auto get_string = [&](const char* section, const char* key, const std::string& defval) {
        const char* val = ini.GetValue(section, key, defval.c_str());
        return val ? std::string(val) : defval;
    };
    auto get_bool = [&](const char* section, const char* key, bool defval) {
        return ini.GetBoolValue(section, key, defval);
    };
    auto get_long = [&](const char* section, const char* key, long defval) {
        return static_cast<int>(ini.GetLongValue(section, key, defval));
    };
    auto get_double = [&](const char* section, const char* key, double defval) {
        return ini.GetDoubleValue(section, key, defval);
    };

    capture_device = get_string("", "capture_device", "");
    capture_format = get_string("", "capture_format", "");
    capture_width  = static_cast<int>(get_long("", "capture_width", 0));
    capture_height = static_cast<int>(get_long("", "capture_height", 0));
    capture_fps    = static_cast<int>(get_long("", "capture_fps", 0));
    if (capture_width  < 0) capture_width  = 0;
    if (capture_height < 0) capture_height = 0;
    if (capture_fps    < 0) capture_fps    = 0;
    capture_gpu_decode = get_bool("", "capture_gpu_decode", true);
    detection_resolution = std::clamp(static_cast<int>(get_long("", "detection_resolution", 320)), 32, 2048);

    circle_mask = true;

    input_method = get_string("", "input_method", "MAKCU");
    if (input_method != "MAKCU" && input_method != "MAKCUNEW" && input_method != "KMBOXNET")
        input_method = "MAKCU";
    const auto finiteSetting = [&](const char* key, double fallback, double low, double high) {
        const double value = get_double("", key, fallback);
        return std::isfinite(value) ? std::clamp(value, low, high) : fallback;
    };
    makcu_baudrate = get_long("", "makcu_baudrate", 115200);
    makcu_port = get_string("", "makcu_port", "COM0");
    makcu_new_baudrate = std::clamp(
        static_cast<int>(get_long("", "makcu_new_baudrate", 6000000)),
        1200, 6000000);
    makcu_new_port = get_string("", "makcu_new_port", "COM0");
    // 第二台(键盘): 默认空 = 未配置, 键盘动作回落到第一台。
    makcu_new_baudrate_kbd = std::clamp(
        static_cast<int>(get_long("", "makcu_new_baudrate_kbd", 6000000)),
        1200, 6000000);
    makcu_new_port_kbd = get_string("", "makcu_new_port_kbd", "");
    kmbox_net_ip = get_string("", "kmbox_net_ip", "192.168.2.88");
    kmbox_net_port = get_string("", "kmbox_net_port", "6234");
    kmbox_net_uuid = get_string("", "kmbox_net_uuid", "12345");
    backend = "TRT";
    ai_model = get_string("", "ai_model", "sunxds_0.5.6.engine");
    confidence_threshold = static_cast<float>(get_double("", "confidence_threshold", 0.15));
    nms_threshold = static_cast<float>(get_double("", "nms_threshold", 0.50));
    max_detections = kFixedMaxDetections;
    small_target_enabled = get_bool("", "small_target_enabled", false);
    small_target_area_frac = static_cast<float>(get_double("", "small_target_area_frac", 0.012));
    small_target_confidence = static_cast<float>(get_double("", "small_target_confidence", 0.06));
    fixed_input_size = get_bool("", "fixed_input_size", false);
    engine_precision = get_string("", "engine_precision", "fp16");
    // 非法值不静默"替换成别的模式": 直接回落到 fp16, 并让上层能看出这是回落。
    if (engine_precision != "fp16" && engine_precision != "int8")
        engine_precision = "fp16";
    int8_calib_dir = get_string("", "int8_calib_dir", "calib");
    int8_calib_images = static_cast<int>(
        std::clamp<long>(get_long("", "int8_calib_images", 200), 1L, 2000L));

    use_cuda_graph = get_bool("", "use_cuda_graph", true);
    use_spin_wait_sync = get_bool("", "use_spin_wait_sync", false);
    spin_wait_timeout_ms = static_cast<int>(std::clamp<long>(get_long("", "spin_wait_timeout_ms", 50), 1L, 1000L));
    use_process_boost = get_bool("", "use_process_boost", true);
    use_mmcss = get_bool("", "use_mmcss", true);
    mmcss_task_name = get_string("", "mmcss_task_name", "Games");
    gpuMemoryReserveMB = get_long("", "gpuMemoryReserveMB", 2048);
    enableGpuExclusiveMode = get_bool("", "enableGpuExclusiveMode", true);

    cpuCoreReserveCount = get_long("", "cpuCoreReserveCount", 4);
    systemMemoryReserveMB = get_long("", "systemMemoryReserveMB", 2048);

    show_window = get_bool("", "show_window", true);
    show_fps = get_bool("", "show_fps", false);
    screenshot_button = splitString(get_string("", "screenshot_button", "None"));
    screenshot_delay = get_long("", "screenshot_delay", 500);
    verbose = get_bool("", "verbose", false);

    replay_record_enabled  = get_bool("", "replay_record_enabled", false);
    replay_seconds         = std::clamp(get_long("", "replay_seconds", 10), 1, 60);
    replay_playback_speed  = std::clamp(
        static_cast<float>(get_double("", "replay_playback_speed", 0.25)),
        0.05f, 2.0f);

    auto_capture_enabled    = get_bool("",   "auto_capture_enabled",    false);
    auto_capture_use_high   = get_bool("",   "auto_capture_use_high",   true);
    auto_capture_high_conf  = std::clamp(
        static_cast<float>(get_double("", "auto_capture_high_conf", 0.85)), 0.0f, 1.0f);
    auto_capture_use_low    = get_bool("",   "auto_capture_use_low",    false);
    auto_capture_low_conf   = std::clamp(
        static_cast<float>(get_double("", "auto_capture_low_conf", 0.30)), 0.0f, 1.0f);
    auto_capture_any_detection   = get_bool("", "auto_capture_any_detection",   false);
    auto_capture_cooldown_ms = std::max(0,
        static_cast<int>(get_long("", "auto_capture_cooldown_ms", 200)));
    auto_capture_force_keys = splitString(
        get_string("", "auto_capture_force_keys", "X2MouseButton"));
    auto_capture_output_dir = get_string("", "auto_capture_output_dir",
                                         "screenshots/auto");
    auto_capture_save_label = get_bool("",   "auto_capture_save_label", true);

    crosshair_rect_w           = std::clamp(get_long("", "crosshair_rect_w",  40), 4, 512);
    crosshair_rect_h           = std::clamp(get_long("", "crosshair_rect_h",  40), 4, 512);
    crosshair_min_pixel_count  = std::clamp(get_long("", "crosshair_min_pixel_count", 4), 1, 10000);
    crosshair_close_radius     = std::clamp(get_long("", "crosshair_close_radius",    1), 0, 7);

    crosshair_colors.clear();
    {
        CSimpleIniA::TNamesDepend sections;
        ini.GetAllSections(sections);
        std::vector<std::pair<int, std::string>> cc_sections;
        const std::string prefix = "crosshair_color.";
        for (const auto& s : sections)
        {
            std::string sname = s.pItem;
            if (sname.rfind(prefix, 0) != 0) continue;
            int idx = 0;
            try { idx = std::stoi(sname.substr(prefix.size())); }
            catch (...) { continue; }
            cc_sections.emplace_back(idx, std::move(sname));
        }
        std::sort(cc_sections.begin(), cc_sections.end(),
            [](const auto& a, const auto& b) { return a.first < b.first; });
        for (const auto& entry : cc_sections)
        {
            const char* sec = entry.second.c_str();
            CrosshairColorProfileConfig c;
            c.name    = get_string(sec, "name", "Color");
            c.enabled = get_bool(sec, "enabled", true);
            c.h_low   = std::clamp(get_long(sec, "h_low",   0),   0, 179);
            c.h_high  = std::clamp(get_long(sec, "h_high",  10),  0, 179);
            c.s_min   = std::clamp(get_long(sec, "s_min",   120), 0, 255);
            c.s_max   = std::clamp(get_long(sec, "s_max",   255), 0, 255);
            c.v_min   = std::clamp(get_long(sec, "v_min",   120), 0, 255);
            c.v_max   = std::clamp(get_long(sec, "v_max",   255), 0, 255);
            crosshair_colors.push_back(std::move(c));
        }
        if (crosshair_colors.empty())
        {
            CrosshairColorProfileConfig low;
            low.name = "Red-Low";  low.h_low = 0;   low.h_high = 10;
            CrosshairColorProfileConfig hi;
            hi.name  = "Red-High"; hi.h_low  = 160; hi.h_high  = 179;
            crosshair_colors.push_back(std::move(low));
            crosshair_colors.push_back(std::move(hi));
        }
    }

    macro_enabled = get_bool("", "macro_enabled", false);
    macro_script_path = get_string("", "macro_script_path", "");
    macro_primary_button_events = get_bool("", "macro_primary_button_events", false);

    class_filters.clear();
    {
        CSimpleIniA::TNamesDepend keys;
        ini.GetAllKeys("classes", keys);
        for (const auto& k : keys)
        {
            int class_id = 0;
            try { class_id = std::stoi(k.pItem); }
            catch (...) { continue; }

            std::string val = ini.GetValue("classes", k.pItem, "");
            auto parts = splitString(val, ',');
            ClassFilterState st;
            st.class_id = class_id;
            if (!parts.empty())
                st.bucket = bucket_from_str(parts[0]);
            if (parts.size() >= 2)
                st.class_name = parts[1];
            class_filters.push_back(st);
        }
        std::sort(class_filters.begin(), class_filters.end(),
            [](const ClassFilterState& a, const ClassFilterState& b) {
                return a.class_id < b.class_id;
            });
    }

    // 全局选靶与稳定器
    target_hysteresis_ratio   = get_double("target_stabilizer", "target_hysteresis_ratio",   target_hysteresis_ratio);
    target_max_distance_px    = get_double("target_stabilizer", "target_max_distance_px",    target_max_distance_px);
    target_match_center_ratio = get_double("target_stabilizer", "target_match_center_ratio", target_match_center_ratio);
    target_area_ratio_tol     = get_double("target_stabilizer", "target_area_ratio_tol",     target_area_ratio_tol);
    target_k_snap_mult        = get_double("target_stabilizer", "target_k_snap_mult",        target_k_snap_mult);
    target_min_aspect         = get_double("target_stabilizer", "target_min_aspect",         target_min_aspect);
    target_max_aspect         = get_double("target_stabilizer", "target_max_aspect",         target_max_aspect);
    target_hysteresis_ratio   = std::clamp(target_hysteresis_ratio, 1.0, 10.0);
    target_max_distance_px    = std::max(0.0, target_max_distance_px);
    target_match_center_ratio = std::clamp(target_match_center_ratio, 1e-3, 10.0);
    target_area_ratio_tol     = std::clamp(target_area_ratio_tol, 1.0, 100.0);
    target_k_snap_mult        = std::clamp(target_k_snap_mult, 1e-3, 100.0);
    target_min_aspect         = std::clamp(target_min_aspect, 1e-3, 100.0);
    target_max_aspect         = std::clamp(target_max_aspect, 1e-3, 100.0);

    hotkeys.clear();
    {
        CSimpleIniA::TNamesDepend sections;
        ini.GetAllSections(sections);
        std::vector<std::pair<int, std::string>> hk_sections;
        for (const auto& s : sections)
        {
            std::string name = s.pItem;
            const std::string prefix = "hotkey.";
            if (name.rfind(prefix, 0) != 0)
                continue;
            int idx = 0;
            try { idx = std::stoi(name.substr(prefix.size())); }
            catch (...) { continue; }
            hk_sections.emplace_back(idx, std::move(name));
        }
        std::sort(hk_sections.begin(), hk_sections.end(),
            [](const auto& a, const auto& b) { return a.first < b.first; });

        for (const auto& entry : hk_sections)
        {
            const char* sec = entry.second.c_str();
            HotkeyProfile hk;
            hk.name = get_string(sec, "name", hk.name);
            hk.group = get_string(sec, "group", hk.group);
            if (hk.group.empty())
                hk.group = u8"默认";
            hk.keys = splitString(get_string(sec, "keys", "RightMouseButton"));

            hk.fovX = get_long(sec, "fovX", hk.fovX);
            hk.fovY = get_long(sec, "fovY", hk.fovY);

            {
                const std::string aim_raw = get_string(sec, "aim_classes", "");
                hk.aim_classes = parse_aim_classes(aim_raw);
                if (aim_raw.empty())
                {
                    for (int slot = 1; slot <= 3; ++slot)
                    {
                        const std::string suffix = std::to_string(slot);
                        const int class_id = static_cast<int>(get_long(
                            sec, ("target_class_" + suffix).c_str(), -1));
                        if (class_id < 0) continue;

                        const float y_top = static_cast<float>(get_double(
                            sec, ("target_y_top_" + suffix).c_str(), 0.0));
                        const float y_bot = static_cast<float>(get_double(
                            sec, ("target_y_bot_" + suffix).c_str(), 1.0));
                        HotkeyAimClass migrated;
                        migrated.class_id = class_id;
                        migrated.y_offset = std::clamp(1.0f - y_bot, 0.0f, 1.0f);
                        migrated.y_offset_max = std::clamp(1.0f - y_top, 0.0f, 1.0f);
                        if (migrated.y_offset > migrated.y_offset_max)
                            std::swap(migrated.y_offset, migrated.y_offset_max);
                        migrated.min_conf = static_cast<float>(get_double(
                            sec, ("target_min_conf_" + suffix).c_str(), 0.0));
                        hk.aim_classes.push_back(migrated);
                    }
                }
            }

            hk.crosshair_detect_enabled  = get_bool(sec, "crosshair_detect_enabled", false);

            {
                hk.dynamic_fov_enabled = get_bool(sec, "dynamic_fov_enabled", hk.dynamic_fov_enabled);
                const float legacy_margin = static_cast<float>(
                    get_double(sec, "dynamic_fov_margin_frac", 2.0 - hk.dynamic_fov_strength));
                const float legacy_strength = std::clamp(2.0f - legacy_margin, 0.0f, 1.0f);
                hk.dynamic_fov_strength = static_cast<float>(
                    get_double(sec, "dynamic_fov_strength", legacy_strength));
            }

            hk.ctl_kp_x = get_double(sec, "ctl_kp_x", hk.ctl_kp_x);
            hk.ctl_kp_y = get_double(sec, "ctl_kp_y", hk.ctl_kp_y);
            hk.ctl_ki_x = get_double(sec, "ctl_ki_x", hk.ctl_ki_x);
            hk.ctl_ki_y = get_double(sec, "ctl_ki_y", hk.ctl_ki_y);
            hk.ctl_kd_x = get_double(sec, "ctl_kd_x", hk.ctl_kd_x);
            hk.ctl_kd_y = get_double(sec, "ctl_kd_y", hk.ctl_kd_y);
            hk.ctl_tau_unwind_sec = get_double(sec, "ctl_tau_unwind_sec", hk.ctl_tau_unwind_sec);
            hk.ctl_tau_deriv_sec  = get_double(sec, "ctl_tau_deriv_sec",  hk.ctl_tau_deriv_sec);
            hk.ctl_i_max          = get_double(sec, "ctl_i_max",          hk.ctl_i_max);
            hk.ctl_max_output_counts =
                static_cast<int>(get_double(sec, "ctl_max_output_counts", hk.ctl_max_output_counts));
            hk.ctl_p_full_scale_px = get_double(sec, "ctl_p_full_scale_px", hk.ctl_p_full_scale_px);
            hk.ctl_predict_lead_ms =
                get_double(sec, "ctl_predict_lead_ms", hk.ctl_predict_lead_ms);
            hk.ctl_predict_max_velocity_px_s =
                get_double(sec, "ctl_predict_max_velocity_px_s", hk.ctl_predict_max_velocity_px_s);
            hk.ctl_predict_max_lead_ratio =
                get_double(sec, "ctl_predict_max_lead_ratio", hk.ctl_predict_max_lead_ratio);
            hk.ctl_k_px_per_count =
                get_double(sec, "ctl_k_px_per_count", hk.ctl_k_px_per_count);
            hk.ctl_inflight_beta =
                get_double(sec, "ctl_inflight_beta", hk.ctl_inflight_beta);
            hk.ctl_inflight_dead_time_ms =
                get_double(sec, "ctl_inflight_dead_time_ms", hk.ctl_inflight_dead_time_ms);
            hk.ctl_y_offset     = get_double(sec, "ctl_y_offset",     hk.ctl_y_offset);
            hk.ctl_y_offset_max = get_double(sec, "ctl_y_offset_max", hk.ctl_y_offset_max);
            hk.ctl_hysteresis_ratio = get_double(sec, "ctl_hysteresis_ratio", hk.ctl_hysteresis_ratio);
            hk.ctl_enabled = get_bool(sec, "ctl_enabled", false);
            hk.ctl_max_distance_px = get_double(sec, "ctl_max_distance_px", hk.ctl_max_distance_px);
            hk.ctl_match_center_ratio = get_double(sec, "ctl_match_center_ratio", hk.ctl_match_center_ratio);
            hk.ctl_area_ratio_tol = get_double(sec, "ctl_area_ratio_tol", hk.ctl_area_ratio_tol);
            hk.ctl_k_snap_mult = get_double(sec, "ctl_k_snap_mult", hk.ctl_k_snap_mult);
            hk.ctl_min_aspect = get_double(sec, "ctl_min_aspect", hk.ctl_min_aspect);
            hk.ctl_max_aspect = get_double(sec, "ctl_max_aspect", hk.ctl_max_aspect);
            hk.ctl_random_seed =
                static_cast<int>(get_double(sec, "ctl_random_seed", hk.ctl_random_seed));

            // ── 开镜档 (自动开镜生效期间取代上面的默认档) ──────────────────
            // ★ 缺键一律取结构体默认值(= 默认档的默认值), 所以老配置读进来
            //   即使把开关打开, 也不会突然变成另一套参数。
            hk.scope_ctl_enabled = static_cast<int>(
                get_double(sec, "ctl_scope_enabled", hk.scope_ctl_enabled));
            hk.ctl_scope.kp_x = get_double(sec, "ctl_scope_kp_x", hk.ctl_scope.kp_x);
            hk.ctl_scope.kp_y = get_double(sec, "ctl_scope_kp_y", hk.ctl_scope.kp_y);
            hk.ctl_scope.ki_x = get_double(sec, "ctl_scope_ki_x", hk.ctl_scope.ki_x);
            hk.ctl_scope.ki_y = get_double(sec, "ctl_scope_ki_y", hk.ctl_scope.ki_y);
            hk.ctl_scope.kd_x = get_double(sec, "ctl_scope_kd_x", hk.ctl_scope.kd_x);
            hk.ctl_scope.kd_y = get_double(sec, "ctl_scope_kd_y", hk.ctl_scope.kd_y);
            hk.ctl_scope.tau_unwind_sec =
                get_double(sec, "ctl_scope_tau_unwind_sec", hk.ctl_scope.tau_unwind_sec);
            hk.ctl_scope.tau_deriv_sec =
                get_double(sec, "ctl_scope_tau_deriv_sec", hk.ctl_scope.tau_deriv_sec);
            hk.ctl_scope.i_max = get_double(sec, "ctl_scope_i_max", hk.ctl_scope.i_max);
            hk.ctl_scope.max_output_counts = static_cast<int>(get_double(
                sec, "ctl_scope_max_output_counts", hk.ctl_scope.max_output_counts));
            hk.ctl_scope.p_full_scale_px =
                get_double(sec, "ctl_scope_p_full_scale_px", hk.ctl_scope.p_full_scale_px);
            hk.ctl_scope.predict_lead_ms =
                get_double(sec, "ctl_scope_predict_lead_ms", hk.ctl_scope.predict_lead_ms);
            hk.ctl_scope.predict_max_velocity_px_s = get_double(
                sec, "ctl_scope_predict_max_velocity_px_s",
                hk.ctl_scope.predict_max_velocity_px_s);
            hk.ctl_scope.predict_max_lead_ratio = get_double(
                sec, "ctl_scope_predict_max_lead_ratio", hk.ctl_scope.predict_max_lead_ratio);
            hk.ctl_scope.k_px_per_count = get_double(
                sec, "ctl_scope_k_px_per_count", hk.ctl_scope.k_px_per_count);
            hk.ctl_scope.inflight_beta = get_double(
                sec, "ctl_scope_inflight_beta", hk.ctl_scope.inflight_beta);
            hk.ctl_scope.inflight_dead_time_ms = get_double(
                sec, "ctl_scope_inflight_dead_time_ms", hk.ctl_scope.inflight_dead_time_ms);
            hk.ctl_scope.random_seed = static_cast<int>(
                get_double(sec, "ctl_scope_random_seed", hk.ctl_scope.random_seed));

            hk.trigger_enabled = get_bool(sec, "trigger_enabled", false);
            hk.trigger_fire_delay = static_cast<int>(get_double(sec, "trigger_fire_delay", hk.trigger_fire_delay));
            hk.trigger_fire_duration = static_cast<int>(get_double(sec, "trigger_fire_duration", hk.trigger_fire_duration));
            hk.trigger_fire_interval = static_cast<int>(get_double(sec, "trigger_fire_interval", hk.trigger_fire_interval));
            hk.trigger_y_percent = static_cast<int>(get_double(sec, "trigger_y_percent", hk.trigger_y_percent));
            hk.trigger_delay_jitter_ms = static_cast<int>(get_double(sec, "trigger_delay_jitter_ms", hk.trigger_delay_jitter_ms));
            hk.trigger_duration_jitter_ms = static_cast<int>(get_double(sec, "trigger_duration_jitter_ms", hk.trigger_duration_jitter_ms));
            hk.trigger_interval_jitter_ms = static_cast<int>(get_double(sec, "trigger_interval_jitter_ms", hk.trigger_interval_jitter_ms));
            hk.trigger_switch_cooldown_ms = static_cast<int>(get_double(sec, "trigger_switch_cooldown_ms", hk.trigger_switch_cooldown_ms));
            hk.trigger_auto_scope = static_cast<int>(get_double(sec, "trigger_auto_scope", hk.trigger_auto_scope));
            hk.trigger_scope_delay_ms = static_cast<int>(get_double(sec, "trigger_scope_delay_ms", hk.trigger_scope_delay_ms));
            hk.trigger_auto_stop = static_cast<int>(get_double(sec, "trigger_auto_stop", hk.trigger_auto_stop));
            hk.trigger_stop_ms = static_cast<int>(get_double(sec, "trigger_stop_ms", hk.trigger_stop_ms));

            hk.aim_path_mode = static_cast<int>(get_double(sec, "aim_path_mode", hk.aim_path_mode));
            hk.aim_path_influence = static_cast<int>(get_double(sec, "aim_path_influence", hk.aim_path_influence));
            hk.aim_path_bezier_cx1 = static_cast<float>(get_double(sec, "aim_path_bezier_cx1", hk.aim_path_bezier_cx1));
            hk.aim_path_bezier_cy1 = static_cast<float>(get_double(sec, "aim_path_bezier_cy1", hk.aim_path_bezier_cy1));
            hk.aim_path_bezier_cx2 = static_cast<float>(get_double(sec, "aim_path_bezier_cx2", hk.aim_path_bezier_cx2));
            hk.aim_path_bezier_cy2 = static_cast<float>(get_double(sec, "aim_path_bezier_cy2", hk.aim_path_bezier_cy2));
            hk.aim_path_wind_gravity = static_cast<float>(get_double(sec, "aim_path_wind_gravity", hk.aim_path_wind_gravity));
            hk.aim_path_wind_wind = static_cast<float>(get_double(sec, "aim_path_wind_wind", hk.aim_path_wind_wind));
            hk.aim_path_wind_step = static_cast<float>(get_double(sec, "aim_path_wind_step", hk.aim_path_wind_step));
            hk.aim_path_wind_distance = static_cast<float>(get_double(sec, "aim_path_wind_distance", hk.aim_path_wind_distance));
            hk.aim_path_wind_threshold = static_cast<int>(get_double(sec, "aim_path_wind_threshold", hk.aim_path_wind_threshold));
            {
                // 手绘曲线采样点。★ 空值/缺失 ⇒ 保持 nullptr (模式 2 会退化成直线)。
                const std::string cs = get_string(sec, "aim_path_custom_samples", "");
                if (!cs.empty())
                {
                    auto samples = parse_custom_samples(cs);
                    if (!samples.empty())
                        hk.aim_path_custom_samples =
                            std::make_shared<const std::vector<float>>(std::move(samples));
                }
            }

            hotkeys.push_back(std::move(hk));
        }

        if (hotkeys.empty())
        {
            HotkeyProfile hk;
            apply_default_hotkey(hk);
            hotkeys.push_back(std::move(hk));
        }
    }

    active_hotkey_group = get_string("", "active_hotkey_group", u8"\xe9\xbb\x98\xe8\xae\xa4");

    {
        bool matched = false;
        for (const auto& hk : hotkeys)
            if (hk.group == active_hotkey_group) { matched = true; break; }
        if (!matched && !hotkeys.empty())
            active_hotkey_group = hotkeys[0].group;
    }

    auto clamp_target_fields = [](HotkeyProfile& hk) {

        for (auto& ac : hk.aim_classes)
        {
            ac.y_offset = std::clamp(ac.y_offset, 0.0f, 1.0f);
            ac.y_offset_max = std::clamp(ac.y_offset_max, 0.0f, 1.0f);
            if (ac.y_offset > ac.y_offset_max)
                std::swap(ac.y_offset, ac.y_offset_max);
            ac.min_conf = std::clamp(ac.min_conf, 0.0f, 1.0f);
        }

        hk.dynamic_fov_strength = std::clamp(hk.dynamic_fov_strength, 0.0f, 1.0f);

        // ── 瞄准控制器参数组 (默认档) ─────────────────────────────────────
        // ★ 借道 AimCtlParams::clamp(): 默认档与开镜档共用【同一份】夹取规则,
        //   不给同一组参数留两处会漂移的规则。
        {
            AimCtlParams p = ctlParamsOf(hk);
            p.clamp();
            applyCtlParams(hk, p);
        }
        // ── 瞄准控制器参数组 (开镜档) ─────────────────────────────────────
        hk.ctl_scope.clamp();
        hk.scope_ctl_enabled = std::clamp(hk.scope_ctl_enabled, 0, 1);

        hk.ctl_y_offset = std::clamp(hk.ctl_y_offset, 0.0, 1.0);
        hk.ctl_y_offset_max = std::clamp(hk.ctl_y_offset_max, 0.0, 1.0);
        if (hk.ctl_y_offset > hk.ctl_y_offset_max)
            std::swap(hk.ctl_y_offset, hk.ctl_y_offset_max);
        hk.ctl_hysteresis_ratio = std::clamp(hk.ctl_hysteresis_ratio, 1.0, 10.0);

        hk.ctl_max_distance_px = std::max(0.0, hk.ctl_max_distance_px);
        hk.ctl_match_center_ratio = std::clamp(hk.ctl_match_center_ratio, 1e-3, 10.0);
        hk.ctl_area_ratio_tol = std::clamp(hk.ctl_area_ratio_tol, 1.0, 100.0);
        hk.ctl_k_snap_mult = std::clamp(hk.ctl_k_snap_mult, 1e-3, 100.0);
        hk.ctl_min_aspect = std::clamp(hk.ctl_min_aspect, 1e-3, 100.0);
        hk.ctl_max_aspect = std::clamp(hk.ctl_max_aspect, 1e-3, 100.0);
        if (hk.ctl_min_aspect > hk.ctl_max_aspect)
            std::swap(hk.ctl_min_aspect, hk.ctl_max_aspect);

        hk.trigger_fire_delay    = std::max(0, hk.trigger_fire_delay);
        hk.trigger_fire_duration = std::max(0, hk.trigger_fire_duration);
        hk.trigger_fire_interval = std::max(1, hk.trigger_fire_interval);
        hk.trigger_delay_jitter_ms    = std::max(0, hk.trigger_delay_jitter_ms);
        hk.trigger_duration_jitter_ms = std::max(0, hk.trigger_duration_jitter_ms);
        hk.trigger_interval_jitter_ms = std::max(0, hk.trigger_interval_jitter_ms);
        hk.trigger_switch_cooldown_ms = std::max(0, hk.trigger_switch_cooldown_ms);
        hk.trigger_scope_delay_ms = std::max(0, hk.trigger_scope_delay_ms);
        hk.trigger_y_percent = std::clamp(hk.trigger_y_percent, 10, 300);
        hk.trigger_auto_scope = std::clamp(hk.trigger_auto_scope, 0, 2);
        hk.trigger_auto_stop = hk.trigger_auto_stop > 0 ? 1 : 0;
        hk.trigger_stop_ms = std::clamp(hk.trigger_stop_ms, 20, 300);

        hk.aim_path_mode = std::clamp(hk.aim_path_mode, 0, 3);
        hk.aim_path_influence = std::clamp(hk.aim_path_influence, 0, 100);
        hk.aim_path_bezier_cx1 = std::clamp(hk.aim_path_bezier_cx1, 0.0f, 1.0f);
        hk.aim_path_bezier_cx2 = std::clamp(hk.aim_path_bezier_cx2, 0.0f, 1.0f);
        hk.aim_path_bezier_cy1 = std::clamp(hk.aim_path_bezier_cy1, -1.0f, 1.0f);
        hk.aim_path_bezier_cy2 = std::clamp(hk.aim_path_bezier_cy2, -1.0f, 1.0f);
        hk.aim_path_wind_gravity = std::clamp(hk.aim_path_wind_gravity, 0.1f, 100.0f);
        hk.aim_path_wind_wind    = std::clamp(hk.aim_path_wind_wind, 0.0f, 100.0f);
        hk.aim_path_wind_step    = std::clamp(hk.aim_path_wind_step, 1.0f, 200.0f);
        hk.aim_path_wind_distance = std::clamp(hk.aim_path_wind_distance, 1.0f, 200.0f);
        hk.aim_path_wind_threshold = std::max(0, hk.aim_path_wind_threshold);
    };
    for (auto& hk : hotkeys)
        clamp_target_fields(hk);

    static const std::unordered_set<std::string> kAllowedAimKeys = {
        "None", "LeftMouseButton", "RightMouseButton",
        "X1MouseButton", "X2MouseButton",
    };
    for (auto& hk : hotkeys)
    {
        for (auto& k : hk.keys)
        {
            if (kAllowedAimKeys.find(k) == kAllowedAimKeys.end())
                k = "None";
        }
        if (hk.keys.empty())
            hk.keys.push_back("None");
    }

    return true;
}

void Config::retargetConfigPath(const std::string& filename)
{
    const std::string target = filename.empty() ? "config.ini" : filename;
    std::error_code absEc;
    const std::filesystem::path absPath =
        std::filesystem::absolute(std::filesystem::u8path(target), absEc);
    config_path = absEc ? target : absPath.u8string();
}

bool Config::saveConfig(const std::string& filename)
{
    std::string target = filename.empty() ? "config.ini" : filename;
    if (target == "config.ini" && !config_path.empty())
        target = config_path;

    std::filesystem::path targetPath = std::filesystem::u8path(target);
    std::error_code mkEc;
    if (targetPath.has_parent_path())
        std::filesystem::create_directories(targetPath.parent_path(), mkEc);

    std::ofstream file(targetPath.wstring().c_str(), std::ios::out | std::ios::trunc);
    if (!file.is_open())
    {
        DWORD winErr = ::GetLastError();
        std::cerr << "[Config] Error opening config for writing: " << target
                  << " (errno=" << errno << ", GetLastError=" << winErr << ")" << std::endl;
        return false;
    }

    file << "# Apotheosis configuration.\n";
    file << "# Generated automatically; hand-edit with care.\n\n";

    file << u8"# Capture  (只有「采集卡」一种方式; 参数必须来自设备真实能力探测,\n"
            u8"# 组合对不上会直接报错, 不做任何替换)\n"
        << "capture_device = " << capture_device << "\n"
        << "capture_format = " << capture_format << "\n"
        << "capture_width = " << capture_width << "\n"
        << "capture_height = " << capture_height << "\n"
        << "capture_fps = " << capture_fps << "\n"
        << "capture_gpu_decode = " << to_bool_str(capture_gpu_decode) << "\n"
        << "detection_resolution = " << detection_resolution << "\n"
        << "circle_mask = " << to_bool_str(circle_mask) << "\n\n";

    file << "# Hardware / input device\n"
        << "# MAKCU | MAKCUNEW | KMBOXNET  (三档共用 mouse/mouse_driver.h 的驱动抽象)\n"
        << "input_method = " << input_method << "\n"
        << "makcu_baudrate = " << makcu_baudrate << "\n"
        << "makcu_port = " << makcu_port << "\n"
        << "makcu_new_baudrate = " << makcu_new_baudrate << "\n"
        << "makcu_new_port = " << makcu_new_port << "\n"
        << "makcu_new_baudrate_kbd = " << makcu_new_baudrate_kbd << "\n"
        << "makcu_new_port_kbd = " << makcu_new_port_kbd << "\n"
        << "# KMBox Net: 三个值照抄盒子屏幕上显示的 ip / port / uuid\n"
        << "kmbox_net_ip = " << kmbox_net_ip << "\n"
        << "kmbox_net_port = " << kmbox_net_port << "\n"
        << "kmbox_net_uuid = " << kmbox_net_uuid << "\n\n";

    file << "# AI\n"
        << "ai_model = " << ai_model << "\n"
        << std::fixed << std::setprecision(2)
        << "confidence_threshold = " << confidence_threshold << "\n"
        << "nms_threshold = " << nms_threshold << "\n"
        << "small_target_enabled = " << to_bool_str(small_target_enabled) << "\n"
        << std::setprecision(3)
        << "small_target_area_frac = " << small_target_area_frac << "\n"
        << std::setprecision(2)
        << "small_target_confidence = " << small_target_confidence << "\n"
        << std::setprecision(0)
        << "fixed_input_size = " << to_bool_str(fixed_input_size) << "\n"
        << "# 引擎精度: fp16 | int8  (int8 需要校准图集; 改完必须删旧 .engine)\n"
        << "engine_precision = " << engine_precision << "\n"
        << "int8_calib_dir = " << int8_calib_dir << "\n"
        << "int8_calib_images = " << int8_calib_images << "\n\n";

    file << "# CUDA / system\n"
        << "use_cuda_graph = " << to_bool_str(use_cuda_graph) << "\n"
        << "use_spin_wait_sync = " << to_bool_str(use_spin_wait_sync) << "\n"
        << "spin_wait_timeout_ms = " << spin_wait_timeout_ms << "\n"
        << "use_process_boost = " << to_bool_str(use_process_boost) << "\n"
        << "use_mmcss = " << to_bool_str(use_mmcss) << "\n"
        << "mmcss_task_name = " << mmcss_task_name << "\n"
        << "gpuMemoryReserveMB = " << gpuMemoryReserveMB << "\n"
        << "enableGpuExclusiveMode = " << to_bool_str(enableGpuExclusiveMode) << "\n"
        << "cpuCoreReserveCount = " << cpuCoreReserveCount << "\n"
        << "systemMemoryReserveMB = " << systemMemoryReserveMB << "\n\n";

    file << "# Replay\n"
        << "replay_record_enabled = " << to_bool_str(replay_record_enabled) << "\n"
        << "replay_seconds = " << replay_seconds << "\n"
        << "replay_playback_speed = " << replay_playback_speed << "\n\n";

    file << "# Crosshair color detector (palette + ROI; per-hotkey toggle lives on each [hotkey.N])\n"
        << "crosshair_rect_w = "          << crosshair_rect_w          << "\n"
        << "crosshair_rect_h = "          << crosshair_rect_h          << "\n"
        << "crosshair_min_pixel_count = " << crosshair_min_pixel_count << "\n"
        << "crosshair_close_radius = "    << crosshair_close_radius    << "\n\n";

    file << "# Debug\n"
        << "show_window = " << to_bool_str(show_window) << "\n"
        << "show_fps = " << to_bool_str(show_fps) << "\n"
        << "screenshot_button = " << joinStrings(screenshot_button) << "\n"
        << "screenshot_delay = " << screenshot_delay << "\n"
        << "verbose = " << to_bool_str(verbose) << "\n\n";

    file << "# Auto capture (data collection harness)\n"
        << "auto_capture_enabled = "    << to_bool_str(auto_capture_enabled) << "\n"
        << "auto_capture_use_high = "   << to_bool_str(auto_capture_use_high) << "\n"
        << "auto_capture_high_conf = "  << auto_capture_high_conf << "\n"
        << "auto_capture_use_low = "    << to_bool_str(auto_capture_use_low) << "\n"
        << "auto_capture_low_conf = "   << auto_capture_low_conf << "\n"
        << "auto_capture_any_detection = "  << to_bool_str(auto_capture_any_detection)  << "\n"
        << "auto_capture_cooldown_ms = " << auto_capture_cooldown_ms << "\n"
        << "auto_capture_force_keys = " << joinStrings(auto_capture_force_keys) << "\n"
        << "auto_capture_output_dir = " << auto_capture_output_dir << "\n"
        << "auto_capture_save_label = " << to_bool_str(auto_capture_save_label) << "\n\n";

    file << "# Macro (G HUB-compatible Lua). Drop a .lua script path into\n"
            "# macro_script_path; runtime loads it on startup when macro_enabled\n"
            "# is true. macro_primary_button_events mirrors the script-side\n"
            "# EnablePrimaryMouseButtonEvents default.\n"
        << "macro_enabled = " << to_bool_str(macro_enabled) << "\n"
        << "macro_script_path = " << macro_script_path << "\n"
        << "macro_primary_button_events = " << to_bool_str(macro_primary_button_events) << "\n\n";

    file << "[classes]\n";
    file << "# Format: <class_id> = <bucket>,<display_name>\n";
    file << "# bucket in { delete, filter, aim }\n";
    for (const auto& cf : class_filters)
    {
        file << cf.class_id << " = " << bucket_to_str(cf.bucket);
        if (!cf.class_name.empty())
            file << "," << cf.class_name;
        file << "\n";
    }
    file << "\n";

    file << "active_hotkey_group = " << active_hotkey_group << "\n\n";

    for (size_t i = 0; i < hotkeys.size(); ++i)
    {
        const auto& hk = hotkeys[i];
        file << "[hotkey." << i << "]\n";
        file << "name = " << hk.name << "\n";
        file << "group = " << hk.group << "\n";
        file << "keys = " << joinStrings(hk.keys) << "\n";
        file << "fovX = " << hk.fovX << "\n";
        file << "fovY = " << hk.fovY << "\n";
        file << std::setprecision(0)
             << "aim_classes = "       << serialize_aim_classes(hk.aim_classes) << "\n"
             << "crosshair_detect_enabled = "  << to_bool_str(hk.crosshair_detect_enabled)  << "\n"
             << "dynamic_fov_enabled = " << to_bool_str(hk.dynamic_fov_enabled) << "\n"
             << std::fixed << std::setprecision(3)
             << "dynamic_fov_strength = " << hk.dynamic_fov_strength << "\n"
             << std::setprecision(4);

        file << std::fixed << std::setprecision(4)
             << "ctl_enabled = "          << to_bool_str(hk.ctl_enabled) << "\n"
             << "ctl_kp_x = "             << hk.ctl_kp_x << "\n"
             << "ctl_kp_y = "             << hk.ctl_kp_y << "\n"
             << "ctl_ki_x = "             << hk.ctl_ki_x << "\n"
             << "ctl_ki_y = "             << hk.ctl_ki_y << "\n"
             << "ctl_kd_x = "             << hk.ctl_kd_x << "\n"
             << "ctl_kd_y = "             << hk.ctl_kd_y << "\n"
             << "ctl_tau_unwind_sec = "   << hk.ctl_tau_unwind_sec << "\n"
             << "ctl_tau_deriv_sec = "    << hk.ctl_tau_deriv_sec << "\n"
             << "ctl_i_max = "            << hk.ctl_i_max << "\n"
             << "ctl_p_full_scale_px = "  << hk.ctl_p_full_scale_px << "\n"
             << "ctl_predict_lead_ms = "  << hk.ctl_predict_lead_ms << "\n"
             << "ctl_predict_max_velocity_px_s = " << hk.ctl_predict_max_velocity_px_s << "\n"
             << "ctl_predict_max_lead_ratio = "    << hk.ctl_predict_max_lead_ratio << "\n"
             << "ctl_k_px_per_count = "  << hk.ctl_k_px_per_count << "\n"
             << "ctl_inflight_beta = "          << hk.ctl_inflight_beta << "\n"
             << "ctl_inflight_dead_time_ms = "  << hk.ctl_inflight_dead_time_ms << "\n"
             << "ctl_y_offset = "         << hk.ctl_y_offset << "\n"
             << "ctl_y_offset_max = "     << hk.ctl_y_offset_max << "\n"
             << "ctl_hysteresis_ratio = " << hk.ctl_hysteresis_ratio << "\n"
             << "ctl_max_output_counts = " << hk.ctl_max_output_counts << "\n"
             << "ctl_max_distance_px = "    << hk.ctl_max_distance_px << "\n"
             << "ctl_match_center_ratio = " << hk.ctl_match_center_ratio << "\n"
             << "ctl_area_ratio_tol = "     << hk.ctl_area_ratio_tol << "\n"
             << "ctl_k_snap_mult = "        << hk.ctl_k_snap_mult << "\n"
             << "ctl_min_aspect = "         << hk.ctl_min_aspect << "\n"
             << "ctl_max_aspect = "         << hk.ctl_max_aspect << "\n"
             << "ctl_random_seed = "        << hk.ctl_random_seed << "\n";

        // ── 开镜档 (自动开镜生效期间取代上面的默认档) ────────────────────
        // ★ 键名 = 默认档的键名前缀 "ctl_scope_", 一一对应, 方便手改与对照。
        file << std::fixed << std::setprecision(4)
             << "ctl_scope_enabled = "          << hk.scope_ctl_enabled << "\n"
             << "ctl_scope_kp_x = "             << hk.ctl_scope.kp_x << "\n"
             << "ctl_scope_kp_y = "             << hk.ctl_scope.kp_y << "\n"
             << "ctl_scope_ki_x = "             << hk.ctl_scope.ki_x << "\n"
             << "ctl_scope_ki_y = "             << hk.ctl_scope.ki_y << "\n"
             << "ctl_scope_kd_x = "             << hk.ctl_scope.kd_x << "\n"
             << "ctl_scope_kd_y = "             << hk.ctl_scope.kd_y << "\n"
             << "ctl_scope_tau_unwind_sec = "   << hk.ctl_scope.tau_unwind_sec << "\n"
             << "ctl_scope_tau_deriv_sec = "    << hk.ctl_scope.tau_deriv_sec << "\n"
             << "ctl_scope_i_max = "            << hk.ctl_scope.i_max << "\n"
             << "ctl_scope_p_full_scale_px = "  << hk.ctl_scope.p_full_scale_px << "\n"
             << "ctl_scope_predict_lead_ms = "  << hk.ctl_scope.predict_lead_ms << "\n"
             << "ctl_scope_predict_max_velocity_px_s = "
             << hk.ctl_scope.predict_max_velocity_px_s << "\n"
             << "ctl_scope_predict_max_lead_ratio = "
             << hk.ctl_scope.predict_max_lead_ratio << "\n"
             << "ctl_scope_k_px_per_count = "  << hk.ctl_scope.k_px_per_count << "\n"
             << "ctl_scope_inflight_beta = "          << hk.ctl_scope.inflight_beta << "\n"
             << "ctl_scope_inflight_dead_time_ms = "  << hk.ctl_scope.inflight_dead_time_ms << "\n"
             << "ctl_scope_max_output_counts = " << hk.ctl_scope.max_output_counts << "\n"
             << "ctl_scope_random_seed = "       << hk.ctl_scope.random_seed << "\n";

        file << "trigger_enabled = "        << to_bool_str(hk.trigger_enabled) << "\n"
             << "trigger_fire_delay = "     << hk.trigger_fire_delay << "\n"
             << "trigger_fire_duration = "  << hk.trigger_fire_duration << "\n"
             << "trigger_fire_interval = "  << hk.trigger_fire_interval << "\n"
             << "trigger_y_percent = "      << hk.trigger_y_percent << "\n"
             << "trigger_delay_jitter_ms = "    << hk.trigger_delay_jitter_ms << "\n"
             << "trigger_duration_jitter_ms = " << hk.trigger_duration_jitter_ms << "\n"
             << "trigger_interval_jitter_ms = " << hk.trigger_interval_jitter_ms << "\n"
             << "trigger_switch_cooldown_ms = " << hk.trigger_switch_cooldown_ms << "\n"
             << "trigger_auto_scope = "     << hk.trigger_auto_scope << "\n"
             << "trigger_scope_delay_ms = " << hk.trigger_scope_delay_ms << "\n"
             << "trigger_auto_stop = "      << hk.trigger_auto_stop << "\n"
             << "trigger_stop_ms = "        << hk.trigger_stop_ms << "\n";

        file << "aim_path_mode = "          << hk.aim_path_mode << "\n"
             << "aim_path_influence = "     << hk.aim_path_influence << "\n"
             << "aim_path_bezier_cx1 = "    << hk.aim_path_bezier_cx1 << "\n"
             << "aim_path_bezier_cy1 = "    << hk.aim_path_bezier_cy1 << "\n"
             << "aim_path_bezier_cx2 = "    << hk.aim_path_bezier_cx2 << "\n"
             << "aim_path_bezier_cy2 = "    << hk.aim_path_bezier_cy2 << "\n"
             << "aim_path_wind_gravity = "  << hk.aim_path_wind_gravity << "\n"
             << "aim_path_wind_wind = "     << hk.aim_path_wind_wind << "\n"
             << "aim_path_wind_step = "     << hk.aim_path_wind_step << "\n"
             << "aim_path_wind_distance = " << hk.aim_path_wind_distance << "\n"
             << "aim_path_wind_threshold = " << hk.aim_path_wind_threshold << "\n";

        // 手绘曲线采样点。★ 换行折行: 160KB 挤在一行虽然 SimpleIni 能吃下
        //   (它整文件 fread 进内存, 无行长上限), 但任何文本编辑器都会卡死,
        //   人也没法看。每 64 个值一折行, 解析端已按空白/逗号跳过。
        if (hk.aim_path_custom_samples && !hk.aim_path_custom_samples->empty())
        {
            const auto& s = *hk.aim_path_custom_samples;
            file << "aim_path_custom_samples = ";
            for (size_t i = 0; i < s.size(); ++i)
            {
                if (i) { file << ","; if (i % 64 == 0) file << "\n    "; }
                file << static_cast<int>(std::lround(
                    std::clamp(static_cast<double>(s[i]), -1.0, 1.0) * 10000.0));
            }
            file << "\n";
        }

        file << "\n";
    }

    for (size_t i = 0; i < crosshair_colors.size(); ++i)
    {
        const auto& c = crosshair_colors[i];
        file << "[crosshair_color." << i << "]\n"
             << "name = "    << c.name    << "\n"
             << "enabled = " << to_bool_str(c.enabled) << "\n"
             << "h_low = "   << c.h_low   << "\n"
             << "h_high = "  << c.h_high  << "\n"
             << "s_min = "   << c.s_min   << "\n"
             << "s_max = "   << c.s_max   << "\n"
             << "v_min = "   << c.v_min   << "\n"
             << "v_max = "   << c.v_max   << "\n\n";
    }

    file << "[target_stabilizer]\n"
         << "target_hysteresis_ratio = "   << target_hysteresis_ratio << "\n"
         << "target_max_distance_px = "    << target_max_distance_px << "\n"
         << "target_match_center_ratio = " << target_match_center_ratio << "\n"
         << "target_area_ratio_tol = "     << target_area_ratio_tol << "\n"
         << "target_k_snap_mult = "        << target_k_snap_mult << "\n"
         << "target_min_aspect = "         << target_min_aspect << "\n"
         << "target_max_aspect = "         << target_max_aspect << "\n\n";

    file.close();
    return true;
}

void Config::sync_class_filters_from_model(int class_count,
                                           const std::vector<std::string>& class_names)
{
    std::unordered_map<int, ClassFilterState> keep;
    keep.reserve(class_filters.size());
    for (const auto& cf : class_filters)
        keep[cf.class_id] = cf;

    class_filters.clear();
    class_filters.reserve(static_cast<size_t>(std::max(0, class_count)));

    for (int id = 0; id < class_count; ++id)
    {
        ClassFilterState st;
        auto it = keep.find(id);
        if (it != keep.end())
            st = it->second;
        st.class_id = id;

        if (id < static_cast<int>(class_names.size()) && !class_names[static_cast<size_t>(id)].empty())
            st.class_name = class_names[static_cast<size_t>(id)];
        else if (st.class_name.empty())
            st.class_name = "class_" + std::to_string(id);

        class_filters.push_back(std::move(st));
    }

}
