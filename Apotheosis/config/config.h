#ifndef CONFIG_H
#define CONFIG_H

#include <memory>
#include <string>
#include <vector>

enum class ClassBucket
{
    Delete = 0,
    Filter = 1,
    Aim = 2,
};

struct ClassFilterState
{
    int class_id = 0;
    std::string class_name;
    ClassBucket bucket = ClassBucket::Delete;
};

struct HotkeyAimClass
{
    int   class_id = 0;
    float y_offset = 0.5f;
    float y_offset_max = 0.5f;
    float min_conf = 0.0f;
};

struct HotkeyProfile
{
    std::string name = "Aim";
    std::string group = u8"默认";
    std::vector<std::string> keys;

    int fovX = 106;
    int fovY = 74;

    std::vector<HotkeyAimClass> aim_classes;

    bool crosshair_detect_enabled = false;

    bool  dynamic_fov_enabled  = false;
    float dynamic_fov_strength = 0.60f;

    double ctl_kp_x = 35.0;
    double ctl_kp_y = 35.0;
    double ctl_ki_x = 0.0;
    double ctl_ki_y = 0.0;
    double ctl_kd_x = 0.0;
    double ctl_kd_y = 0.0;

    double ctl_tau_unwind_sec = 0.030;
    double ctl_tau_deriv_sec = 0.020;
    double ctl_i_max = 0.0;
    int    ctl_max_output_counts = 200;
    double ctl_p_full_scale_px = 0.0;

    // ── Smith 在途自身位移补偿 (一帧拉枪) ──────────────────────────────────
    // 扣除链路死区内已下发但画面尚未显现的自身位移，避免重复下令导致过冲振荡。
    // k_px_per_count: 灵敏度折算系数 (像素/计数，0 = 关闭自身位移补偿)
    // inflight_beta : 补偿阻尼系数 (默认 0.8，可配置范围 0.0 ~ 2.0)
    // inflight_dead_time_ms: Smith 补偿专用的死区时间 (ms，默认 46.0ms 实测死区)
    double ctl_k_px_per_count = 0.0;
    double ctl_inflight_beta  = 0.8;
    double ctl_inflight_dead_time_ms = 46.0;

    // ── 在途补偿（预测提前量）─────────────────────────────────────────────
    // 链路（采集→推理→瞄准→下发→游戏渲染）有几十毫秒延迟，等这一拍算完
    // 目标已经跑掉了。预测按目标速度把瞄准点往前推一段来抵消它。
    //
    // 生效规则：ctl_predict_lead_ms == 0 时预测整体不生效，
    //           下面两个参数不读取（行为与未加该功能时逐帧一致）。
    //           lead_ms != 0 时，下面两个各自 0 表示【不限制】。

    // 预测提前时间（毫秒）= 整条链路的【全部延迟】，由用户实测后填入。
    // ★ 只用这一个来源，程序不再自动往里加任何东西（来源单一，不会重复计算）。
    // 包含采集、推理、瞄准、下发，以及游戏渲染的内部延迟 —— 全都算在这一个值里。
    // ★ 0 = 关闭（默认）。这是总开关。
    double ctl_predict_lead_ms = 0.0;

    // 速度上限（像素/秒）。估计速度超过它时【钳住速度】——保留方向、只压大小。
    // ★ 0 = 不限制（默认）。先看日志里打出的实际速度再定这个值。
    double ctl_predict_max_velocity_px_s = 0.0;

    // 预测距离上限，单位是【目标框对角线倍数】。
    // 1.0 = 最多提前一个对角线；0.5 = 半个；★ 0 = 不限制（默认）。
    // 用相对量而不是绝对像素，是为了让远近目标的保护尺度一致。
    double ctl_predict_max_lead_ratio = 0.0;

    double ctl_y_offset = 0.5;
    double ctl_y_offset_max = 0.5;

    double ctl_hysteresis_ratio = 1.3;

    bool ctl_enabled = false;

    double ctl_max_distance_px = 0.0;

    double ctl_match_center_ratio = 0.5;
    double ctl_area_ratio_tol = 2.0;
    double ctl_k_snap_mult = 1.15;
    double ctl_min_aspect = 0.2;
    double ctl_max_aspect = 5.0;

    int ctl_random_seed = 0;

    bool trigger_enabled = false;
    int  trigger_fire_delay = 0;
    int  trigger_fire_duration = 0;
    int  trigger_fire_interval = 200;
    int  trigger_y_percent = 100;
    int  trigger_delay_jitter_ms    = 0;
    int  trigger_duration_jitter_ms = 0;
    int  trigger_interval_jitter_ms = 0;
    int  trigger_switch_cooldown_ms = 0;
    int  trigger_auto_scope = 0;
    int  trigger_scope_delay_ms = 0;
    int  trigger_auto_stop = 0;
    int  trigger_stop_ms   = 60;

