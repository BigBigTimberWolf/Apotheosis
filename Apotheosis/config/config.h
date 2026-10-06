#ifndef CONFIG_H
#define CONFIG_H

#include <algorithm>
#include <array>
#include <memory>
#include <string>
#include <vector>

#include "control/recovered_pid.h"
#include "macro/macro_config.h"
#include "runtime/auto_flash_rules.h"

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
    float x_offset = 0.5f;
    float x_offset_max = 0.5f;
};

// Ordered trigger target classes. Coordinates use the same box-relative
// convention as aim classes: X=0 is left, Y=1 is top. The hit zone is a
// rectangle centered on this point, sized as a percentage of the box.
struct TriggerAimClass
{
    int class_id = -1;
    float x_offset = 0.5f;
    float y_offset = 0.5f;
    int range_x_percent = 100;
    int range_y_percent = 100;
};

struct TriggerParams
{
    bool trigger_enabled = false;
    int trigger_fire_mode = -1; // AM: 0 smart click, 1 burst, 2 continuous, 3 smart continuous; -1 migrate
    int trigger_mode = 0; // 0 = existing, 1 = snap/fire/return, 2 = spin/search/fire
    double trigger_snap_px_per_count = 1.0;
    int trigger_snap_max_counts = 500;
    double trigger_snap_tolerance_px = 3.0;
    int trigger_snap_counts_per_second = 4000;
    int trigger_return_counts_per_second = 16000;
    int trigger_return_y_percent = 75;
    int trigger_spin_counts_per_turn = 2400;
    int trigger_spin_step_degrees = 90;
    int trigger_spin_step_ms = 40;
    int trigger_spin_hold_ms = 300;
    int trigger_spin_counts_per_second = 5000;
    int trigger_spin_turns = 1;
    int trigger_snap_fire_hold_ms = 30;
    int trigger_snap_cooldown_ms = 200;
    int trigger_flash_disappear_ms = 80;
    int trigger_fire_delay = 0;
    bool trigger_prearm_enabled = false;
    int trigger_prearm_expand_percent = 50;
    int trigger_fire_duration = 0;
    int trigger_fire_interval = 200;
    int trigger_y_percent = 100;
    int trigger_delay_jitter_ms = 0;
    int trigger_duration_jitter_ms = 0;
    int trigger_interval_jitter_ms = 0;
    int trigger_switch_cooldown_ms = 0;
    int trigger_auto_scope = 0;
    int trigger_scope_delay_ms = 0;
    int trigger_auto_stop = 0;
    int trigger_stop_before_ms = 0;
    int trigger_stop_after_ms = 60;
    bool trigger_weapon_switch31 = false;
    int trigger_switch31_delay_ms = 50;
    int trigger_switch31_step_ms = 20;
    int trigger_loss_delay_ms = 20;
};

struct HotkeyProfile
{
    bool enabled = true;
    std::string name = "Aim";
    std::string group = u8"默认";
    std::vector<std::string> keys;
    bool keys_chord = false; // Require both configured keys at the same time.
    std::string activation_key; // Select this whole profile among identical bindings.
    bool activation_selected = false;

    int fovX = 106;
    int fovY = 74;
    bool block_hotkey = false;
    bool mask_x = false;
    bool mask_y = false; // Block physical mouse input only while actively aiming at a target.
    int mask_delay_ms = 0; // Delay physical XY masking after active aiming begins.
    bool unlock_x = false; // Disable automatic aiming on this axis.
    bool unlock_y = false;
    int unlock_y_delay_ms = 0; // Begin automatic Y unlock after target and hotkey overlap.
    int aim_delay_ms = 0; // Wait after both the hotkey and a valid target are present.

    std::vector<HotkeyAimClass> aim_classes;
    std::vector<TriggerAimClass> trigger_classes;

    bool crosshair_detect_enabled = false;
    bool laser_detect_enabled = false;
    bool aimpoint_recoil_enabled = false;

    // Separate recovered-controller parameters. Legacy ctl_* gains have a
    // different dt-dependent meaning and must never be read as these gains.
    control::RecoveredPidConfig recovered_pid{};
    // Explicit, persistent selection: each key always selects the same set.
    std::string primary_select_key = "Key1";
    std::string secondary_select_key = "Key2";
    control::RecoveredPidConfig recovered_secondary_pid{};
    control::RecoveredPidConfig recovered_scope_pid{};

    bool  dynamic_fov_enabled  = false;
    int dynamic_fov_size = 40;
    int dynamic_fov_shrink_ms = 200;
    int dynamic_fov_expand_ms = 120;

    double ctl_y_offset = 0.5;
    double ctl_y_offset_max = 0.5;
    double ctl_x_offset = 0.5;
    double ctl_x_offset_max = 0.5;

    bool ctl_enabled = false;

