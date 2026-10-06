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
    // Measured jitter of the box centre while nothing was sent (baseline + tail
    // windows). The fit cannot be better than this, so it scales the tolerance.
    Vec2 jitterPixels{};
    // Achieved normalised residual (sum of both axes) and usable-sample ratio.
    double score = 0.0;
    double inlierRatio = 1.0;
    // True when the best delay sits on the end of the scan range, i.e. the real
    // capture/render delay may be larger than the scan can express.
    bool delayAtScanLimit = false;
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
//
// ★ 现场最常见的失败是"拟合误差过大"，而误差通常不是来自算法，而是来自少数
//   被污染的画面：手抖一下、开了一枪的后坐、一帧运动模糊、检测框瞬跳。旧实现
//   用的是整段最小二乘 + 绝对门槛（rmse ≤ max(0.8, 6% 位移)、score ≤ 0.08），
//   几个离群帧就能把残差顶上去、把一轮标定判死。这里改成：
//     1. 两遍拟合：先最小二乘，再按残差中位数/MAD 剔掉离群帧重拟合；
//     2. 门槛跟着**实测静止抖动**走（静止窗口量出来的噪声），而不是写死 0.8px；
//     3. 失效时把实测抖动、实际残差、可用帧比例一起报出来，好判断是"场景脏"
//        还是"模型不对"。
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

    // 静止窗口（发送前的基线与回到起点之后）用来量"这台机器的检测噪声"：
    // 拟合做不到比它更准，所以后面的门槛按它放大。
    const auto baselineTime = moves.front().timeUs;
    const auto tailTime = moves.back().timeUs + 220000;
    auto staticJitter = [&](bool xAxis) {
        double sum = 0, sumSq = 0;
        int count = 0;
        for (const auto& o : observations) {
            const bool inBaseline = o.timeUs < baselineTime;
            const bool inTail = o.timeUs >= tailTime;
            if (!inBaseline && !inTail) continue;
            const double value = xAxis ? o.center.x : o.center.y;
            sum += value;
            sumSq += value * value;
            ++count;
        }
        if (count < 6) return 0.0;
        const double mean = sum / count;
        return std::sqrt(std::max(0.0, sumSq / count - mean * mean));
    };
    result.jitterPixels = {staticJitter(true), staticJitter(false)};

    // 两遍拟合用的轴模型。
    struct AxisFit {
        double gain = 0, rmse = 0, score = 0, inlierRatio = 1.0;
        double gainStdError = 0.0;  // 斜率的 1σ 标准误差（px/计数）
        double excursion = 0.0;     // 模型位移 = 计数范围 × 增益（px）
        bool computed = false;      // 方差门槛通过，gain 有意义
        bool valid = false;         // 全部门槛通过
    };
    // 灵敏度估计的相对误差上限：残差相对位移的比例会把"画面抖但录得久"的标定
    // 判死，而那种情况下增益其实很准；斜率的标准误差才是该看的量。
    constexpr double kMaxRelativeGainError = 0.08;
    // 静止抖动相对模型位移的上限：探测得让框动得比场景自身的抖动大得多，否则
    // "框动了"到底是因为我们发了位移、还是目标自己在动，是分不出来的。
    constexpr double kMaxJitterToExcursion = 0.5;
    auto varianceOf = [](const std::vector<double>& v, const std::vector<char>& keep) {
        double count = 0, sum = 0, sumSq = 0;
        for (size_t i = 0; i < v.size(); ++i) {
            if (!keep[i]) continue;
            sum += v[i]; sumSq += v[i] * v[i]; count += 1.0;
        }
        if (count <= 0.0) return 0.0;
        return std::max(0.0, sumSq - sum * sum / count);
    };
    auto leastSquares = [](const std::vector<double>& c, const std::vector<double>& p,
                           const std::vector<char>& keep, double& gain, double& intercept) {
        // ★ 必须数"保留了几帧"：用 keep.size() 会把整段的分母套到剔除后的子集上，
        //   增益被系统性带偏（实测 8% 污染就能偏 9%）。
        double count = 0;
        double sx = 0, sy = 0, sxx = 0, sxy = 0;
        for (size_t i = 0; i < c.size(); ++i) {
            if (!keep[i]) continue;
            count += 1.0;
            sx += c[i]; sy += p[i];
            sxx += c[i] * c[i]; sxy += c[i] * p[i];
        }
        if (count < 3.0) return false;
        const double vx = sxx - sx * sx / count;
        if (!(vx > 0.0)) return false;
        gain = -(sxy - sx * sy / count) / vx;
        intercept = (sy + gain * sx) / count;
        return true;
    };
    // 第一段：整段最小二乘。**延迟只由它选**，和旧实现一致 —— 两遍拟合不能拿来
    // 选延迟：低噪声时"剔除"可以把一个错误延迟的残差也剔到接近 0，于是挑出一个
    // 偏的延迟、把增益带偏（实测能偏到 9%）。剔除只用来稳住增益。
    auto lsFit = [&](const std::vector<double>& count, const std::vector<double>& position) {
        AxisFit f;
        const size_t n = count.size();
        if (n < 8) return f;
        std::vector<char> all(n, 1);
        const double vy = varianceOf(position, all);
        if (varianceOf(count, all) < n || vy < n * 2.0) return f;
        double gain = 0, intercept = 0;
        if (!leastSquares(count, position, all, gain, intercept)) return f;
        double sumSq = 0;
        for (size_t i = 0; i < n; ++i) {
            const double e = position[i] - intercept + gain * count[i];
            sumSq += e * e;
        }
        f.gain = gain;
        f.rmse = std::sqrt(sumSq / static_cast<double>(n));
        f.score = vy > 0.0 ? sumSq / vy : std::numeric_limits<double>::infinity();
        {
            const auto range = std::minmax_element(count.begin(), count.end());
            f.excursion = (*range.second - *range.first) * gain;
        }
        f.computed = true;
        return f;
    };
    // 第二段：稳健拟合，门槛全作用在它上面。
    //
    // ★ 剔除的初值必须稳健：整段最小二乘被离群帧拉偏 9% 时，用它的残差去剔除会
    //   剔出"与计数相关"的子集（大 |计数| 的干净帧被当成离群），增益反而更偏。
    //   所以先用 Theil–Sen 中位斜率（只取计数差足够大的点对）拿一个抗 50% 污染
    //   的初值，再剔除、再拟合。
    auto robustFit = [&](const std::vector<double>& count, const std::vector<double>& position,
                         double jitterPx, const AxisFit& seed) {
        AxisFit f;
        const size_t n = count.size();
        if (n < 8) return f;
        if (varianceOf(count, std::vector<char>(n, 1)) <= 0.0) return f;
        if (!seed.computed) return f;

        double robustSlope = 0.0;
        {
            const auto range = std::minmax_element(count.begin(), count.end());
            const double minGap = std::max(4.0, 0.15 * (*range.second - *range.first));
            std::vector<double> slopes;
            slopes.reserve(n * 4);
            for (size_t i = 0; i < n; ++i) {
                for (size_t j = i + 1; j < n; ++j) {
                    const double dc = count[i] - count[j];
                    if (std::abs(dc) < minGap) continue;
                    slopes.push_back(-(position[i] - position[j]) / dc);
                }
            }
            if (slopes.size() >= 16) {
                std::nth_element(slopes.begin(), slopes.begin() + slopes.size() / 2, slopes.end());
                robustSlope = slopes[slopes.size() / 2];
            }
        }

        const auto medianOf = [](std::vector<double>& values) {
            std::nth_element(values.begin(), values.begin() + values.size() / 2, values.end());
            return values[values.size() / 2];
        };
        double gain = robustSlope != 0.0 ? robustSlope : seed.gain;
        double intercept = 0.0;
        {
            std::vector<double> levels(n);
            for (size_t i = 0; i < n; ++i) levels[i] = position[i] + gain * count[i];
            intercept = medianOf(levels);
        }

        std::vector<char> keep(n, 1);
        size_t kept = n;
        for (int round = 0; round < 2; ++round) {
            std::vector<double> residual(n);
            for (size_t i = 0; i < n; ++i)
                residual[i] = position[i] - intercept + gain * count[i];
            std::vector<double> sorted = residual;
            const double median = medianOf(sorted);
            std::vector<double> deviations;
            deviations.reserve(n);
            for (const double e : residual) deviations.push_back(std::abs(e - median));
            const double mad = medianOf(deviations);
            const double sigma = 1.4826 * mad;
            // 阈值要有地板：现场抖动常是平滑的（压缩、缓慢晃动），它的 MAD 会远小于
            // 实际幅度，只按 3σ 剔会把正常帧也剔掉（可用帧比例掉下去，反而判死）。
            const double threshold = std::max({3.0 * sigma, 1.0, 2.0 * jitterPx});
            keep.assign(n, 0);
            kept = 0;
            for (size_t i = 0; i < n; ++i) {
                if (std::abs(residual[i] - median) <= threshold) { keep[i] = 1; ++kept; }
            }
            f.inlierRatio = static_cast<double>(kept) / static_cast<double>(n);
            if (kept < 8) return f;
            double g = 0, b = 0;
            if (!leastSquares(count, position, keep, g, b)) return f;
            gain = g; intercept = b;
        }

        double sumSq = 0;
        for (size_t i = 0; i < n; ++i) {
            if (!keep[i]) continue;
            const double e = position[i] - intercept + gain * count[i];
            sumSq += e * e;
        }
        f.gain = gain;
        f.rmse = std::sqrt(sumSq / static_cast<double>(kept));
        const double vy = varianceOf(position, keep);
        f.score = vy > 0.0 ? sumSq / vy : std::numeric_limits<double>::infinity();
        // 斜率的标准误差：σ_gain = rmse / sqrt(Σ(c-c̄)²)。"拟合误差过大"真正该问的
        // 是"灵敏度估得准不准"，而不是"残差占位移多少" —— 后者会把一段录了很久、
        // 只是画面偏抖的标定判死（实测那种情况下增益误差只有 2%）。
        double sxx = 0, meanCount = 0;
        for (size_t i = 0; i < n; ++i) if (keep[i]) meanCount += count[i];
        meanCount /= static_cast<double>(kept);
        for (size_t i = 0; i < n; ++i) {
            if (!keep[i]) continue;
            const double d = count[i] - meanCount;
            sxx += d * d;
        }
        f.gainStdError = sxx > 0.0 ? f.rmse / std::sqrt(sxx) : std::numeric_limits<double>::infinity();
        const double relativeGainError = f.gain > 0.0 ? f.gainStdError / f.gain
                                                      : std::numeric_limits<double>::infinity();
        const auto range = std::minmax_element(count.begin(), count.end());
        const double excursion = (*range.second - *range.first) * f.gain;
        // 残差容忍 = 实测静止抖动的 3 倍（拟合不可能比场景更稳），再给绝对地板和
        // "相对位移"两项：这一条只挡"模型根本不合"的粗差，精度由斜率误差管。
        // 判据分两类，各有各的用处：
        //  · 精度类 —— 斜率标准误差 ≤ 8%：录得久的时候，画面有点抖也不影响精度，
        //    旧实现用"残差占位移比例 ≤ 6%"会把这类完全可用的标定判死；
        //  · 可信类 —— 静止抖动 ≤ 0.5×模型位移：探测引起的位移必须明显大于场景
        //    自己的抖动（目标在动、检测乱跳），否则分不清是谁造成的位移，直接否掉。
        f.excursion = excursion;
        f.valid = f.gain >= 0.02 && f.gain <= 20.0 && excursion >= 6.0 &&
                  f.inlierRatio >= 0.6 && relativeGainError <= kMaxRelativeGainError &&
                  jitterPx <= kMaxJitterToExcursion * excursion &&
                  f.rmse <= std::max(1.0, 3.0 * jitterPx);
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
    // 200ms 之外的延迟也能被表达（采集卡 + 推理 + 渲染叠加时很常见），到边界时
    // 会明确提示，而不是悄悄给一个偏小的延迟。
    constexpr int kMaxDelayMs = 250;
    double best = std::numeric_limits<double>::infinity();
    int firstBest = 0, lastBest = 0;
    // 失败时的诊断：记下"最接近成功"的那次尝试，好把实测数字报给用户。
    AxisFit diagX, diagY;
    double diagScore = std::numeric_limits<double>::infinity();
    for (int delay = 0; delay <= kMaxDelayMs; ++delay) {
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
        const auto x = lsFit(xs, px), y = lsFit(ys, py);
        if (!x.computed || !y.computed) continue;
        const double score = x.score + y.score;
        if (score < diagScore) { diagScore = score; diagX = x; diagY = y; }
        if (score < best - 1e-10) {
            best = score; firstBest = lastBest = delay;
        } else if (std::abs(score - best) <= 1e-10) lastBest = delay;
    }
    result.delayMs = (firstBest + lastBest) * 0.5;
    result.delayAtScanLimit = firstBest >= kMaxDelayMs - 1;
    // 在选中的延迟上做鲁棒拟合：门槛（残差、R²、可用帧比例）全部作用在它上面。
    if (diagScore < std::numeric_limits<double>::infinity()) {
        const int chosen = std::clamp(static_cast<int>(std::lround(result.delayMs)), 0, kMaxDelayMs);
        size_t cursor = 0;
        Vec2 cumulative;
        for (size_t i = 0; i < observations.size(); ++i) {
            const auto cutoff = observations[i].timeUs - int64_t(chosen) * 1000;
            while (cursor < moves.size() && moves[cursor].timeUs <= cutoff) {
                cumulative += Vec2{double(moves[cursor].counts.x), double(moves[cursor].counts.y)};
                ++cursor;
            }
            xs[i] = cumulative.x; ys[i] = cumulative.y;
        }
        // 用与延迟选择一致的整段最小二乘当剔除初值。
        const auto x = robustFit(xs, px, result.jitterPixels.x, lsFit(xs, px));
        const auto y = robustFit(ys, py, result.jitterPixels.y, lsFit(ys, py));
        diagX = x; diagY = y;
        if (x.valid && y.valid) {
            result.valid = true;
            result.pixelsPerCount = {x.gain, y.gain};
            result.rmse = {x.rmse, y.rmse};
            result.score = x.score + y.score;
            result.inlierRatio = std::min(x.inlierRatio, y.inlierRatio);
        }
    }
    if (!result.valid) {
        // Below kMinTravelPixels the detector noise dominates the regression, so
        // the useful advice is about the scene, not about retrying unchanged.
        constexpr double kMinTravelPixels = 8.0;
        const double travel = std::min(result.travelPixels.x, result.travelPixels.y);
        if (travel < kMinTravelPixels) {
            result.reason = u8"移动像素过低：检测框全程只移动了 " + ffTravelText(result.travelPixels) +
                u8"。请在训练场改用更低的倍率或更高的游戏灵敏度后重试";
            return result;
        }
        const double relativeError = std::max(
            diagX.gain > 0.0 ? diagX.gainStdError / diagX.gain : 1.0,
            diagY.gain > 0.0 ? diagY.gainStdError / diagY.gain : 1.0);
        result.reason = u8"数据不稳定：检测框移动 " + ffTravelText(result.travelPixels) +
            u8"，拟合误差 X " + ffPixelsText(diagX.rmse) + u8" px、Y " + ffPixelsText(diagY.rmse) +
            u8" px；静止抖动 X " + ffPixelsText(result.jitterPixels.x) + u8" px、Y " +
            ffPixelsText(result.jitterPixels.y) + u8" px，可用帧 " +
            ffPixelsText(100.0 * std::min(diagX.inlierRatio, diagY.inlierRatio)) +
            u8"%，换算系数误差约 ±" + ffPixelsText(100.0 * relativeError) + u8"%" +
            (std::min(result.jitterPixels.x, result.jitterPixels.y) >
                     kMaxJitterToExcursion * std::min(diagX.excursion, diagY.excursion)
                ? u8"。画面自身抖动太大（相对位移）：目标在动或检测框在跳，请换更清楚的目标、"
                  u8"降低采集噪点，或在训练场把目标放远一点让位移更明显"
                : relativeError > kMaxRelativeGainError
                    ? u8"。位移相对噪声太小：请拉长标定（第二次会更准）、降低采集噪点或换更清楚的目标"
                    : u8"。请保持目标、视角和倍率完全固定后重试");
        return result;
    }
    // A full no-send baseline/return must remain stationary. This catches
    // slow drift that a high R² alone can hide.
    Vec2 baseline, tail; int nb = 0, nt = 0;
    for (const auto& o : observations) {
        if (o.timeUs < baselineTime) { baseline += o.center; ++nb; }
        if (o.timeUs >= tailTime) { tail += o.center; ++nt; }
    }
    Counts total;
    for (const auto& m : moves) { total.x += m.counts.x; total.y += m.counts.y; }
    const double driftLimit = 1.5 + std::max(result.jitterPixels.x, result.jitterPixels.y) * 2.0;
    if (nb < 3 || nt < 3 || total.x != 0 || total.y != 0 ||
        (baseline / nb - tail / nt).normSq() > driftLimit * driftLimit) {
        result.valid = false;
        result.reason = u8"位移回到起点后目标位置发生变化（回位偏差超过 " +
            ffPixelsText(driftLimit) + u8" px）：请保持目标、视角和倍率固定后重试";
        return result;
    }
    result.reason = result.delayAtScanLimit
        ? u8"标定完成，可应用到所选参数档；但响应延迟已到扫描上限 " +
              ffPixelsText(result.delayMs) + u8" ms，若延迟补偿效果不理想请检查采集/渲染延迟"
        : u8"标定完成，可应用到所选参数档";
    return result;
}

} // namespace control
