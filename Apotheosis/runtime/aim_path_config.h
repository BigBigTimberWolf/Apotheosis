#pragma once

#include "config/config.h"
#include "mouse/aim_path.h"

#include <algorithm>

namespace runtime::aim_loop {

inline boss::AimPathDriver::Params pathParamsFrom(const HotkeyProfile& hk)
{
    boss::AimPathDriver::Params p;
    if (hk.aim_path_mode == 4)
    {
        // 神经网络仍走 Custom 的安全整形链，实时只读 25 个模型权重。
        // 未训练时退回直线，不把全零权重当成「已训练曲线」。
        p.mode = hk.aim_path_neural_trained
            ? boss::AimPathDriver::Mode::Custom : boss::AimPathDriver::Mode::Linear;
        p.neural_enabled = hk.aim_path_neural_trained;
        p.neural_weights = hk.aim_path_neural_weights;
    }
    else
    {
        p.mode = static_cast<boss::AimPathDriver::Mode>(
            std::clamp(hk.aim_path_mode, 0, 3));
    }
    p.strength = std::clamp(hk.aim_path_influence, 0, 100) / 100.0;
    p.cx1 = hk.aim_path_bezier_cx1;
    p.cy1 = hk.aim_path_bezier_cy1;
    p.cx2 = hk.aim_path_bezier_cx2;
    p.cy2 = hk.aim_path_bezier_cy2;
    p.custom_samples = hk.aim_path_custom_samples;
    p.wind_gravity = hk.aim_path_wind_gravity;
    p.wind_wind = hk.aim_path_wind_wind;
    p.wind_step = hk.aim_path_wind_step;
    p.wind_distance = hk.aim_path_wind_distance;
    p.wind_threshold_px = hk.aim_path_wind_threshold;
    return p;
}

}
