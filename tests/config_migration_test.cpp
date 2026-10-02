#include "config/config.h"
#include "mouse/neural_curve.h"

#include <cstdio>
#include <cmath>
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
    {
        Config c;
        c.loadConfig(write_config("curve_full_seed.ini", "aim_path_mode = 2\n"));
        std::vector<float> samples(32768);
        for (size_t i=0; i<samples.size(); ++i)
            samples[i]=float(.37 * std::sin(double(i)*.001));
        c.hotkeys[0].aim_path_custom_samples=std::make_shared<const std::vector<float>>(samples);
        check(c.saveConfig("curve_full_roundtrip.ini"), "full hand-drawn curve saves");
        Config restored;
        check(restored.loadConfig("curve_full_roundtrip.ini"), "full curve file loads");
        const auto actual=restored.hotkeys[0].aim_path_custom_samples;
        bool same=actual && actual->size()==samples.size();
        if(same) for(size_t i=0;i<samples.size();++i)
            same &= std::abs((*actual)[i]-samples[i]) <= .000051f;
        check(same, "all 32768 hand-drawn samples survive save/restart");

        // Recover the old writer's indented lines while still present on disk.
        write_config("curve_old_wrapped.ini",
            "aim_path_custom_samples = 0,1000,\n    2000,-3000,\n    4000,0\n"
            "aim_path_neural_trained = false\n");
        Config legacy;
        legacy.loadConfig("curve_old_wrapped.ini");
        const auto old=legacy.hotkeys[0].aim_path_custom_samples;
        check(old && old->size()==6 && std::abs((*old)[3]+.3f)<1e-6,
              "legacy wrapped curve continuation is recovered");
    }

    {
        Config migrated;
        check(migrated.loadConfig(write_config("am_trigger_migrate.ini",
            "trigger_fire_duration = 0\nsecondary_trigger_custom = true\n"
            "secondary_trigger_fire_duration = 25\n")), "load legacy AM mode migration");
        check(!migrated.hotkeys.empty() && migrated.hotkeys[0].trigger_fire_mode == 3 &&
            migrated.hotkeys[0].secondary_trigger.trigger_fire_mode == 0,
            "legacy hold becomes smart continuous; timed press becomes smart click");
        if (!migrated.hotkeys.empty()) {
            migrated.hotkeys[0].trigger_fire_mode = 1;
            migrated.hotkeys[0].secondary_trigger.trigger_fire_mode = 2;
            migrated.hotkeys[0].trigger_fire_interval = 0;
            check(migrated.saveConfig("am_trigger_roundtrip.ini"), "save AM modes");
            Config loaded;
            check(loaded.loadConfig("am_trigger_roundtrip.ini") && !loaded.hotkeys.empty() &&
                loaded.hotkeys[0].trigger_fire_mode == 1 &&
                loaded.hotkeys[0].secondary_trigger.trigger_fire_mode == 2 &&
                loaded.hotkeys[0].trigger_fire_interval == 0,
                "all AM modes and zero release delay survive save/load");
        }
    }

    std::printf("=== config_migration_test: 配置解析 ===\n");
    {
        Config c;
        const auto path=write_global_config("ndi_config.ini",
            "capture_source = ndi\ncapture_ndi_source = 游戏电脑 (Apotheosis)\n");
        check(c.loadConfig(path) && c.capture_source=="ndi" &&
            c.capture_ndi_source=="游戏电脑 (Apotheosis)", "NDI source loads including UTF-8 and spaces");
        check(c.saveConfig("ndi_config_roundtrip.ini"), "NDI config saves");
        Config restored;
        check(restored.loadConfig("ndi_config_roundtrip.ini") && restored.capture_source=="ndi" &&
            restored.capture_ndi_source==c.capture_ndi_source, "NDI source survives roundtrip");
    }

    {
        Config c;
        const auto path = write_config("color_lab_exact_hsv.ini",
            "[crosshair_color.0]\nname = Lab\nexact_hsv = true\nh_low = 50\nh_high = 70\n"
            "s_min = 190\ns_max = 255\nv_min = 100\nv_max = 240\n");
        check(c.loadConfig(path) && !c.crosshair_colors.empty() &&
              c.crosshair_colors[0].exact_hsv &&
              c.crosshair_colors[0].s_min == 190,
              "lab profile exact HSV setting loads");
        check(c.saveConfig("color_lab_exact_hsv_roundtrip.ini"),
              "lab profile exact HSV setting saves");
        Config restored;
        check(restored.loadConfig("color_lab_exact_hsv_roundtrip.ini") &&
              !restored.crosshair_colors.empty() &&
              restored.crosshair_colors[0].exact_hsv,
              "lab profile exact HSV survives save/load");
    }


    {
        const auto path = write_config("whole_profile_activation.ini",
            "activation_key = Key2\nactivation_selected = true\n"
            "recovered_pid_kp_x = 0.8\nrecovered_secondary_pid_kp_x = 1.5\n");
        Config c;
        check(c.loadConfig(path) && c.hotkeys.size() == 1 &&
              c.hotkeys[0].activation_key == "Key2" && c.hotkeys[0].activation_selected,
              "new activation profile loads without expanding legacy secondary data");
        check(c.saveConfig("whole_profile_activation_roundtrip.ini"), "activation profile saves");
        Config restored;
        check(restored.loadConfig("whole_profile_activation_roundtrip.ini") && restored.hotkeys.size() == 1 &&
              restored.hotkeys[0].activation_key == "Key2" && restored.hotkeys[0].activation_selected,
              "activation key and choice survive a save/load roundtrip");
        const auto old = write_config("whole_profile_legacy.ini",
            "primary_select_key = Key1\nsecondary_select_key = Key2\n"
            "recovered_pid_kp_x = 0.8\nrecovered_secondary_pid_kp_x = 1.5\n"
            "aim_classes = 3:0.5:0.5:0.5:0.5:0.5\n");
        check(c.loadConfig(old) && c.hotkeys.size() == 2 &&
              c.hotkeys[0].activation_key == "Key1" && c.hotkeys[0].activation_selected &&
              c.hotkeys[1].activation_key == "Key2" && !c.hotkeys[1].activation_selected &&
              c.hotkeys[1].recovered_pid.kpX == 1.5f &&
              c.hotkeys[1].keys == c.hotkeys[0].keys && c.hotkeys[1].group == c.hotkeys[0].group,
              "legacy secondary parameters become an independent profile with original binding");
        check(c.saveConfig("whole_profile_legacy_roundtrip.ini"), "migrated profiles save");
        check(restored.loadConfig("whole_profile_legacy_roundtrip.ini") && restored.hotkeys.size() == 2,
              "legacy migration is not repeated on subsequent loads");
    }
    {
        const std::string p = write_config("direct_parameter_keys.ini",
            "primary_select_key = Key1\n"
            "secondary_select_key = Key2\n"
            "secondary_toggle_key = F8\n");
        Config c;
        check(c.loadConfig(p) && !c.hotkeys.empty() &&
              c.hotkeys.front().primary_select_key == "Key1" &&
              c.hotkeys.front().secondary_select_key == "Key2",
              "explicit parameter selectors take precedence over legacy toggle binding");
        check(c.saveConfig("direct_parameter_keys_roundtrip.ini"), "direct parameter selectors save");
        Config restored;
        check(restored.loadConfig("direct_parameter_keys_roundtrip.ini") &&
              !restored.hotkeys.empty() &&
              restored.hotkeys.front().primary_select_key == "Key1" &&
              restored.hotkeys.front().secondary_select_key == "Key2",
              "both direct parameter selectors survive roundtrip");
        std::ifstream saved("direct_parameter_keys_roundtrip.ini");
        const std::string contents((std::istreambuf_iterator<char>(saved)), {});
        check(contents.find("secondary_toggle_key") == std::string::npos,
              "legacy toggle key is not written again");
    }

    {
        const std::string p = write_config("secondary_aim.ini",
            "secondary_toggle_key = X1MouseButton\n"
            "recovered_pid_kp_x = 0.7\n"
            "recovered_pid_follow_x = 2.5\n"
            "recovered_pid_follow_y = 1.5\n"
            "recovered_secondary_pid_kp_x = 1.2\n"
            "recovered_secondary_pid_follow_x = 6.5\n"
            "recovered_scope_pid_follow_x = 3.5\n"
            "recovered_secondary_pid_ff_y = 0.125\n"
            "recovered_secondary_pid_segment_enabled = true\n");
        Config c;
        check(c.loadConfig(p) && !c.hotkeys.empty(), "lead profiles load");
        if (!c.hotkeys.empty())
        {
            const auto& h = c.hotkeys.front();
            check(h.recovered_pid.kpX == 0.7f && h.recovered_secondary_pid.kpX == 1.2f,
                  "existing PID values remain independent");
            check(h.recovered_pid.followX == 2.5 && h.recovered_pid.followY == 1.5 &&
                  h.recovered_secondary_pid.followX == 6.5 &&
                  h.recovered_scope_pid.followX == 3.5,
                  "three profiles have independent following compensation");
        }
        check(c.saveConfig("secondary_aim_roundtrip.ini"), "lead profiles save");
        Config restored;
        check(restored.loadConfig("secondary_aim_roundtrip.ini"), "lead profiles reload");
        if (!restored.hotkeys.empty())
        {
            const auto& h = restored.hotkeys.front();
            check(h.primary_select_key == "Key1" && h.secondary_select_key == "X1MouseButton" &&
                  h.recovered_secondary_pid.feedforwardY == 0.125f &&
                  h.recovered_secondary_pid.segmentEnabled &&
                  h.recovered_pid.followX == 2.5 && h.recovered_pid.followY == 1.5 &&
                  h.recovered_secondary_pid.followX == 6.5 &&
                  h.recovered_scope_pid.followX == 3.5,
                  "lead, FF and original settings survive roundtrip");
        }
    }
    {
        const std::string p = write_config("lead_legacy.ini",
            "recovered_pid_engine = 2\n"
            "recovered_pid_lead_time_ms = 50\n"
            "recovered_pid_lead_frames = 5\n"
            "recovered_secondary_pid_lead_time_ms = 100\n"
            "recovered_scope_pid_lead_time_ms = 20\n"
            "recovered_predicted_video_delay_ms = 80\n"
            "recovered_predicted_follow_gain = 1.25\n"
            "recovered_predicted_pixels_per_count = 0.8\n"
            "recovered_ava_x_kp = 3\n"
            "recovered_pid_high_speed_lead_enabled = true\n"
            "recovered_pid_kp_x = 0.75\n"
            "recovered_pid_ff_x = 0.12\n");
        Config c;
        check(c.loadConfig(p) && !c.hotkeys.empty(), "legacy experimental config loads");
        if (!c.hotkeys.empty())
        {
            const auto& h = c.hotkeys.front();
            check(h.recovered_pid.followX == 0 &&
                  h.recovered_secondary_pid.followX == 0 &&
                  h.recovered_scope_pid.followX == 0 &&
                  h.recovered_pid.kpX == 0.75f && h.recovered_pid.feedforwardX == 0.12f,
                  "old experimental values do not silently enable anticipation");
        }
        check(c.saveConfig("lead_legacy_roundtrip.ini"), "legacy config saves cleanly");
        std::ifstream saved("lead_legacy_roundtrip.ini");
        const std::string contents((std::istreambuf_iterator<char>(saved)), {});
        check(contents.find("pid_engine") == std::string::npos &&
              contents.find("predicted_") == std::string::npos &&
              contents.find("_ava_") == std::string::npos &&
              contents.find("_pid_lead_time_ms") == std::string::npos &&
              contents.find("_pid_lead_frames") == std::string::npos,
              "removed algorithm settings are not saved again");
    }
    {
        const std::string p = write_config("lead_clamp.ini",
            "recovered_pid_follow_x = -10\n"
            "recovered_secondary_pid_follow_x = 200\n"
            "recovered_scope_pid_follow_x = nan\n");
        Config c;
        check(c.loadConfig(p) && !c.hotkeys.empty(), "invalid anticipation values load safely");
        if (!c.hotkeys.empty())
            check(c.hotkeys.front().recovered_pid.followX == 0 &&
                  c.hotkeys.front().recovered_secondary_pid.followX == 50 &&
                  c.hotkeys.front().recovered_scope_pid.followX == 0,
                  "following strength is finite and clamped to UI range");
    }

    {
        const std::string p = write_config("secondary_trigger.ini",
            "secondary_toggle_key = F8\n"
            "trigger_enabled = true\n"
            "trigger_fire_delay = 50\n"
            "secondary_trigger_custom = true\n"
            "secondary_trigger_enabled = true\n"
            "secondary_trigger_fire_delay = 140\n"
            "secondary_trigger_auto_stop = 2\n"
            "secondary_trigger_auto_scope = 3\n");
        Config c;
        check(c.loadConfig(p) && !c.hotkeys.empty(), "键盘切换键和第二套扳机配置能加载");
        if (!c.hotkeys.empty())
        {
            const auto& h = c.hotkeys.front();
            check(h.secondary_select_key == "F8" && h.secondary_trigger_custom,
                  "键盘切换键及独立扳机开关正确读取");
            check(h.trigger_fire_delay == 50 && h.secondary_trigger.trigger_fire_delay == 140 &&
                  h.secondary_trigger.trigger_auto_stop == 2 &&
                  h.secondary_trigger.trigger_auto_scope == 0,
                  "两套扳机参数独立且旧瞬狙值被清理");
        }
        check(c.saveConfig("secondary_trigger_roundtrip.ini"), "第二套扳机配置能保存");
        Config restored;
        check(restored.loadConfig("secondary_trigger_roundtrip.ini") &&
              !restored.hotkeys.empty() &&
              restored.hotkeys.front().secondary_trigger.trigger_fire_delay == 140 &&
              restored.hotkeys.front().secondary_trigger_custom,
              "第二套扳机配置往返不丢失");
    }

    {
        const std::string p = write_global_config("network_source.ini",
            "capture_source = udp\n"
            "capture_stream_url = udp://0.0.0.0:23000?fifo_size=500000\n");
        Config c;
        check(c.loadConfig(p), "网络采集配置能加载");
        check(c.capture_source == "udp" &&
              c.capture_stream_url == "udp://0.0.0.0:23000?fifo_size=500000",
              "UDP 模式及地址正确读取");
        check(c.saveConfig("network_source_roundtrip.ini"), "网络采集配置能保存");
        Config restored;
        check(restored.loadConfig("network_source_roundtrip.ini"),
              "网络采集配置能重新加载");
        check(restored.capture_source == c.capture_source &&
              restored.capture_stream_url == c.capture_stream_url,
              "网络采集配置往返后保持一致");
    }

    std::printf("\n[1] 热键字段的解析往返\n");
    {
        const std::string p = write_config("basic.ini",
            "fovX = 120\nfovY = 90\n"
            "crosshair_detect_enabled = true\n"
            "dynamic_fov_enabled = true\ndynamic_fov_strength = 0.40\n"
            "dynamic_fov_size = 32\ndynamic_fov_expand_ms = 300\n"
            "dynamic_fov_shrink_ms = 450\nmask_x = true\nmask_y = false\n"
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
            check(hp.mask_x && !hp.mask_y && hp.dynamic_fov_shrink_ms==450,
                  "axis masks and FOV contraction time load independently");
            check(hp.crosshair_detect_enabled,
                  "crosshair_detect_enabled 原样读回");
            check(hp.dynamic_fov_enabled, "dynamic_fov_enabled 原样读回");
            check(hp.dynamic_fov_size==32 && hp.dynamic_fov_expand_ms==300,
                  "explicit FOV size and expansion time replace legacy strength");
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
            check(!hp.mask_x && !hp.mask_y && hp.dynamic_fov_shrink_ms==200,
                  "legacy configs retain both axes and default FOV contraction time");
            check(hp.dynamic_fov_size==40 && hp.dynamic_fov_expand_ms==120,
                  "missing FOV size and expansion time use defaults");
            check(hp.aim_classes.empty(), "缺键 -> aim_classes 为空");
            check(!hp.trigger_weapon_switch31,
                  "旧配置缺键时开火后切枪默认关闭");
        }
    }

    {
        const std::string p = write_config("switch31.ini",
            "trigger_weapon_switch31 = true\n"
            "trigger_switch31_delay_ms = 75\n"
            "trigger_switch31_step_ms = 25\n");
        Config c;
        check(c.loadConfig(p), "31 切枪配置能加载");
        if (!c.hotkeys.empty())
        {
            const auto& hk = c.hotkeys[0];
            check(hk.trigger_weapon_switch31 &&
                  hk.trigger_switch31_delay_ms == 75 &&
                  hk.trigger_switch31_step_ms == 25,
                  "31 切枪开关、开火后等待和按键间隔正确读取");
        }
        check(c.saveConfig("switch31_roundtrip.ini"), "31 切枪配置能保存");
        Config loaded;
        check(loaded.loadConfig("switch31_roundtrip.ini"), "31 切枪配置能重新加载");
        if (!loaded.hotkeys.empty())
            check(loaded.hotkeys[0].trigger_weapon_switch31 &&
                  loaded.hotkeys[0].trigger_switch31_delay_ms == 75 &&
                  loaded.hotkeys[0].trigger_switch31_step_ms == 25,
                  "31 切枪配置保存后不丢失");
    }

    {
        std::string weights;
        for (int i = 0; i < 25; ++i)
        {
            if (i) weights += ',';
            weights += "0.1";
        }
        const std::string path = write_config("neural_legacy.ini",
            "aim_path_mode = 2\n"
            "aim_path_neural_enabled = true\n"
            "aim_path_neural_weights = " + weights + "\n");
        Config old;
        check(old.loadConfig(path), "旧版神经网络曲线配置能加载");
        if (!old.hotkeys.empty())
            check(old.hotkeys[0].aim_path_mode == 4 &&
                  old.hotkeys[0].aim_path_neural_trained,
                  "旧版 Custom 神经网络模型迁移到独立曲线模式");
        check(old.saveConfig("neural_migrated.ini"), "迁移模型能保存");
        Config restored;
        check(restored.loadConfig("neural_migrated.ini"), "迁移模型能回读");
        if (!restored.hotkeys.empty())
            check(restored.hotkeys[0].aim_path_neural_trained &&
                  restored.hotkeys[0].aim_path_mode == 4 &&
                  restored.hotkeys[0].aim_path_neural_weights[24] > 0.09f,
                  "25 个神经网络权重保存后完整回读");
    }

    {
        Config saved;
        saved.hotkeys.emplace_back();
        auto& hp = saved.hotkeys.back();
        hp.aim_path_mode = 4;
        hp.aim_path_neural_trained = true;
        hp.aim_path_neural_weights = boss::randomNeuralCurve(2026).weights;
        check(saved.saveConfig("neural_random_roundtrip.ini"), "随机曲线能保存");
        Config loaded;
        check(loaded.loadConfig("neural_random_roundtrip.ini"), "随机曲线能重新加载");
        check(!loaded.hotkeys.empty(), "随机曲线所属热键保留");
        if (!loaded.hotkeys.empty()) {
            const auto& actual = loaded.hotkeys.back();
            check(actual.aim_path_mode == 4 && actual.aim_path_neural_trained &&
                  actual.aim_path_neural_examples == 0, "随机曲线不伪装成录制训练");
            for (int i = 0; i <= 100; ++i)
                check(std::abs(boss::evaluateNeuralCurve(hp.aim_path_neural_weights, i / 100.0) -
                               boss::evaluateNeuralCurve(actual.aim_path_neural_weights, i / 100.0)) < 1e-6,
                      "随机曲线重启后形状不变");
        }
    }

    std::printf("\n[3] 旧三槽 -> aim_classes 的迁移\n");
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
            "aim_classes = 1:2.0:-1.0:5.0:1.5:-0.5;7:0.500:0.500:0.0\n");
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
                check(hp.aim_classes[0].x_offset == 0.0f &&
                      hp.aim_classes[0].x_offset_max == 1.0f,
                      "X 偏移越界及反向区间被修正");
                check(hp.aim_classes[1].x_offset == 0.5f &&
                      hp.aim_classes[1].x_offset_max == 0.5f,
                      "旧格式未提供 X 时保持框中心");
                check(hp.aim_classes[1].y_offset == hp.aim_classes[1].y_offset_max,
                      "两端相等 -> 固定锁点(允许)");
            }
        }
    }

    {
        Config saved;
        saved.hotkeys.emplace_back();
        HotkeyAimClass ac;
        ac.class_id = 4;
        ac.y_offset = 0.8f;
        ac.y_offset_max = 0.9f;
        ac.x_offset = 0.2f;
        ac.x_offset_max = 0.8f;
        saved.hotkeys[0].aim_classes = { ac };
        saved.hotkeys[0].ctl_x_offset = 0.3;
        saved.hotkeys[0].ctl_x_offset_max = 0.7;
        saved.hotkeys[0].recovered_pid.kpX = 0.73f;
        saved.hotkeys[0].recovered_pid.deadzoneY = 12.0f;
        saved.hotkeys[0].recovered_pid.segmentEnabled = true;
        saved.hotkeys[0].recovered_pid.segment = 2.0f;
        saved.hotkeys[0].recovered_scope_pid.kpX = 0.31f;
        check(saved.saveConfig("x_offset_roundtrip.ini"), "X 偏移能保存");
        Config loaded;
        check(loaded.loadConfig("x_offset_roundtrip.ini"), "X 偏移能回读");
        if (!loaded.hotkeys.empty() && !loaded.hotkeys[0].aim_classes.empty())
            check(std::abs(loaded.hotkeys[0].aim_classes[0].x_offset - 0.2f) < 1e-4f &&
                  std::abs(loaded.hotkeys[0].aim_classes[0].x_offset_max - 0.8f) < 1e-4f &&
                  std::abs(loaded.hotkeys[0].aim_classes[0].y_offset - 0.8f) < 1e-4f &&
                  std::abs(loaded.hotkeys[0].aim_classes[0].y_offset_max - 0.9f) < 1e-4f &&
                  std::abs(loaded.hotkeys[0].ctl_x_offset - 0.3) < 1e-6 &&
                  std::abs(loaded.hotkeys[0].ctl_x_offset_max - 0.7) < 1e-6,
                  "逐类与热键级 X 偏移保存后不丢失");
        if (!loaded.hotkeys.empty())
            check(std::abs(loaded.hotkeys[0].recovered_pid.kpX - 0.73f) < 1e-4f &&
                  std::abs(loaded.hotkeys[0].recovered_pid.deadzoneY - 12.0f) < 1e-4f &&
                  loaded.hotkeys[0].recovered_pid.segmentEnabled &&
                  std::abs(loaded.hotkeys[0].recovered_pid.segment - 2.0f) < 1e-4f &&
                  std::abs(loaded.hotkeys[0].recovered_scope_pid.kpX - 0.31f) < 1e-4f,
                  "移植版 PID 默认档和开镜档可保存回读");
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
            check(hp.dynamic_fov_size==40 && hp.dynamic_fov_expand_ms==120,
                  "legacy strength is not misread as a pixel diameter");
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
                check(e.trigger_stop_before_ms == 0 && e.trigger_stop_after_ms == 77,
                      "旧 trigger_stop_ms 迁移到开枪后时间");
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

    {
        const std::string p = write_config("stop_timing.ini",
            "trigger_auto_stop = 1\ntrigger_stop_ms = 99\n"
            "trigger_stop_before_ms = 125\ntrigger_stop_after_ms = 240\n");
        Config c;
        check(c.loadConfig(p), "双时间急停配置加载");
        check(!c.hotkeys.empty() && c.hotkeys[0].trigger_stop_before_ms == 125 &&
              c.hotkeys[0].trigger_stop_after_ms == 240,
              "新字段覆盖旧时长并独立读取");
        check(c.saveConfig("stop_timing_roundtrip.ini"), "双时间急停配置保存");
        Config roundtrip;
        check(roundtrip.loadConfig("stop_timing_roundtrip.ini") &&
              !roundtrip.hotkeys.empty() &&
              roundtrip.hotkeys[0].trigger_stop_before_ms == 125 &&
              roundtrip.hotkeys[0].trigger_stop_after_ms == 240,
              "双时间急停配置往返不丢失");
    }

    {
        const std::string p = write_config("stop_continuous.ini",
            "trigger_auto_stop = 2\ntrigger_auto_scope = 3\n");
        Config c;
        check(c.loadConfig(p) && !c.hotkeys.empty() &&
              c.hotkeys[0].trigger_auto_stop == 2 &&
              c.hotkeys[0].trigger_auto_scope == 0,
              "旧瞬狙模式迁移为关闭开镜，持续急停保留");
        check(c.saveConfig("stop_continuous_roundtrip.ini"),
              "持续急停模式保存");
        Config roundtrip;
        check(roundtrip.loadConfig("stop_continuous_roundtrip.ini") &&
              !roundtrip.hotkeys.empty() &&
              roundtrip.hotkeys[0].trigger_auto_stop == 2 &&
              roundtrip.hotkeys[0].trigger_auto_scope == 0,
              "迁移后的开镜关闭和持续急停往返不丢失");
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

    // Retired controller keys may remain in imported INI files. They must
    // not override the controller currently exposed in the UI.
    {
        Config c;
        const auto path = write_config("retired_controller_keys.ini",
            "ctl_kp_x = 500\nctl_predict_lead_ms = 80\n"
            "ctl_scope_kp_x = 500\nctl_scope_enabled = 1\n"
            "recovered_pid_kp_x = 0.7\nrecovered_scope_pid_kp_x = 0.3\n");
        check(c.loadConfig(path) && !c.hotkeys.empty() &&
              c.hotkeys[0].recovered_pid.kpX == 0.7f &&
              c.hotkeys[0].recovered_scope_pid.kpX == 0.3f &&
              c.hotkeys[0].scope_ctl_enabled == 1,
              "retired controller keys do not override visible controller settings");
        check(c.saveConfig("retired_controller_keys_roundtrip.ini"),
              "visible controller settings save");
        std::ifstream saved("retired_controller_keys_roundtrip.ini");
        const std::string contents((std::istreambuf_iterator<char>(saved)),
                                    std::istreambuf_iterator<char>());
        check(contents.find("ctl_predict_lead_ms") == std::string::npos &&
              contents.find("ctl_scope_kp_x") == std::string::npos,
              "retired controller keys are omitted on save");
    }

    {
        Config c;
        check(c.loadConfig(write_config("trigger_prearm.ini",
            "trigger_prearm_enabled = true\ntrigger_prearm_expand_percent = 75\n"
            "secondary_trigger_custom = true\nsecondary_trigger_prearm_enabled = true\n"
            "secondary_trigger_prearm_expand_percent = 120\n")) &&
            !c.hotkeys.empty(), "load prearm for both trigger profiles");
        if (!c.hotkeys.empty()) {
            check(c.hotkeys[0].trigger_prearm_enabled &&
                  c.hotkeys[0].trigger_prearm_expand_percent == 75 &&
                  c.hotkeys[0].secondary_trigger.trigger_prearm_enabled &&
                  c.hotkeys[0].secondary_trigger.trigger_prearm_expand_percent == 120,
                  "prearm profile values stay independent");
            c.saveConfig("trigger_prearm_roundtrip.ini");
            Config again;
            check(again.loadConfig("trigger_prearm_roundtrip.ini") &&
                  !again.hotkeys.empty() &&
                  again.hotkeys[0].trigger_prearm_expand_percent == 75 &&
                  again.hotkeys[0].secondary_trigger.trigger_prearm_expand_percent == 120,
                  "prearm survives config roundtrip");
        }
    }

    {
        Config c;
        check(c.loadConfig(write_config("trigger_loss.ini",
            "trigger_loss_delay_ms = 170\nsecondary_trigger_custom = true\n"
            "secondary_trigger_loss_delay_ms = 250\n")), "load target-loss grace");
        check(!c.hotkeys.empty(), "grace config contains a hotkey");
        if (!c.hotkeys.empty()) {
            check(c.hotkeys[0].trigger_loss_delay_ms == 170 &&
                  c.hotkeys[0].secondary_trigger.trigger_loss_delay_ms == 250,
                  "primary and legacy secondary grace are independent");
            c.saveConfig("trigger_loss_roundtrip.ini");
            Config again;
            check(again.loadConfig("trigger_loss_roundtrip.ini") && !again.hotkeys.empty() &&
                  again.hotkeys[0].trigger_loss_delay_ms == 170 &&
                  again.hotkeys[0].secondary_trigger.trigger_loss_delay_ms == 250,
                  "grace survives config roundtrip");
        }
        Config limits;
        check(limits.loadConfig(write_config("trigger_loss_limits.ini",
            "trigger_loss_delay_ms = -1\nsecondary_trigger_custom = true\n"
            "secondary_trigger_loss_delay_ms = 9999\n")) && !limits.hotkeys.empty() &&
              limits.hotkeys[0].trigger_loss_delay_ms == 0 &&
              limits.hotkeys[0].secondary_trigger.trigger_loss_delay_ms == 1000,
              "AM grace is clamped to 0..1000 ms");
        Config defaults;
        check(defaults.loadConfig(write_config("trigger_loss_default.ini", "trigger_enabled = true\n")) &&
              !defaults.hotkeys.empty() && defaults.hotkeys[0].trigger_loss_delay_ms == 20,
              "configs without grace use AM default 20 ms");
    }

    {
        Config c;
        check(c.loadConfig(write_config("aimpoint_recoil.ini",
            "aimpoint_recoil_enabled = true\n"
            "aimpoint_recoil_speed_px_s = 45.5\n"
            "aimpoint_recoil_max_px = 72\n")) && !c.hotkeys.empty() &&
            c.hotkeys[0].aimpoint_recoil_enabled &&
            c.aimpoint_recoil_speed_px_s == 45.5 &&
            c.aimpoint_recoil_max_px == 72.0 &&
            c.aimpoint_recoil_fire_key == "LeftMouseButton",
            "legacy recoil parameters migrate to global settings");
        check(c.saveConfig("aimpoint_recoil_roundtrip.ini"), "aimpoint recoil settings save");
        Config restored;
        check(restored.loadConfig("aimpoint_recoil_roundtrip.ini") &&
            !restored.hotkeys.empty() && restored.hotkeys[0].aimpoint_recoil_enabled &&
            restored.aimpoint_recoil_speed_px_s == 45.5 &&
            restored.aimpoint_recoil_max_px == 72.0 &&
            restored.aimpoint_recoil_fire_key == "LeftMouseButton",
            "aimpoint recoil settings survive save/load");
        Config global;
        check(global.loadConfig(write_global_config("aimpoint_recoil_global.ini",
            "aimpoint_recoil_speed_px_s = 60.5\n"
            "aimpoint_recoil_max_px = 100\n"
            "aimpoint_recoil_fire_key = X1MouseButton\n")) &&
            global.aimpoint_recoil_speed_px_s == 60.5 &&
            global.aimpoint_recoil_max_px == 100.0 &&
            global.aimpoint_recoil_fire_key == "X1MouseButton",
            "shared recoil parameters load without a separate hotkey selector");
        check(global.saveConfig("aimpoint_recoil_global_roundtrip.ini"),
              "recoil fire key saves");
        Config fireKeyRestored;
        check(fireKeyRestored.loadConfig("aimpoint_recoil_global_roundtrip.ini") &&
              fireKeyRestored.aimpoint_recoil_fire_key == "X1MouseButton",
              "recoil fire key survives save/load");
        Config exclusive;
        check(exclusive.loadConfig(write_config("aimpoint_recoil_exclusive.ini",
            "crosshair_detect_enabled = true\n"
            "laser_detect_enabled = true\n"
            "aimpoint_recoil_enabled = true\n")) &&
            !exclusive.hotkeys.empty() && exclusive.hotkeys[0].crosshair_detect_enabled &&
            !exclusive.hotkeys[0].laser_detect_enabled &&
            !exclusive.hotkeys[0].aimpoint_recoil_enabled,
            "older conflicting mode flags resolve to one mode");
    }

    {
        Config c;
        check(c.loadConfig(write_config("trigger_snap_modes.ini",
            "trigger_mode = 2\n"
            "trigger_snap_px_per_count = 1.75\n"
            "trigger_spin_turns = 3\n"
            "trigger_spin_counts_per_turn = 120000\n"
            "trigger_spin_step_degrees = 120\n"
            "trigger_spin_step_ms = 25\n"
            "trigger_spin_hold_ms = 750\n"
            "trigger_spin_counts_per_second = 180000\n"
            "trigger_return_y_percent = 60\n"
            "trigger_snap_fire_hold_ms = 45\n"
            "trigger_snap_cooldown_ms = 280\n"
            "trigger_flash_disappear_ms = 120\n"
            "secondary_trigger_custom = true\n"
            "secondary_trigger_mode = 1\n"
            "secondary_trigger_return_counts_per_second = 2500\n"
            "secondary_trigger_spin_step_degrees = 180\n"
            "secondary_trigger_spin_step_ms = 35\n"
            "secondary_trigger_spin_hold_ms = 450\n"
            "secondary_trigger_return_y_percent = 85\n"
            "secondary_trigger_snap_fire_hold_ms = 70\n"
            "secondary_trigger_flash_disappear_ms = 240\n")) &&
            !c.hotkeys.empty() && c.hotkeys[0].trigger_mode == 2 &&
            c.hotkeys[0].trigger_snap_px_per_count == 1.75 &&
            c.hotkeys[0].trigger_spin_turns == 3 &&
            c.hotkeys[0].trigger_spin_counts_per_turn == 120000 &&
            c.hotkeys[0].trigger_spin_step_degrees == 120 &&
            c.hotkeys[0].trigger_spin_step_ms == 25 &&
            c.hotkeys[0].trigger_spin_hold_ms == 750 &&
            c.hotkeys[0].trigger_spin_counts_per_second == 180000 &&
            c.hotkeys[0].trigger_return_y_percent == 60 &&
            c.hotkeys[0].trigger_snap_fire_hold_ms == 45 &&
            c.hotkeys[0].trigger_snap_cooldown_ms == 280 &&
            c.hotkeys[0].trigger_flash_disappear_ms == 120 &&
            c.hotkeys[0].secondary_trigger.trigger_mode == 1 &&
            c.hotkeys[0].secondary_trigger.trigger_return_counts_per_second == 2500 &&
            c.hotkeys[0].secondary_trigger.trigger_spin_step_degrees == 180 &&
            c.hotkeys[0].secondary_trigger.trigger_spin_step_ms == 35 &&
            c.hotkeys[0].secondary_trigger.trigger_spin_hold_ms == 450 &&
            c.hotkeys[0].secondary_trigger.trigger_return_y_percent == 85 &&
            c.hotkeys[0].secondary_trigger.trigger_snap_fire_hold_ms == 70 &&
            c.hotkeys[0].secondary_trigger.trigger_flash_disappear_ms == 240,
            "both trigger modes load independent settings");
        check(c.saveConfig("trigger_snap_modes_roundtrip.ini"), "snap modes save");
        Config restored;
        check(restored.loadConfig("trigger_snap_modes_roundtrip.ini") &&
            !restored.hotkeys.empty() && restored.hotkeys[0].trigger_mode == 2 &&
            restored.hotkeys[0].secondary_trigger.trigger_mode == 1 &&
            restored.hotkeys[0].trigger_snap_fire_hold_ms == 45 &&
            restored.hotkeys[0].trigger_spin_counts_per_turn == 120000 &&
            restored.hotkeys[0].trigger_spin_step_degrees == 120 &&
            restored.hotkeys[0].trigger_spin_step_ms == 25 &&
            restored.hotkeys[0].trigger_spin_hold_ms == 750 &&
            restored.hotkeys[0].trigger_spin_counts_per_second == 180000 &&
            restored.hotkeys[0].trigger_return_y_percent == 60 &&
            restored.hotkeys[0].trigger_flash_disappear_ms == 120 &&
            restored.hotkeys[0].secondary_trigger.trigger_snap_fire_hold_ms == 70 &&
            restored.hotkeys[0].secondary_trigger.trigger_return_y_percent == 85 &&
            restored.hotkeys[0].secondary_trigger.trigger_spin_step_degrees == 180 &&
            restored.hotkeys[0].secondary_trigger.trigger_spin_step_ms == 35 &&
            restored.hotkeys[0].secondary_trigger.trigger_spin_hold_ms == 450 &&
            restored.hotkeys[0].secondary_trigger.trigger_flash_disappear_ms == 240,
            "snap mode settings survive save/load");
    }

    {
        Config c;
        check(c.loadConfig(write_config("trigger_classes.ini",
            "aim_classes = 0:0.5:0.5:0.0:0.5:0.5\n"
            "trigger_classes = 2:0.250:0.800:30:45;1:0.500:0.500:100:120\n")) &&
            !c.hotkeys.empty() && c.hotkeys[0].trigger_classes.size() == 2 &&
            c.hotkeys[0].trigger_classes[0].class_id == 2 &&
            c.hotkeys[0].trigger_classes[0].range_y_percent == 45 &&
            c.hotkeys[0].aim_classes[0].class_id == 0,
            "trigger target classes have independent order, point and range");
        check(c.saveConfig("trigger_classes_roundtrip.ini"), "trigger class rules save");
        Config restored;
        check(restored.loadConfig("trigger_classes_roundtrip.ini") &&
            !restored.hotkeys.empty() && restored.hotkeys[0].trigger_classes.size() == 2 &&
            restored.hotkeys[0].trigger_classes[0].x_offset == 0.25f &&
            restored.hotkeys[0].trigger_classes[1].range_y_percent == 120,
            "trigger class rules survive save/load");

        Config wide;
        check(wide.loadConfig(write_config("trigger_range_1000.ini",
            "trigger_y_percent = 1000\n"
            "trigger_classes = 2:0.500:0.500:1000:1000\n")) &&
            !wide.hotkeys.empty() && wide.hotkeys[0].trigger_y_percent == 1000 &&
            wide.hotkeys[0].trigger_classes.size() == 1 &&
            wide.hotkeys[0].trigger_classes[0].range_x_percent == 1000 &&
            wide.hotkeys[0].trigger_classes[0].range_y_percent == 1000,
            "trigger range accepts 1000 percent");
        check(wide.saveConfig("trigger_range_1000_roundtrip.ini"),
            "1000 percent trigger range saves");
        Config wideRestored;
        check(wideRestored.loadConfig("trigger_range_1000_roundtrip.ini") &&
            !wideRestored.hotkeys.empty() &&
            wideRestored.hotkeys[0].trigger_y_percent == 1000 &&
            wideRestored.hotkeys[0].trigger_classes.size() == 1 &&
            wideRestored.hotkeys[0].trigger_classes[0].range_y_percent == 1000,
            "1000 percent trigger range survives save/load");
    }

    {
        Config c;macros::Program p;p.id="macro-roundtrip";p.name=u8"嵌套条件";p.enabled=true;p.trigger="LeftControl+U";
        p.options={{"event","ocr_found"},{"pattern",u8"中文 ; # = test"},{"priority","50"},{"mutex",u8"测试组"}};
        macros::Condition condition;condition.metric="pixels.multi";condition.text="1,2,ff00ff;3,4,000000";condition.parent=-2;condition.region={2,3,100,150};condition.value=.125;condition.upper=16;
        p.conditions={condition};macros::Action action;action.type=macros::ActionType::Log;action.text=u8"  中文 ; # = 路径 C:/测试  ";action.a=4;action.b=5;action.c=6;action.d=7;action.value=.0625;p.actions={action};
        c.macro_programs={p};check(c.saveConfig("macro_roundtrip.ini"),"macro extended config saves");
        Config restored;check(restored.loadConfig("macro_roundtrip.ini")&&restored.macro_programs==c.macro_programs,"macro conditions actions text and locks roundtrip exactly");
    }
    std::printf("\n=== %d 项失败 ===\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
