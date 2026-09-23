#ifndef CONFIG_H
#define CONFIG_H

#include <algorithm>
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

// ── 瞄准控制器参数组 ────────────────────────────────────────────────────────
//
// 同一套参数在配置里存在【两份】:
//   · 默认档 HotkeyProfile::ctl_*      —— 平时(未开镜)生效
//   · 开镜档 HotkeyProfile::ctl_scope  —— 自动开镜生效期间整组取代默认档
//
// ★ 为什么要有开镜档: 开镜后游戏内灵敏度被【倍率】放大, 镜前调好的一套增益
//   与灵敏度折算填进镜内必然过冲(瞬狙最典型: 镜前一甩就在镜内飞出去)。
//   所以镜内单独一套参数; 跟随热键(默认)时逐拍与没有这个功能时完全一致。
//
// ★ 这里只装【「瞄准控制器」卡里那 18 个旋钮】。选靶/稳定器/滞回/瞄点 Y 等
//   参数不属于"控制器增益", 仍然只有热键一份, 不随开镜切档。
//
// ★ clamp() 是这两档【唯一】的夹取实现(默认档也走它), 规则只有一份 —— 见
//   Config::loadConfig 里对 ctl_* 的处理。
struct AimCtlParams
{
    double kp_x = 35.0;
    double kp_y = 35.0;
    double ki_x = 0.0;
    double ki_y = 0.0;
    double kd_x = 0.0;
    double kd_y = 0.0;

    double tau_unwind_sec = 0.030;
    double tau_deriv_sec = 0.020;
    double i_max = 0.0;
    int    max_output_counts = 200;
    double p_full_scale_px = 0.0;


    // 在途补偿 (预测提前量)。lead_ms == 0 时预测整体不生效。
    double predict_lead_ms = 0.0;
    double predict_max_velocity_px_s = 0.0;
    double predict_max_lead_ratio = 0.0;

    // ── 灵敏度折算系数 k (像素/计数) ────────────────────────────────────────
    // 画面上观测到的目标速度会被自己的追踪动作污染：准星每追近一截，画面里
    // 目标的相对位移就被抵消一截，导致喂给上面「预测提前时间」的速度系统性
    // 偏小。用这个系数把自身下发速率折算回像素、加回观测速度，就能拿到目标
    // 的真实速度。0 = 关闭这项修正(不需要标定就能用，只是预测会偏保守)。
    // 不知道填多少可以用界面上的「测算灵敏度」在线拟合。
    double k_px_per_count = 0.0;

    // ── 在途自身位移补偿 (Smith) ────────────────────────────────────────────
    // 扣除链路死区内已下发但画面尚未显现的自身位移，避免重复下令导致过冲振荡。
    // 纯计数域运算，不需要任何"每计数像素"标定。inflight_beta = 0 时关闭，
    // 与没有这个功能逐位相同。
    double inflight_beta = 1.6;          // 补偿强度(无量纲)；0=关闭
    double inflight_dead_time_ms = 46.0; // 补偿窗口(ms)；必须等于真实链路死区

    // 瞄点 Y 随机抖动的种子 (0 = 用内部固定常数)
    int random_seed = 0;

    // 夹取。★ 逐条对应 config.cpp 里原来手写的那一组, 不新增也不放松任何一条。
    void clamp()
    {
        kp_x = std::max(0.0, kp_x);
        kp_y = std::max(0.0, kp_y);
        ki_x = std::max(0.0, ki_x);
        ki_y = std::max(0.0, ki_y);
        kd_x = std::max(0.0, kd_x);
        kd_y = std::max(0.0, kd_y);
        tau_unwind_sec = std::clamp(tau_unwind_sec, 1e-4, 10.0);
        tau_deriv_sec = std::clamp(tau_deriv_sec, 0.0, 10.0);
        i_max = std::max(0.0, i_max);
        p_full_scale_px = std::max(0.0, p_full_scale_px);
        // 下面三个 0 都有明确含义(关闭/不限制), 所以只做下界与有限性保护,
        // 上界留宽, 避免把用户合理的调参夹掉。
        predict_lead_ms = std::clamp(predict_lead_ms, 0.0, 1000.0);
        predict_max_velocity_px_s = std::clamp(predict_max_velocity_px_s, 0.0, 100000.0);
        predict_max_lead_ratio = std::clamp(predict_max_lead_ratio, 0.0, 100.0);
        // ★ 物理合法范围与标定器的拟合上限一致(见 sensitivity_calibrator.h)。
        k_px_per_count = std::clamp(k_px_per_count, 0.0, 10.0);
        max_output_counts = std::clamp(max_output_counts, 1, 1000);
        random_seed = std::max(0, random_seed);
        // ★ 上限 3.0：实测 beta=2.0 在 60fps 已经发散(尾段 56px)，3.0 用来挡住
        //   填错量级的配置，不是可用值。0 合法(=关闭)，不夹掉。
        inflight_beta = std::clamp(inflight_beta, 0.0, 3.0);
        // ★ 窗口不许离真实链路死区太远：填错窗口等于把"还没生效的位移"算错。
        inflight_dead_time_ms = std::clamp(inflight_dead_time_ms, 0.0, 1000.0);
    }
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