    int ctl_random_seed = 0;

    int scope_ctl_enabled = 0;

    bool trigger_enabled = false;
    int trigger_fire_mode = -1; // AM: 0 smart click, 1 burst, 2 continuous, 3 smart continuous; -1 migrate
    int trigger_mode = 0;
    double trigger_snap_px_per_count = 1.0;
    int trigger_snap_max_counts = 500;
    double trigger_snap_tolerance_px = 3.0;
    int trigger_snap_counts_per_second = 4000;
    int trigger_return_counts_per_second = 16000;
    int trigger_return_y_percent = 75;
    int trigger_spin_counts_per_turn = 2400;
    int trigger_spin_step_degrees = 90;
    int trigger_spin_step_ms = 40;
    int trigger_spin_hold_ms = 300;
    int trigger_spin_counts_per_second = 5000;
    int trigger_spin_turns = 1;
    int trigger_snap_fire_hold_ms = 30;
    int trigger_snap_cooldown_ms = 200;
    int trigger_flash_disappear_ms = 80;
    int  trigger_fire_delay = 0;
    bool trigger_prearm_enabled = false;
    int trigger_prearm_expand_percent = 50;
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
    int  trigger_stop_before_ms = 0;
    int  trigger_stop_after_ms = 60;
    bool trigger_weapon_switch31 = false;
    int  trigger_switch31_delay_ms = 50;
    int  trigger_switch31_step_ms = 20; // 旧配置兼容；切枪按键时长与间隔固定 20ms
    bool secondary_trigger_custom = false;
    int trigger_loss_delay_ms = 20;
    TriggerParams secondary_trigger{};

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
    bool aim_path_neural_trained = false;
    std::array<float, 25> aim_path_neural_weights{};
    int aim_path_neural_examples = 0;
    float aim_path_neural_validation_rmse = 0.0f;
    float aim_path_neural_validation_p95 = 0.0f;
    float aim_path_neural_slope_variation = 0.0f;

};

inline TriggerParams triggerParamsOf(const HotkeyProfile& hk)
{
    TriggerParams p;
    p.trigger_enabled = hk.trigger_enabled;
    p.trigger_fire_mode = hk.trigger_fire_mode;
    p.trigger_mode = hk.trigger_mode;
    p.trigger_snap_px_per_count = hk.trigger_snap_px_per_count;
    p.trigger_snap_max_counts = hk.trigger_snap_max_counts;
    p.trigger_snap_tolerance_px = hk.trigger_snap_tolerance_px;
    p.trigger_snap_counts_per_second = hk.trigger_snap_counts_per_second;
    p.trigger_return_counts_per_second = hk.trigger_return_counts_per_second;
    p.trigger_return_y_percent = hk.trigger_return_y_percent;
    p.trigger_spin_counts_per_turn = hk.trigger_spin_counts_per_turn;
    p.trigger_spin_step_degrees = hk.trigger_spin_step_degrees;
    p.trigger_spin_step_ms = hk.trigger_spin_step_ms;
    p.trigger_spin_hold_ms = hk.trigger_spin_hold_ms;
    p.trigger_spin_counts_per_second = hk.trigger_spin_counts_per_second;
    p.trigger_spin_turns = hk.trigger_spin_turns;
    p.trigger_snap_fire_hold_ms = hk.trigger_snap_fire_hold_ms;
    p.trigger_snap_cooldown_ms = hk.trigger_snap_cooldown_ms;
    p.trigger_flash_disappear_ms = hk.trigger_flash_disappear_ms;
    p.trigger_fire_delay = hk.trigger_fire_delay;
    p.trigger_prearm_enabled = hk.trigger_prearm_enabled;
    p.trigger_prearm_expand_percent = hk.trigger_prearm_expand_percent;
    p.trigger_fire_duration = hk.trigger_fire_duration;
    p.trigger_fire_interval = hk.trigger_fire_interval;
    p.trigger_y_percent = hk.trigger_y_percent;
    p.trigger_delay_jitter_ms = hk.trigger_delay_jitter_ms;
    p.trigger_duration_jitter_ms = hk.trigger_duration_jitter_ms;
    p.trigger_interval_jitter_ms = hk.trigger_interval_jitter_ms;
    p.trigger_switch_cooldown_ms = hk.trigger_switch_cooldown_ms;
    p.trigger_auto_scope = hk.trigger_auto_scope;
    p.trigger_scope_delay_ms = hk.trigger_scope_delay_ms;
    p.trigger_auto_stop = hk.trigger_auto_stop;
    p.trigger_stop_before_ms = hk.trigger_stop_before_ms;
    p.trigger_stop_after_ms = hk.trigger_stop_after_ms;
    p.trigger_weapon_switch31 = hk.trigger_weapon_switch31;
    p.trigger_switch31_delay_ms = hk.trigger_switch31_delay_ms;
    p.trigger_switch31_step_ms = hk.trigger_switch31_step_ms;
    p.trigger_loss_delay_ms = hk.trigger_loss_delay_ms;
    return p;
}

