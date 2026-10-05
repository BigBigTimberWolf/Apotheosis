#pragma once

#include "types.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

namespace control {

struct FfCalibrationObservation { int64_t timeUs = 0; Vec2 center{}; };
struct FfCalibrationMove { int64_t timeUs = 0; Counts counts{}; };
struct FfCalibrationResult {
    bool valid = false;
    Vec2 pixelsPerCount{};
    double delayMs = 0.0;
    Vec2 rmse{};
    // Raw detector travel (max - min of the box centre over the whole record).
    // Reported so a user can separate "the scene really did not move" from
    // "the recording moved but the fit was unstable".
    Vec2 travelPixels{};
    std::string reason = u8"样本不足，需对静止目标重新标定";
};

inline std::string ffPixelsText(double value) {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%.1f", value);
    return buffer;
}

inline std::string ffTravelText(const Vec2& travel) {
    return "X " + ffPixelsText(travel.x) + " px、Y " + ffPixelsText(travel.y) + " px";
}

// Offline fit only. A fixed target obeys image_position = intercept -
// pixels_per_count * cumulative_successful_sends(t - delay). Fitting positions
// avoids differentiating detector jitter. No backgrounds or new image work.
inline FfCalibrationResult fitFfCalibration(
    const std::vector<FfCalibrationObservation>& observations,
    const std::vector<FfCalibrationMove>& moves)
{
    FfCalibrationResult result;
    if (observations.size() < 40 || moves.size() < 8) return result;
    int64_t previous = 0;
    for (const auto& o : observations) {
        if (o.timeUs <= previous || !std::isfinite(o.center.x) || !std::isfinite(o.center.y)) {
            result.reason = u8"检测时间戳异常：采集或检测中断，请重试";
            return result;
        }
        previous = o.timeUs;
    }
    previous = 0;
    for (const auto& m : moves) {
        if (m.timeUs < previous || m.timeUs <= 0 || (m.counts.x == 0 && m.counts.y == 0)) {
            result.reason = u8"鼠标位移记录异常：请检查鼠标设备连接后重试";
            return result;
        }
        previous = m.timeUs;
    }
    if (observations.front().timeUs >= moves.front().timeUs ||
        observations.back().timeUs < moves.back().timeUs + 250000) {
        result.reason = u8"采样区间不足：请按住热键直到界面提示采样完成再松开";
        return result;
    }

    struct AxisFit { double gain = 0, rmse = 0, score = 0; bool valid = false; };
    auto fitAxis = [](const std::vector<double>& count, const std::vector<double>& position) {
        AxisFit f;
        const double n = double(count.size());
        double sx = 0, sy = 0, sxx = 0, sxy = 0, syy = 0;
        for (size_t i = 0; i < count.size(); ++i) {
            sx += count[i]; sy += position[i];
            sxx += count[i] * count[i]; sxy += count[i] * position[i];
            syy += position[i] * position[i];
        }
        const double vx = sxx - sx * sx / n, vy = syy - sy * sy / n;
        if (vx < n || vy < n * 2.0) return f;
        f.gain = -(sxy - sx * sy / n) / vx;
        const double intercept = (sy + f.gain * sx) / n;
        double residual = 0;
        for (size_t i = 0; i < count.size(); ++i) {
            const double e = position[i] - intercept + f.gain * count[i];
            residual += e * e;
        }
        f.rmse = std::sqrt(residual / n);
        f.score = residual / vy;
        const auto range = std::minmax_element(count.begin(), count.end());
        const double excursion = (*range.second - *range.first) * f.gain;
        f.valid = f.gain >= 0.02 && f.gain <= 20.0 && excursion >= 6.0 &&
                  f.score <= 0.08 && f.rmse <= std::max(0.8, excursion * 0.06);
        return f;
    };
    std::vector<double> xs(observations.size()), ys(observations.size());
    std::vector<double> px(observations.size()), py(observations.size());
    // Shift to a local origin for numerical stability.
    for (size_t i = 0; i < observations.size(); ++i) {
        px[i] = observations[i].center.x - observations.front().center.x;
        py[i] = observations[i].center.y - observations.front().center.y;
    }
    // Measured travel is reported even when the fit is rejected: it is the only
    // number that tells "the scene moved too little" from "the scene drifted".
    {
        const auto rx = std::minmax_element(px.begin(), px.end());
        const auto ry = std::minmax_element(py.begin(), py.end());
        result.travelPixels = {*rx.second - *rx.first, *ry.second - *ry.first};
    }
    double best = std::numeric_limits<double>::infinity();
    int firstBest = 0, lastBest = 0;
    for (int delay = 0; delay <= 200; ++delay) {
        size_t cursor = 0;
        Vec2 cumulative;
        for (size_t i = 0; i < observations.size(); ++i) {
            const auto cutoff = observations[i].timeUs - int64_t(delay) * 1000;
            while (cursor < moves.size() && moves[cursor].timeUs <= cutoff) {
                cumulative += Vec2{double(moves[cursor].counts.x), double(moves[cursor].counts.y)};
                ++cursor;
            }
            xs[i] = cumulative.x; ys[i] = cumulative.y;
        }
        const auto x = fitAxis(xs, px), y = fitAxis(ys, py);
        if (!x.valid || !y.valid) continue;
        const double score = x.score + y.score;
        if (score < best - 1e-10) {
            best = score; firstBest = lastBest = delay;
            result.valid = true;
            result.pixelsPerCount = {x.gain, y.gain}; result.rmse = {x.rmse, y.rmse};
        } else if (std::abs(score - best) <= 1e-10) lastBest = delay;
    }
    result.delayMs = (firstBest + lastBest) * 0.5;
    if (!result.valid) {
        // Below kMinTravelPixels the detector noise dominates the regression, so
        // the useful advice is about the scene, not about retrying unchanged.
        constexpr double kMinTravelPixels = 8.0;
        const double travel = std::min(result.travelPixels.x, result.travelPixels.y);
        result.reason = travel < kMinTravelPixels
            ? u8"移动像素过低：检测框全程只移动了 " + ffTravelText(result.travelPixels) +
              u8"。请在训练场改用更低的倍率或更高的游戏灵敏度后重试"
            : u8"数据不稳定：检测框移动 " + ffTravelText(result.travelPixels) +
              u8"，但拟合误差过大。请保持目标、视角和倍率完全固定后重试";
        return result;
    }
    // A full no-send baseline/return must remain stationary. This catches
    // slow drift that a high R² alone can hide.
    const auto baselineTime = moves.front().timeUs;
    Vec2 baseline, tail; int nb = 0, nt = 0;
    for (const auto& o : observations) {
        if (o.timeUs < baselineTime) { baseline += o.center; ++nb; }
        if (o.timeUs >= moves.back().timeUs + 220000) { tail += o.center; ++nt; }
    }
    Counts total;
    for (const auto& m : moves) { total.x += m.counts.x; total.y += m.counts.y; }
    if (nb < 3 || nt < 3 || total.x != 0 || total.y != 0 ||
        (baseline / nb - tail / nt).normSq() > 2.25) {
        result.valid = false;
        result.reason = u8"位移回到起点后目标位置发生变化：请保持目标、视角和倍率固定后重试";
        return result;
    }
    result.reason = u8"标定完成，可应用到所选参数档";
    return result;
}

} // namespace control