    // ── 在途自身位移补偿 (Smith) ─────────────────────────────────────────
    // 扣除链路死区内已下发但画面尚未显现的自身位移，避免重复下令导致过冲振荡。
    // 纯计数域运算，不需要任何"每计数像素"标定。0 = 关闭。
    double ctl_inflight_beta = 1.6;
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

    // 灵敏度折算系数 k (像素/计数)。画面观测到的目标速度会被自己的追踪动作
    // 污染(准星追近一截，画面里目标的相对位移就被抵消一截)，导致喂给上面
    // 预测的速度系统性偏小。这里把自身下发速率折算回像素、加回观测速度，
    // 就能拿到目标真实速度。0 = 关闭这项修正。可以用界面「测算灵敏度」在线拟合。
    double ctl_k_px_per_count = 0.0;

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

    // ── 开镜档 (自动开镜生效期间取代上面的 ctl_* 整组) ────────────────────
    // ★ scope_ctl_enabled: 0 = 跟随热键默认档(默认值, 逐拍与没有这个功能一致);
    //                      1 = 开关按下右键之后改用 ctl_scope。
    // ★ 判定在 runtime/aim_loop.cpp: 只有【自动开镜真的按下了右键】且热键仍被
    //   按住的那几拍才切档 —— 不是"只要开镜就切"。
    int scope_ctl_enabled = 0;
    AimCtlParams ctl_scope;

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

// ── 默认档 ↔ AimCtlParams 的搬运 ────────────────────────────────────────────
//
// 两个用途, 都用同一份实现:
//   1) Config::loadConfig 的夹取: 默认档借道 AimCtlParams::clamp() 走【同一份】
//      规则, 免得默认档与开镜档的夹取各写一遍、日后漂移。
//   2) 界面上的「一键复制指定热键的默认瞄准参数」: 把别的热键的【默认档】
//      搬进本热键的【开镜档】(复制的是默认档, 不是对方的开镜档)。
//
// ★ 故意放在头文件里(非 config.cpp): 它是纯搬运, 逻辑测试不需要链 config.cpp
//   就能钉住它。
inline AimCtlParams ctlParamsOf(const HotkeyProfile& hk)
{
    AimCtlParams p;
    p.kp_x = hk.ctl_kp_x;
    p.kp_y = hk.ctl_kp_y;
    p.ki_x = hk.ctl_ki_x;
    p.ki_y = hk.ctl_ki_y;
    p.kd_x = hk.ctl_kd_x;
    p.kd_y = hk.ctl_kd_y;
    p.tau_unwind_sec = hk.ctl_tau_unwind_sec;
    p.tau_deriv_sec = hk.ctl_tau_deriv_sec;
    p.i_max = hk.ctl_i_max;
    p.max_output_counts = hk.ctl_max_output_counts;
    p.p_full_scale_px = hk.ctl_p_full_scale_px;
    p.predict_lead_ms = hk.ctl_predict_lead_ms;
    p.predict_max_velocity_px_s = hk.ctl_predict_max_velocity_px_s;
    p.predict_max_lead_ratio = hk.ctl_predict_max_lead_ratio;
    p.k_px_per_count = hk.ctl_k_px_per_count;
    p.inflight_beta = hk.ctl_inflight_beta;
    p.inflight_dead_time_ms = hk.ctl_inflight_dead_time_ms;
    p.random_seed = hk.ctl_random_seed;
    return p;
}

inline void applyCtlParams(HotkeyProfile& hk, const AimCtlParams& p)
{
    hk.ctl_kp_x = p.kp_x;
    hk.ctl_kp_y = p.kp_y;
    hk.ctl_ki_x = p.ki_x;
    hk.ctl_ki_y = p.ki_y;
    hk.ctl_kd_x = p.kd_x;
    hk.ctl_kd_y = p.kd_y;
    hk.ctl_tau_unwind_sec = p.tau_unwind_sec;
    hk.ctl_tau_deriv_sec = p.tau_deriv_sec;
    hk.ctl_i_max = p.i_max;
    hk.ctl_max_output_counts = p.max_output_counts;
    hk.ctl_p_full_scale_px = p.p_full_scale_px;
    hk.ctl_predict_lead_ms = p.predict_lead_ms;
    hk.ctl_predict_max_velocity_px_s = p.predict_max_velocity_px_s;
    hk.ctl_predict_max_lead_ratio = p.predict_max_lead_ratio;
    hk.ctl_k_px_per_count = p.k_px_per_count;
    hk.ctl_inflight_beta = p.inflight_beta;
    hk.ctl_inflight_dead_time_ms = p.inflight_dead_time_ms;
    hk.ctl_random_seed = p.random_seed;
}

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