inline void applyTriggerParams(HotkeyProfile& hk, const TriggerParams& p)
{
    hk.trigger_enabled = p.trigger_enabled;
    hk.trigger_fire_mode = p.trigger_fire_mode;
    hk.trigger_mode = p.trigger_mode;
    hk.trigger_snap_px_per_count = p.trigger_snap_px_per_count;
    hk.trigger_snap_max_counts = p.trigger_snap_max_counts;
    hk.trigger_snap_tolerance_px = p.trigger_snap_tolerance_px;
    hk.trigger_snap_counts_per_second = p.trigger_snap_counts_per_second;
    hk.trigger_return_counts_per_second = p.trigger_return_counts_per_second;
    hk.trigger_return_y_percent = p.trigger_return_y_percent;
    hk.trigger_spin_counts_per_turn = p.trigger_spin_counts_per_turn;
    hk.trigger_spin_step_degrees = p.trigger_spin_step_degrees;
    hk.trigger_spin_step_ms = p.trigger_spin_step_ms;
    hk.trigger_spin_hold_ms = p.trigger_spin_hold_ms;
    hk.trigger_spin_counts_per_second = p.trigger_spin_counts_per_second;
    hk.trigger_spin_turns = p.trigger_spin_turns;
    hk.trigger_snap_fire_hold_ms = p.trigger_snap_fire_hold_ms;
    hk.trigger_snap_cooldown_ms = p.trigger_snap_cooldown_ms;
    hk.trigger_flash_disappear_ms = p.trigger_flash_disappear_ms;
    hk.trigger_fire_delay = p.trigger_fire_delay;
    hk.trigger_prearm_enabled = p.trigger_prearm_enabled;
    hk.trigger_prearm_expand_percent = p.trigger_prearm_expand_percent;
    hk.trigger_fire_duration = p.trigger_fire_duration;
    hk.trigger_fire_interval = p.trigger_fire_interval;
    hk.trigger_y_percent = p.trigger_y_percent;
    hk.trigger_delay_jitter_ms = p.trigger_delay_jitter_ms;
    hk.trigger_duration_jitter_ms = p.trigger_duration_jitter_ms;
    hk.trigger_interval_jitter_ms = p.trigger_interval_jitter_ms;
    hk.trigger_switch_cooldown_ms = p.trigger_switch_cooldown_ms;
    hk.trigger_auto_scope = p.trigger_auto_scope;
    hk.trigger_scope_delay_ms = p.trigger_scope_delay_ms;
    hk.trigger_auto_stop = p.trigger_auto_stop;
    hk.trigger_stop_before_ms = p.trigger_stop_before_ms;
    hk.trigger_stop_after_ms = p.trigger_stop_after_ms;
    hk.trigger_weapon_switch31 = p.trigger_weapon_switch31;
    hk.trigger_switch31_delay_ms = p.trigger_switch31_delay_ms;
    hk.trigger_switch31_step_ms = p.trigger_switch31_step_ms;
    hk.trigger_loss_delay_ms = p.trigger_loss_delay_ms;
}

struct CrosshairColorProfileConfig
{
    std::string name = "Red-Low";
    bool enabled = true;
    bool exact_hsv = false;
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
    std::string capture_source = "device"; // device | udp | tcp | ndi | dxgi
    std::string capture_device_api = "mf"; // mf | dshow; applies to standard capture devices
    std::string capture_stream_url;
    std::string capture_ndi_source;
    // DXGI desktop capture: the monitor's device name (e.g. \\.\DISPLAY1); empty = primary monitor.
    std::string capture_dxgi_output;
    std::string capture_format;
    int  capture_width  = 0;
    int  capture_height = 0;
    int  capture_fps    = 0;
    bool capture_gpu_decode = true;

    int detection_resolution = 320;
    bool circle_mask = false;

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
    std::string ferrum_port = "";
    int ferrum_baudrate = 3000000;
    std::string cpbox_port;
    std::string dhzbox_ip = "192.168.2.88";
    int dhzbox_port = 8888;
    int dhzbox_key = 88;
    std::string cat_ip = "192.168.7.1";
    int cat_port = 8888;
    std::string cat_uuid = "";
    int cat_monitor_port = 1234;

    std::string backend = "TRT";
    std::string ai_model = "sunxds_0.5.6.engine";
    float confidence_threshold = 0.15f;
    float nms_threshold = 0.50f;
    int max_detections = kFixedMaxDetections;
    bool  small_target_enabled = false;
    float small_target_area_frac = 0.012f;
    float small_target_confidence = 0.06f;
    bool fixed_input_size = false;

