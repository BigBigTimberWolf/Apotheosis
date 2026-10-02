#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <random>
#include <string>
#include <vector>

namespace boss {

struct NeuralCurvePoint
{
    double progress = 0.0;
    double deviation = 0.0;
};

using NeuralTrajectory = std::vector<NeuralCurvePoint>;

struct NeuralCurveQuality
{
    int trainingTrajectories = 0;
    int validationTrajectories = 0;
    double trainingRmse = 0.0;
    double validationRmse = 0.0;
    double validationP95 = 0.0;
    double baselineRmse = 0.0; // 全直线在相同验证集上的误差，用于对照。
    double slopeVariation = 0.0;
};

struct NeuralCurveTrainResult
{
    bool success = false;
    std::array<float, 25> weights{};
    NeuralCurveQuality quality;
    std::string error;
};

// 与 AimPathDriver 使用完全相同的 1→8→1 网络和端点包络。
inline double evaluateNeuralCurve(const std::array<float, 25>& weights, double progress)
{
    const double t = std::clamp(progress, 0.0, 1.0);
    const double x = t * 2.0 - 1.0;
    double raw = static_cast<double>(weights[24]);
    for (int h = 0; h < 8; ++h)
        raw += static_cast<double>(weights[16 + h]) * std::tanh(
            static_cast<double>(weights[h]) * x + static_cast<double>(weights[8 + h]));
    return std::tanh(raw) * 4.0 * t * (1.0 - t);
}

// Generate directly in the runtime model format: smooth, bounded, and zero at
// both endpoints. This is a generated curve, not a fitted personal model.
inline NeuralCurveTrainResult randomNeuralCurve(std::uint32_t seed)
{
    std::mt19937 rng(seed);
    std::uniform_real_distribution<double> unit(0.0, 1.0);
    NeuralCurveTrainResult result;
    const double sign = unit(rng) < 0.5 ? -1.0 : 1.0;
    result.weights[24] = static_cast<float>(sign * (0.12 + 0.14 * unit(rng)));
    for (int h = 0; h < 3; ++h) {
        result.weights[h] = static_cast<float>(0.5 + 1.5 * unit(rng));
        result.weights[8 + h] = static_cast<float>(unit(rng) - 0.5);
        result.weights[16 + h] = static_cast<float>(sign * (unit(rng) - 0.5) * 0.05);
    }
    result.success = true;
    return result;
}

inline NeuralCurveTrainResult trainNeuralCurve(const std::vector<NeuralTrajectory>& input)
{
    NeuralCurveTrainResult result;
    std::vector<const NeuralTrajectory*> valid;
    for (const auto& trajectory : input)
    {
        if (trajectory.size() < 32) continue;
        bool sane = true;
        double previous = -1.0;
        for (const auto& point : trajectory)
        {
            if (!std::isfinite(point.progress) || !std::isfinite(point.deviation) ||
                point.progress < previous || point.progress < 0.0 || point.progress > 1.0 ||
                std::abs(point.deviation) > 1.0)
            {
                sane = false;
                break;
            }
            previous = point.progress;
        }
        if (sane && trajectory.front().progress <= 0.01 &&
            trajectory.back().progress >= 0.99)
            valid.push_back(&trajectory);
    }
    if (valid.size() < 5)
    {
        result.error = "至少需要 5 条有效轨迹才能留出独立验证样本";
        return result;
    }

    // Equal spatial sampling gives each stroke equal weight. Determine the
    // reference shape using training strokes ONLY; held-out strokes never pick
    // the reference or fit the prototype. Mirror whole strokes, not individual
    // points, so S curves retain their changes of direction.
    constexpr int kShapeSamples = 128;
    std::vector<NeuralTrajectory> aligned(valid.size());
    for (size_t j = 0; j < valid.size(); ++j) {
        const auto& source = *valid[j];
        size_t segment = 0;
        for (int i = 0; i < kShapeSamples; ++i) {
            const double t = static_cast<double>(i) / (kShapeSamples - 1);
            while (segment + 1 < source.size() && source[segment + 1].progress < t)
                ++segment;
            const auto& a = source[segment];
            const auto& b = source[std::min(segment + 1, source.size() - 1)];
            const double f = b.progress > a.progress
                ? std::clamp((t - a.progress) / (b.progress - a.progress), 0.0, 1.0) : 0.0;
            aligned[j].push_back({t, i == 0 || i == kShapeSamples - 1
                ? 0.0 : a.deviation + f * (b.deviation - a.deviation)});
        }
    }
    auto dot = [&](size_t a, size_t b) {
        double sum = 0.0;
        for (int i = 0; i < kShapeSamples; ++i)
            sum += aligned[a][i].deviation * aligned[b][i].deviation;
        return sum;
    };
    std::vector<double> norms(valid.size());
    for (size_t j = 0; j < valid.size(); ++j) norms[j] = std::sqrt(dot(j, j));
    size_t reference = 1;
    double bestSimilarity = -1.0;
    for (size_t a = 0; a < valid.size(); ++a) {
        if (a % 5 == 0 || norms[a] < 1e-6) continue;
        double similarity = 0.0;
        for (size_t b = 0; b < valid.size(); ++b)
            if (b % 5 != 0 && norms[b] >= 1e-6)
                similarity += std::abs(dot(a, b)) / (norms[a] * norms[b]);
        if (similarity > bestSimilarity) {
            bestSimilarity = similarity;
            reference = a;
        }
    }
    for (size_t j = 0; j < valid.size(); ++j)
        if (dot(j, reference) < 0.0)
            for (auto& point : aligned[j]) point.deviation = -point.deviation;

    std::vector<const NeuralTrajectory*> training, validation;
    for (size_t i = 0; i < valid.size(); ++i)
        (i % 5 == 0 ? validation : training).push_back(&aligned[i]);
    result.quality.trainingTrajectories = static_cast<int>(training.size());
    result.quality.validationTrajectories = static_cast<int>(validation.size());

    // Median preserves the typical bend while suppressing occasional detours.
    // Fitting one prototype also keeps 100-200 stroke training inexpensive.
    NeuralTrajectory prototype;
    for (int i = 0; i < kShapeSamples; ++i) {
        std::vector<double> values;
        for (const auto* stroke : training) values.push_back((*stroke)[i].deviation);
        std::sort(values.begin(), values.end());
        const size_t middle = values.size() / 2;
        prototype.push_back({aligned[0][i].progress,
            (values[middle] + values[(values.size() - 1) / 2]) * 0.5});
    }
    const std::vector<const NeuralTrajectory*> fitting{&prototype};

    std::mt19937 rng(0x41504F54u);
    std::normal_distribution<double> init(0.0, 0.24);
    std::array<double, 25> w{}, m{}, v{}, best{};
    for (int h = 0; h < 8; ++h)
    {
        w[h] = (h - 3.5) * 0.8;
        w[8 + h] = init(rng) * 0.5;
        w[16 + h] = init(rng);
    }

    auto evaluate = [](const std::array<double, 25>& weights,
                       const std::vector<const NeuralTrajectory*>& set) {
        double sum = 0.0;
        size_t n = 0;
        for (const auto* trajectory : set)
            for (const auto& point : *trajectory)
            {
                const double t = point.progress;
                const double x = 2.0 * t - 1.0;
                double raw = weights[24];
                for (int h = 0; h < 8; ++h)
                    raw += weights[16 + h] * std::tanh(weights[h] * x + weights[8 + h]);
                const double e = std::tanh(raw) * 4.0 * t * (1.0 - t) - point.deviation;
                sum += e * e;
                ++n;
            }
        return n ? std::sqrt(sum / static_cast<double>(n))
                 : std::numeric_limits<double>::infinity();
    };

    double bestTraining = std::numeric_limits<double>::infinity();
    int staleChecks = 0;
    for (int epoch = 1; epoch <= 1200; ++epoch)
    {
        std::array<double, 25> grad{};
        size_t samples = 0;
        for (const auto* trajectory : fitting)
            for (const auto& point : *trajectory)
            {
                const double t = point.progress;
                const double x = 2.0 * t - 1.0;
                const double envelope = 4.0 * t * (1.0 - t);
                std::array<double, 8> activation{};
                double raw = w[24];
                for (int h = 0; h < 8; ++h)
                {
                    activation[h] = std::tanh(w[h] * x + w[8 + h]);
                    raw += w[16 + h] * activation[h];
                }
                const double squashed = std::tanh(raw);
                const double delta = 2.0 * (squashed * envelope - point.deviation)
                                   * envelope * (1.0 - squashed * squashed);
                grad[24] += delta;
                for (int h = 0; h < 8; ++h)
                {
                    grad[16 + h] += delta * activation[h];
                    const double hiddenDelta = delta * w[16 + h]
                                             * (1.0 - activation[h] * activation[h]);
                    grad[h] += hiddenDelta * x;
                    grad[8 + h] += hiddenDelta;
                }
                ++samples;
            }
        if (samples == 0) break;
        const double beta1Power = std::pow(0.9, epoch);
        const double beta2Power = std::pow(0.999, epoch);
        for (size_t i = 0; i < w.size(); ++i)
        {
            double g = grad[i] / static_cast<double>(samples);
            if (i < 8 || (i >= 16 && i < 24)) g += 0.0002 * w[i];
            g = std::clamp(g, -1.0, 1.0);
            m[i] = 0.9 * m[i] + 0.1 * g;
            v[i] = 0.999 * v[i] + 0.001 * g * g;
            w[i] -= 0.025 * (m[i] / (1.0 - beta1Power)) /
                    (std::sqrt(v[i] / (1.0 - beta2Power)) + 1e-8);
        }
        if (epoch % 10 == 0)
        {
            const double score = evaluate(w, fitting);
            if (score + 1e-7 < bestTraining)
            {
                bestTraining = score;
                best = w;
                staleChecks = 0;
            }
            else if (++staleChecks >= 12 && epoch >= 300)
                break;
        }
    }

    if (!std::isfinite(bestTraining))
    {
        result.error = "训练未产生有效模型";
        return result;
    }
    for (size_t i = 0; i < best.size(); ++i)
    {
        if (!std::isfinite(best[i]) || std::abs(best[i]) > 20.0)
        {
            result.error = "模型参数超出安全范围";
            return result;
        }
        result.weights[i] = static_cast<float>(best[i]);
    }

    result.quality.trainingRmse = evaluate(best, training);
    // 留出的整条轨迹只在训练结束后评估，不参与权重更新或模型选择。
    result.quality.validationRmse = evaluate(best, validation);
    std::vector<double> errors;
    double baseline = 0.0;
    for (const auto* trajectory : validation)
        for (const auto& point : *trajectory)
        {
            const double e = std::abs(evaluateNeuralCurve(result.weights, point.progress)
                                    - point.deviation);
            errors.push_back(e);
            baseline += point.deviation * point.deviation;
        }
    std::sort(errors.begin(), errors.end());
    result.quality.validationP95 = errors[static_cast<size_t>(0.95 * (errors.size() - 1))];
    result.quality.baselineRmse = std::sqrt(baseline / errors.size());

    constexpr int kEvaluationSteps = 256;
    double previousSlope = 0.0;
    for (int i = 1; i < kEvaluationSteps; ++i)
    {
        const double t0 = static_cast<double>(i - 1) / (kEvaluationSteps - 1);
        const double t1 = static_cast<double>(i) / (kEvaluationSteps - 1);
        const double slope = (evaluateNeuralCurve(result.weights, t1)
                            - evaluateNeuralCurve(result.weights, t0)) / (t1 - t0);
        if (i > 1) result.quality.slopeVariation += std::abs(slope - previousSlope);
        previousSlope = slope;
    }
    result.quality.slopeVariation /= (kEvaluationSteps - 2);
    result.success = true;
    return result;
}

}
