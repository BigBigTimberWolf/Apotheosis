#include "config/config.h"

#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

namespace
{

int g_failures = 0;

void check(bool ok, const std::string& what)
{
    if (!ok)
    {
        std::printf("  [FAIL] %s\n", what.c_str());
        ++g_failures;
    }
}

std::string write_config(const std::string& path, const std::string& extra = "")
{
    std::ofstream f(path, std::ios::binary);
    f << "# 配置解析回归用配置\n"
      << "[hotkey.0]\n"
      << "name = Aim\n"
      << "group = 默认\n"
      << "keys = RightMouseButton\n"
      << "fovX = 106\n"
      << "fovY = 74\n"
      << extra;
    f.close();
    return path;
}

// 全局段(section "")的键必须写在第一个 [section] 之前, 否则会被 INI 解析器
// 归到那个段里, 而 loadConfig 是用 section "" 去读它们的 —— 键会静默读不到。
std::string write_global_config(const std::string& path, const std::string& globals)
{
    std::ofstream f(path, std::ios::binary);
    f << "# 配置解析回归用配置(全局键 + 一个热键段)\n"
      << globals
      << "[hotkey.0]\n"
      << "name = Aim\n"
      << "group = 默认\n"
      << "keys = RightMouseButton\n"
      << "fovX = 106\n"
      << "fovY = 74\n";
    f.close();
    return path;
}

}