    // ── 引擎精度 (导出 .engine 时生效; 改完必须删掉旧 .engine 重新导出) ──────
    //   "fp16" : 现状, 默认。网络 IO 钉成 kHALF, 关 TF32。
    //   "int8" : TensorRT 隐式量化 + 熵校准 (IInt8EntropyCalibrator2)。
    //            Turing(sm_75) 有 INT8 tensor core, 吞吐约为 FP16 的 2 倍,
    //            代价是需要校准图集, 且量化误差对小目标最敏感 —— 必须实测精度。
    //   ⚠ FP8 不在选项里: 它需要 Ada(sm_89)/Hopper(sm_90)+, sm_75 硬件不支持。
    std::string engine_precision = "fp16";
    std::string int8_calib_dir   = "calib";  // 校准图目录(相对 exe 目录或绝对路径)
    int         int8_calib_images = 200;     // 最多使用多少张 (1..2000)
    // 注意: 没有"校准批大小"这个键 —— 本工程的网络是固定 batch=1 的
    // (优化 profile 写死 Dims4{1,3,H,W}), TensorRT 要求校准批大小与网络 batch
    // 维一致, 所以它恒为 1, 不暴露成可调参数。

    bool use_cuda_graph = true;
    bool use_spin_wait_sync = false;
    int spin_wait_timeout_ms = 50;

    bool use_process_boost = false;
    bool use_mmcss = false;
    std::string mmcss_task_name = "Games";

    int gpuMemoryReserveMB = 2048;
    bool enableGpuExclusiveMode = true;
    int cpuCoreReserveCount = 0;
    int systemMemoryReserveMB = 0;

    bool show_window = true;
    bool show_fps = false;

    bool replay_record_enabled = false;
    int  replay_seconds = 10;
    float replay_playback_speed = 0.25f;
    std::vector<std::string> screenshot_button;
    int screenshot_delay = 500;
    bool verbose = false;

    bool   auto_capture_enabled    = false;
    bool   auto_capture_trigger_only = false;
    bool   auto_capture_use_high   = true;
    float  auto_capture_high_conf  = 0.85f;
    bool   auto_capture_use_low    = false;
    float  auto_capture_low_conf   = 0.30f;
    bool   auto_capture_any_detection = false;
    int    auto_capture_cooldown_ms = 200;
    std::vector<std::string> auto_capture_force_keys;
    std::string auto_capture_output_dir = "screenshots/auto";
    bool   auto_capture_save_label = true;
    // Auto flash: locked target box area as a percentage of the detection frame.
    bool auto_flash_enabled = false;
    // 全局阈值 = 「任意类别」兜底行的值（旧配置直接沿用）。
    double auto_flash_area_percent = 5.0;
    // 按瞄准类别单独设的阈值；命中的类别没有专档时回落到上面那个全局值。
    std::vector<runtime::AutoFlashRule> auto_flash_rules;
    std::string auto_flash_key;

    std::vector<ClassFilterState> class_filters;
    bool head_body_fusion_enabled = false;
    int head_body_head_class_id = -1;
    int head_body_body_class_id = -1;

    // ── 全局选靶与稳定器 (独立于热键的视觉目标感知层) ────────────────────────
    double target_hysteresis_ratio   = 1.3;
    double target_max_distance_px    = 0.0;

    int crosshair_algorithm = 0; // 0: existing, 1: whole-mask centroid
    int crosshair_rect_w = 40;
    int crosshair_rect_h = 40;
    int crosshair_offset_y = 0;
    int crosshair_min_pixel_count = 4;
    int crosshair_close_radius = 1;
    double aimpoint_recoil_speed_px_s = 30.0;
    double aimpoint_recoil_max_px = 60.0;
    std::string aimpoint_recoil_fire_key = "LeftMouseButton";

    // FF 自动标定：可选的“标定启动键”。在标定窗口按下这个键等同于点一次
    // 「开始标定」，之后再按住该档的瞄准热键才开始采样。留空表示未设置。
    std::string ff_calibration_key;

    std::vector<CrosshairColorProfileConfig> crosshair_colors;
    int laser_rect_w = 160, laser_rect_h = 240;
    int laser_center_x = 160, laser_center_y = 200;
    int laser_target_center_x = 160, laser_target_center_y = 160;
    int laser_target_rect_w = 60, laser_target_rect_h = 60;
    int laser_min_pixel_count = 10, laser_close_radius = 1;
    float laser_min_elongation = 3.0f;
    float laser_smooth = 0.5f;
    std::vector<CrosshairColorProfileConfig> laser_colors;

    std::vector<HotkeyProfile> hotkeys;
    std::string active_hotkey_group;

    bool        macro_enabled = false;
    bool macro_programs_enabled = false;
    std::string macro_stop_key = "F12";
    std::vector<macros::Program> macro_programs;
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