    int   aim_path_mode = 0;
    int   aim_path_influence = 25;
    float aim_path_bezier_cx1 = 0.30f;
    float aim_path_bezier_cy1 = 0.00f;
    float aim_path_bezier_cx2 = 0.70f;
    float aim_path_bezier_cy2 = 0.00f;
    float aim_path_wind_gravity   = 5.0f;
    float aim_path_wind_wind      = 2.0f;
    float aim_path_wind_step      = 10.0f;
    float aim_path_wind_distance  = 8.0f;
    int   aim_path_wind_threshold = 10;
    std::shared_ptr<const std::vector<float>> aim_path_custom_samples;

};

struct CrosshairColorProfileConfig
{
    std::string name = "Red-Low";
    bool enabled = true;
    int h_low = 0;
    int h_high = 10;
    int s_min = 120;
    int s_max = 255;
    int v_min = 120;
    int v_max = 255;
};

inline constexpr int kFixedMaxDetections = 20;

class Config
{
public:
    std::string capture_device;
    std::string capture_format;
    int  capture_width  = 0;
    int  capture_height = 0;
    int  capture_fps    = 0;
    bool capture_gpu_decode = true;

    int detection_resolution = 320;
    bool circle_mask = true;

    std::string input_method = "MAKCU";
    int makcu_baudrate = 115200;
    std::string makcu_port = "COM0";
    int makcu_new_baudrate = 6000000;
    std::string makcu_new_port = "COM0";
    // 第二台 MAKCUNEW(键盘那台)。
    //
    // 本项目部署是两块独立硬件: 一块接游戏机+鼠标, 一块接游戏机+键盘。
    // 位移/按键/滚轮走 makcu_new_port, 键盘(tapKey)与自动急停的屏蔽命令走这一台。
    //
    // 留空表示"没有第二台", 此时键盘动作回落到第一台, 行为与旧版完全一致
    // (单硬件同时插键鼠的场景仍然可用)。
    int makcu_new_baudrate_kbd = 6000000;
    std::string makcu_new_port_kbd = "";
    std::string kmbox_net_ip = "192.168.2.88";
    std::string kmbox_net_port = "6234";
    std::string kmbox_net_uuid = "12345";

    std::string backend = "TRT";
    std::string ai_model = "sunxds_0.5.6.engine";
    float confidence_threshold = 0.15f;
    float nms_threshold = 0.50f;
    int max_detections = kFixedMaxDetections;
    bool  small_target_enabled = false;
    float small_target_area_frac = 0.012f;
    float small_target_confidence = 0.06f;
    bool fixed_input_size = false;

    bool use_cuda_graph = true;
    bool use_spin_wait_sync = true;
    int spin_wait_timeout_ms = 50;

    bool use_process_boost = true;
    bool use_mmcss = true;
    std::string mmcss_task_name = "Games";

    int gpuMemoryReserveMB = 2048;
    bool enableGpuExclusiveMode = true;
    int cpuCoreReserveCount = 4;
    int systemMemoryReserveMB = 2048;

    bool show_window = true;
    bool show_fps = false;

    bool replay_record_enabled = false;
    int  replay_seconds = 10;
    float replay_playback_speed = 0.25f;
    std::vector<std::string> screenshot_button;
    int screenshot_delay = 500;
    bool verbose = false;

    bool   auto_capture_enabled    = false;
    bool   auto_capture_use_high   = true;
    float  auto_capture_high_conf  = 0.85f;
    bool   auto_capture_use_low    = false;
    float  auto_capture_low_conf   = 0.30f;
    bool   auto_capture_any_detection = false;
    int    auto_capture_cooldown_ms = 200;
    std::vector<std::string> auto_capture_force_keys;
    std::string auto_capture_output_dir = "screenshots/auto";
    bool   auto_capture_save_label = true;

    std::vector<ClassFilterState> class_filters;

    // ── 全局选靶与稳定器 (独立于热键的视觉目标感知层) ────────────────────────
    double target_hysteresis_ratio   = 1.3;
    double target_max_distance_px    = 0.0;
    double target_match_center_ratio = 0.5;
    double target_area_ratio_tol     = 2.0;
    double target_k_snap_mult        = 1.15;
    double target_min_aspect         = 0.2;
    double target_max_aspect         = 5.0;

    int crosshair_rect_w = 40;
    int crosshair_rect_h = 40;
    int crosshair_min_pixel_count = 4;
    int crosshair_close_radius = 1;

    std::vector<CrosshairColorProfileConfig> crosshair_colors;

    std::vector<HotkeyProfile> hotkeys;
    std::string active_hotkey_group;

    bool        macro_enabled = false;
    std::string macro_script_path;
    bool        macro_primary_button_events = false;

    bool loadConfig(const std::string& filename = "config.ini");
    bool saveConfig(const std::string& filename = "config.ini");

    void retargetConfigPath(const std::string& filename);

    std::string configPath() const { return config_path; }
    void setConfigPath(const std::string& path) { config_path = path; }

    void sync_class_filters_from_model(int class_count,
                                       const std::vector<std::string>& class_names);

    std::string joinStrings(const std::vector<std::string>& vec,
                            const std::string& delimiter = ",") const;

private:
    std::vector<std::string> splitString(const std::string& str, char delimiter = ',') const;
    std::string config_path;
    void writeDefaultsInPlace();
};

#endif // CONFIG_H