int main()
{
    std::printf("=== config_migration_test: 配置解析 ===\n");

    std::printf("\n[1] 热键字段的解析往返\n");
    {
        const std::string p = write_config("basic.ini",
            "fovX = 120\nfovY = 90\n"
            "crosshair_detect_enabled = true\n"
            "dynamic_fov_enabled = true\ndynamic_fov_strength = 0.40\n"
            "aim_classes = 3:0.100:0.900:0.250\n");

        Config c;
        check(c.loadConfig(p), "最小配置能加载");
        check(!c.hotkeys.empty(), "解析出了热键组");
        if (!c.hotkeys.empty())
        {
            const auto& hp = c.hotkeys[0];
            check(hp.name == "Aim", "name 原样读回");
            check(hp.keys.size() == 1 && hp.keys[0] == "RightMouseButton",
                  "keys 原样读回");
            check(hp.fovX == 120 && hp.fovY == 90, "fovX/fovY 原样读回");
            check(hp.crosshair_detect_enabled,
                  "crosshair_detect_enabled 原样读回");
            check(hp.dynamic_fov_enabled, "dynamic_fov_enabled 原样读回");
            check(hp.dynamic_fov_strength > 0.39f && hp.dynamic_fov_strength < 0.41f,
                  "dynamic_fov_strength 原样读回");
            check(hp.aim_classes.size() == 1 && hp.aim_classes[0].class_id == 3,
                  "aim_classes 原样读回");
        }
    }

    std::printf("\n[2] 缺键取默认\n");
    {
        const std::string p = write_config("missing.ini");
        Config c;
        check(c.loadConfig(p), "只有段头的配置能加载");
        if (!c.hotkeys.empty())
        {
            const auto& hp = c.hotkeys[0];
            check(hp.fovX == 106 && hp.fovY == 74, "缺键 -> fovX/fovY 取默认 106/74");
            check(!hp.crosshair_detect_enabled, "缺键 -> 准星找色默认关闭");
            check(!hp.dynamic_fov_enabled, "缺键 -> 动态 FOV 默认关闭");
            check(hp.dynamic_fov_strength > 0.59f && hp.dynamic_fov_strength < 0.61f,
                  "缺键 -> dynamic_fov_strength 默认 0.60");
            check(hp.aim_classes.empty(), "缺键 -> aim_classes 为空");
        }
    }

    std::printf("\n[3] 旧三槽 -> aim_classes 的迁移(唯一还活着的迁移)\n");
    {
        const std::string p = write_config("legacy_slots.ini",
            "target_class_1 = 2\n"
            "target_y_top_1 = 0.0\ntarget_y_bot_1 = 0.25\n"
            "target_min_conf_1 = 0.30\n");

        Config c;
        check(c.loadConfig(p), "旧三槽配置能加载");
        if (!c.hotkeys.empty())
        {
            const auto& hp = c.hotkeys[0];
            check(hp.aim_classes.size() == 1, "旧三槽被搬成一条 aim_classes");
            if (hp.aim_classes.size() == 1)
            {
                const auto& ac = hp.aim_classes[0];
                check(ac.class_id == 2, "旧三槽: class_id 原样搬过来");
                check(ac.y_offset > 0.74f && ac.y_offset < 0.76f,
                      "★ 旧三槽: y_offset = 1 - y_bot = 0.75(比例语义被反转)");
                check(ac.y_offset_max > 0.99f && ac.y_offset_max <= 1.0f,
                      "★ 旧三槽: y_offset_max = 1 - y_top = 1.00");
                check(ac.min_conf > 0.29f && ac.min_conf < 0.31f,
                      "旧三槽: min_conf 原样搬过来");
            }
        }
    }

    std::printf("\n[4] aim_classes 的解析与夹取\n");
    {
        const std::string p = write_config("clamp.ini",
            "aim_classes = 1:2.0:-1.0:5.0;7:0.500:0.500:0.0\n");
        Config c;
        c.loadConfig(p);
        if (!c.hotkeys.empty())
        {
            const auto& hp = c.hotkeys[0];
            check(hp.aim_classes.size() == 2, "两条 aim_classes 都被解析");
            if (hp.aim_classes.size() == 2)
            {
                check(hp.aim_classes[0].y_offset >= 0.0f &&
                      hp.aim_classes[0].y_offset <= 1.0f,
                      "越界 y_offset 被夹到 [0,1]");
                check(hp.aim_classes[0].y_offset_max >= 0.0f &&
                      hp.aim_classes[0].y_offset_max <= 1.0f,
                      "越界 y_offset_max 被夹到 [0,1]");
                check(hp.aim_classes[0].min_conf >= 0.0f &&
                      hp.aim_classes[0].min_conf <= 1.0f,
                      "越界 min_conf 被夹到 [0,1]");
                check(hp.aim_classes[0].y_offset <= hp.aim_classes[0].y_offset_max,
                      "★ 夹取后 y_offset <= y_offset_max(顺序被修正)");
                check(hp.aim_classes[1].y_offset == hp.aim_classes[1].y_offset_max,
                      "两端相等 -> 固定锁点(允许)");
            }
        }
    }

    std::printf("\n[5] ★★ 已删除的瞄准控制键: 不报错、不污染活着的键\n");
    {
        const std::string p = write_config("removed_keys.ini",
            "fovX = 111\n"
            "some_typo_key = 1\n"
            "crosshair_detect_enabled = true\n"
            "pidf_mapping_version = 7\n"
            "pidf_kp_x = 35\npidf_kp_y = 35\n"
            "pidf_ki_x = 1.0\npidf_ki_y = 1.0\n"
            "pidf_kd_x = 0.0\npidf_kd_y = 0.0\n"
            "pidf_psat_x = 5\npidf_psat_y = 5\n"
            "pidf_deadzone_x = 3\npidf_deadzone_y = 3\n"
            "pidf_limit_x = 200\npidf_limit_y = 200\n"
            "pidf_predict_x = 0.1\npidf_predict_y = 0.1\n"
            "pidf_predict_min_w = 20\npidf_predict_max_w = 80\n"
            "pidf_predict_damp = 0.25\n"
            "pidf_predict_max_px = 12\npidf_predict_vel_floor = 60\n"
            "pidf_inflight_x = 1.6\npidf_inflight_y = 1.6\n"
            "pidf_inflight_window_ms = 46\n"
            "pidf_kf_x = 1.0\npidf_lr_x = 0.08\n"
            "aim_px_per_count_x = 0.5\naim_px_per_count_y = 0.5\n"
            "esync_min_hits = 7\nesync_max_age = 9\n"
            "esync_assoc_iou = 0.5\nesync_vel_sample_ms = 30\n"
            "esync_pred_factor_x = 0.2\nesync_pred_factor_y = 0.2\n"
            "esync_pred_min_w = 25\nesync_pred_max_w = 90\n"
            "esync_assoc_radius_px = 123\nesync_vel_window_ms = 456\n"
            "esync_counts_per_pixel_x = 0.5\nesync_counts_per_pixel_y = 0.6\n"
            "esync_inflight_window_ms = 30\nesync_inflight_beta = 2.5\n"
            "esync_self_motion_gain = 0.8\n"
            "aim_scale_enabled = 1\naim_scale_max = 1.5\n"
            "aim_scale_min = 0.7\naim_scale_base_h = 123.5\n"
            "aim_scale_near_h = 160\naim_scale_far_h = 45\n"
            "aim_path_mode = 3\naim_path_influence = 40\n"
            "aim_path_bezier_cx1 = 0.3\naim_path_bezier_cy1 = 0.0\n"
            "aim_path_bezier_cx2 = 0.7\naim_path_bezier_cy2 = 0.0\n"
            "aim_path_wind_gravity = 5\naim_path_wind_wind = 2\n"
            "aim_path_wind_step = 10\naim_path_wind_distance = 8\n"
            "aim_path_wind_threshold = 10\n"
            "aim_path_custom_samples = 0.0,0.5,0.0\n"
            "aim_path_custom_file = whatever.curve\n"
            "aim_path_neural_enabled = true\n"
            "aim_path_neural_weights = 1,2,3\n"
            "trigger_enabled = true\ntrigger_fire_delay = 50\n"
            "trigger_fire_duration = 100\ntrigger_fire_interval = 200\n"
            "trigger_y_percent = 100\n"
            "trigger_delay_jitter_ms = 5\ntrigger_duration_jitter_ms = 5\n"
            "trigger_interval_jitter_ms = 5\ntrigger_switch_cooldown_ms = 10\n"
            "trigger_auto_scope = 2\ntrigger_scope_delay_ms = 150\n"
            "trigger_auto_stop = 1\ntrigger_stop_ms = 60\n"
            "aim_mode = 0\n"
            "use_prediction_tick = true\nprediction_tick_hz = 240\n"
            "prediction_tick_max_run = 2\n"
            "anchor_filter_ms = 8\n"
            "dynamic_fov_enabled = true\ndynamic_fov_strength = 0.35\n"
            "aim_classes = 4:0.200:0.800:0.400\n");

        Config c;
        check(c.loadConfig(p),
              "★★ 满篇已删除的键 -> 仍然加载成功(不报错)");
        check(!c.hotkeys.empty(), "满篇已删除的键 -> 仍解析出热键组");
        if (!c.hotkeys.empty())
        {
            const auto& hp = c.hotkeys[0];
            check(hp.fovX == 111 && hp.fovY == 74,
                  "★★ 活着的 fovX 读到 111; 未写的 fovY 仍是默认 74");
            check(hp.crosshair_detect_enabled,
                  "★★ 活着的 crosshair_detect_enabled 读到 true");
            check(hp.dynamic_fov_enabled, "★★ 活着的 dynamic_fov_enabled 读到 true");
            check(hp.dynamic_fov_strength > 0.34f && hp.dynamic_fov_strength < 0.36f,
                  "★★ 活着的 dynamic_fov_strength 读到 0.35");
            check(hp.aim_classes.size() == 1 && hp.aim_classes[0].class_id == 4,
                  "★★ 活着的 aim_classes 读到 class_id = 4");
            check(hp.name == "Aim" && hp.keys.size() == 1 &&
                  hp.keys[0] == "RightMouseButton",
                  "★★ 活着的 name / keys 未受影响");
            check(hp.aim_path_mode == 3, "★ 恢复的 aim_path_mode 读到 3");
            check(hp.aim_path_influence == 40, "★ 恢复的 aim_path_influence 读到 40");
            check(hp.trigger_enabled, "★ 恢复的 trigger_enabled 读到 true");
            check(hp.trigger_fire_delay == 50, "★ 恢复的 trigger_fire_delay 读到 50");
            check(hp.trigger_auto_scope == 2, "★ 恢复的 trigger_auto_scope 读到 2");
        }
    }

    std::printf("\n[6] 已删除的键不会被写回\n");
    {
        const std::string p = write_config("roundtrip.ini",
            "pidf_mapping_version = 7\npidf_kp_x = 35\n"
            "esync_min_hits = 3\naim_scale_base_h = 100\n"
            "trigger_enabled = true\naim_path_mode = 3\n"
            "crosshair_detect_enabled = true\n");

        Config c;
        check(c.loadConfig(p), "往返用配置能加载");

        const std::string out = "roundtrip_out.ini";
        check(c.saveConfig(out), "保存成功");

        std::ifstream f(out, std::ios::binary);
        std::string body((std::istreambuf_iterator<char>(f)),
                         std::istreambuf_iterator<char>());
        f.close();

        check(!body.empty(), "写出的文件非空");
        for (const char* gone : {"pidf_mapping_version", "pidf_kp_x",
                                 "esync_min_hits", "aim_scale_base_h"})
        {
            check(body.find(gone) == std::string::npos,
                  std::string("★ 已删除的键不再写出: ") + gone);
        }
        for (const char* back : {"trigger_enabled", "aim_path_mode"})
        {
            check(body.find(back) != std::string::npos,
                  std::string("★ 恢复的键重新写出: ") + back);
        }
        for (const char* kept : {"fovX", "fovY", "crosshair_detect_enabled",
                                 "aim_classes", "dynamic_fov_enabled"})
        {
            check(body.find(kept) != std::string::npos,
                  std::string("活着的键仍然写出: ") + kept);
        }

        {
            const std::string p2 = write_config("restored.ini",
                "trigger_enabled = true\ntrigger_fire_delay = 45\n"
                "trigger_fire_interval = 133\ntrigger_y_percent = 150\n"
                "trigger_auto_scope = 1\ntrigger_auto_stop = 1\ntrigger_stop_ms = 77\n"
                "aim_path_mode = 3\naim_path_influence = 63\n"
                "aim_path_wind_gravity = 7.5\naim_path_wind_wind = 3.25\n"
                "aim_path_wind_threshold = 12\n");
            Config c2;
            check(c2.loadConfig(p2), "恢复的键: 配置能加载");
            if (!c2.hotkeys.empty())
            {
                const auto& e = c2.hotkeys[0];
                check(e.trigger_enabled, "trigger_enabled 读到 true");
                check(e.trigger_fire_delay == 45, "trigger_fire_delay 读到 45");
                check(e.trigger_fire_interval == 133, "trigger_fire_interval 读到 133");
                check(e.trigger_y_percent == 150, "trigger_y_percent 读到 150");
                check(e.trigger_auto_scope == 1, "trigger_auto_scope 读到 1");
                check(e.trigger_auto_stop == 1, "trigger_auto_stop 读到 1");
                check(e.trigger_stop_ms == 77, "trigger_stop_ms 读到 77");
                check(e.aim_path_mode == 3, "aim_path_mode 读到 3");
                check(e.aim_path_influence == 63, "aim_path_influence 读到 63");
                check(e.aim_path_wind_gravity > 7.4f && e.aim_path_wind_gravity < 7.6f,
                      "aim_path_wind_gravity 读到 7.5");
                check(e.aim_path_wind_wind > 3.2f && e.aim_path_wind_wind < 3.3f,
                      "aim_path_wind_wind 读到 3.25");
                check(e.aim_path_wind_threshold == 12, "aim_path_wind_threshold 读到 12");
            }
        }
    }

    // 双硬件(方案 A): 一台鼠标 + 一台键盘。
    //
    // 这里守住两件事:
    //   1) 两个新键真的能存下来、能读回去(否则用户填完串口重启就丢了);
    //   2) 旧配置里没有这两个键时, 键盘端口必须默认成空 —— 空表示"没有第二台",
    //      键盘动作回落到第一台。这正是老用户升级后的行为, 不能变成"打不开某个
    //      乱七八糟的默认串口"。
    std::printf("\n[双硬件] MAKCUNEW 键盘那台的端口/波特率往返\n");
    {
        const std::string p = write_global_config("dual.ini",
            "input_method = MAKCUNEW\n"
            "makcu_new_port = COM7\n"
            "makcu_new_baudrate = 6000000\n"
            "makcu_new_port_kbd = COM9\n"
            "makcu_new_baudrate_kbd = 4000000\n");

        Config c;
        check(c.loadConfig(p), "双硬件配置能加载");
        check(c.makcu_new_port == "COM7", "鼠标那台端口读到 COM7");
        check(c.makcu_new_port_kbd == "COM9", "键盘那台端口读到 COM9");
        check(c.makcu_new_baudrate == 6000000, "鼠标那台波特率读到 6000000");
        check(c.makcu_new_baudrate_kbd == 4000000, "键盘那台波特率读到 4000000");

        // 存档 -> 重新读, 两个新键必须原样回来
        const std::string p2 = "dual_roundtrip.ini";
        check(c.saveConfig(p2), "双硬件配置能存档");
        Config c2;
        check(c2.loadConfig(p2), "存档能重新加载");
        check(c2.makcu_new_port_kbd == "COM9", "往返后键盘端口仍是 COM9");
        check(c2.makcu_new_baudrate_kbd == 4000000, "往返后键盘波特率仍是 4000000");
    }

    std::printf("\n[双硬件] 旧配置(无键盘键) -> 键盘端口默认为空, 不误连\n");
    {
        const std::string p = write_global_config("legacy.ini",
            "input_method = MAKCUNEW\n"
            "makcu_new_port = COM7\n"
            "makcu_new_baudrate = 6000000\n");

        Config c;
        check(c.loadConfig(p), "旧配置能加载");
        check(c.makcu_new_port == "COM7", "旧配置的鼠标端口照常读到");
        check(c.makcu_new_port_kbd.empty(),
              "旧配置缺 makcu_new_port_kbd -> 默认空(键盘回落到第一台)");
        check(c.makcu_new_baudrate_kbd == 6000000,
              "旧配置缺键盘波特率 -> 默认 6000000");
    }

    // 开镜档: 自动开镜生效期间取代「瞄准控制器」的那一整组参数。
    //
    // 这里守住三件事:
    //   1) 老配置(没有这些键)读进来 = 开关关、值与默认档的默认值一致
    //      —— 打开开关也不会突然变成另一套参数;
    //   2) 写了这些键 ⇒ 原样读回, 往返不丢;
    //   3) 两档共用同一份夹取规则 (默认档现在借道 AimCtlParams::clamp())。
    std::printf("\n[7] ★★ 开镜档 (自动开镜期间取代瞄准控制器参数)\n");
    {
        // 1) 缺键 ⇒ 开关关 + 默认值
        const std::string p0 = write_config("scope_missing.ini");
        Config c0;
        check(c0.loadConfig(p0), "缺开镜档键的配置能加载");
        if (!c0.hotkeys.empty())
        {
            const auto& hp = c0.hotkeys[0];
            check(hp.scope_ctl_enabled == 0, "★★ 缺键 ⇒ 开镜档开关默认 0 (跟随热键)");
            check(hp.ctl_scope.kp_x > 34.9 && hp.ctl_scope.kp_x < 35.1,
                  "★★ 缺键 ⇒ 开镜档 kp_x 默认 35 (与默认档一致)");
            check(hp.ctl_scope.kp_x == hp.ctl_kp_x,
                  "★★ 缺键 ⇒ 开镜档与默认档逐位一致 (打开开关也不会突变)");
            check(hp.ctl_scope.max_output_counts == hp.ctl_max_output_counts,
                  "★★ 缺键 ⇒ 单拍限幅两档一致");
            check(hp.ctl_scope.random_seed == hp.ctl_random_seed,
                  "★★ 缺键 ⇒ 随机种子两档一致");
            check(hp.ctl_scope.predict_lead_ms == hp.ctl_predict_lead_ms,
                  "★★ 缺键 ⇒ 预测提前时间两档一致");
        }

        // 2) 写了就原样读回, 且与默认档互不干扰
        const std::string p1 = write_config("scope.ini",
            "ctl_kp_x = 40.0\n"
            "ctl_max_output_counts = 180\n"
            "ctl_scope_enabled = 1\n"
            "ctl_scope_kp_x = 6.5\n"
            "ctl_scope_kp_y = 7.5\n"
            "ctl_scope_k_px_per_count = 0.211\n"
            "ctl_scope_predict_lead_ms = 15\n"
            "ctl_scope_predict_max_velocity_px_s = 800\n"
            "ctl_scope_predict_max_lead_ratio = 0.25\n"
            "ctl_scope_max_output_counts = 45\n"
            "ctl_scope_random_seed = 99\n"
            "ctl_scope_inflight_dead_time_ms = 33\n"
            "ctl_scope_tau_unwind_sec = 0.021\n");

        Config c1;
        check(c1.loadConfig(p1), "开镜档配置能加载");
        if (!c1.hotkeys.empty())
        {
            const auto& hp = c1.hotkeys[0];
            check(hp.scope_ctl_enabled == 1, "★★ ctl_scope_enabled 读到 1");
            check(hp.ctl_scope.kp_x > 6.49 && hp.ctl_scope.kp_x < 6.51,
                  "★★ ctl_scope_kp_x 读到 6.5");
            check(hp.ctl_scope.kp_y > 7.49 && hp.ctl_scope.kp_y < 7.51,
                  "★★ ctl_scope_kp_y 读到 7.5");
            check(hp.ctl_scope.k_px_per_count > 0.210 && hp.ctl_scope.k_px_per_count < 0.212,
                  "★★ ctl_scope_k_px_per_count 读到 0.211");
            check(hp.ctl_scope.predict_lead_ms > 14.9 && hp.ctl_scope.predict_lead_ms < 15.1,
                  "★★ ctl_scope_predict_lead_ms 读到 15");
            check(hp.ctl_scope.predict_max_velocity_px_s > 799.9 &&
                  hp.ctl_scope.predict_max_velocity_px_s < 800.1,
                  "★★ ctl_scope_predict_max_velocity_px_s 读到 800");
            check(hp.ctl_scope.predict_max_lead_ratio > 0.249 &&
                  hp.ctl_scope.predict_max_lead_ratio < 0.251,
                  "★★ ctl_scope_predict_max_lead_ratio 读到 0.25");
            check(hp.ctl_scope.max_output_counts == 45, "★★ ctl_scope_max_output_counts 读到 45");
            check(hp.ctl_scope.random_seed == 99, "★★ ctl_scope_random_seed 读到 99");
            check(hp.ctl_scope.inflight_dead_time_ms > 32.9 &&
                  hp.ctl_scope.inflight_dead_time_ms < 33.1,
                  "★★ ctl_scope_inflight_dead_time_ms 读到 33");
            check(hp.ctl_scope.tau_unwind_sec > 0.0209 && hp.ctl_scope.tau_unwind_sec < 0.0211,
                  "★★ ctl_scope_tau_unwind_sec 读到 0.021");

            // ★ 两档必须互不干扰: 改开镜档不能动到默认档
            check(hp.ctl_kp_x > 39.9 && hp.ctl_kp_x < 40.1,
                  "★★ 开镜档的键没有污染默认档 (ctl_kp_x 仍是 40)");
            check(hp.ctl_max_output_counts == 180,
                  "★★ 开镜档的键没有污染默认档 (限幅仍是 180)");
        }

        // 3) 往返: 存 → 读, 开镜档必须原样回来
        const std::string p2 = "scope_roundtrip.ini";
        check(c1.saveConfig(p2), "开镜档配置能存档");
        Config c2;
        check(c2.loadConfig(p2), "存档能重新加载");
        if (!c2.hotkeys.empty())
        {
            const auto& hp = c2.hotkeys[0];
            check(hp.scope_ctl_enabled == 1, "往返: 开关仍是 1");
            check(hp.ctl_scope.kp_x > 6.49 && hp.ctl_scope.kp_x < 6.51,
                  "往返: 开镜档 kp_x 仍是 6.5");
            check(hp.ctl_scope.max_output_counts == 45, "往返: 开镜档限幅仍是 45");
            check(hp.ctl_scope.random_seed == 99, "往返: 开镜档种子仍是 99");
            check(hp.ctl_kp_x > 39.9 && hp.ctl_kp_x < 40.1, "往返: 默认档 kp_x 仍是 40");
        }

        // 4) 两档共用同一份夹取规则
        const std::string p3 = write_config("scope_clamp.ini",
            "ctl_kp_x = -5\n"
            "ctl_max_output_counts = 0\n"
            "ctl_tau_unwind_sec = 0\n"
            "ctl_random_seed = -7\n"
            "ctl_scope_enabled = 9\n"
            "ctl_scope_kp_x = -5\n"
            "ctl_scope_max_output_counts = 0\n"
            "ctl_scope_tau_unwind_sec = 0\n"
            "ctl_scope_random_seed = -7\n"
            "ctl_scope_inflight_beta = 9\n"
            "ctl_scope_predict_lead_ms = 99999\n");

        Config c3;
        check(c3.loadConfig(p3), "越界开镜档配置能加载");
        if (!c3.hotkeys.empty())
        {
            const auto& hp = c3.hotkeys[0];
            check(hp.scope_ctl_enabled == 1, "★★ 越界的开关 9 被夹成 1");
            check(hp.ctl_scope.kp_x >= 0.0, "★★ 开镜档负 kp 被夹到 >= 0");
            check(hp.ctl_scope.max_output_counts == 1, "★★ 开镜档限幅 0 被夹到 1");
            check(hp.ctl_scope.tau_unwind_sec > 0.0, "★★ 开镜档 tau_unwind 0 被夹到 1e-4");
            check(hp.ctl_scope.random_seed == 0, "★★ 开镜档负种子被夹到 0");
            check(hp.ctl_scope.inflight_beta <= 3.0, "★★ 开镜档 β 9 被夹到 3.0");
            check(hp.ctl_scope.predict_lead_ms <= 1000.0, "★★ 开镜档提前时间 99999 被夹到 1000");
            // 默认档在同一份规则下被夹 (改动前是手写的一组 clamp)
            check(hp.ctl_kp_x >= 0.0, "★★ 默认档负 kp 同样被夹到 >= 0");
            check(hp.ctl_max_output_counts == 1, "★★ 默认档限幅 0 同样被夹到 1");
            check(hp.ctl_tau_unwind_sec > 0.0, "★★ 默认档 tau_unwind 0 同样被夹到 1e-4");
            check(hp.ctl_random_seed == 0, "★★ 默认档负种子同样被夹到 0");
        }

        // 5) 组外的参数不随开镜档走 (只有一份, 不受开关影响)
        const std::string p4 = write_config("scope_off.ini",
            "ctl_y_offset = 0.31\n"
            "ctl_hysteresis_ratio = 2.4\n"
            "ctl_scope_enabled = 1\n"
            "ctl_scope_kp_x = 6.5\n");
        Config c4;
        check(c4.loadConfig(p4), "组外参数配置能加载");
        if (!c4.hotkeys.empty())
        {
            const auto& hp = c4.hotkeys[0];
            check(hp.ctl_y_offset > 0.30 && hp.ctl_y_offset < 0.32,
                  "★ 瞄点 Y 只有一份, 不随开镜档走");
            check(hp.ctl_hysteresis_ratio > 2.39 && hp.ctl_hysteresis_ratio < 2.41,
                  "★ 选靶滞回只有一份, 不随开镜档走");
        }
    }

    std::printf("\n=== %d 项失败 ===\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
