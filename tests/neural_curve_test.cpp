#include "mouse/aim_path.h"
#include "mouse/neural_curve.h"
#include "runtime/aim_path_config.h"

#include <cmath>
#include <cstdio>
#include <vector>

int main()
{
    std::vector<boss::NeuralTrajectory> demos;
    for (int j = 0; j < 15; ++j)
    {
        boss::NeuralTrajectory trajectory;
        for (int i = 0; i < 256; ++i)
        {
            const double t = static_cast<double>(i) / 255.0;
            trajectory.push_back({ t, 0.28 * std::sin(3.141592653589793 * t)
                                    + 0.008 * std::sin(17.0 * t + j) });
        }
        demos.push_back(std::move(trajectory));
    }
    const auto trained = boss::trainNeuralCurve(demos);
    if (!trained.success || trained.quality.validationTrajectories != 3 ||
        trained.quality.validationRmse >= 0.04 ||
        trained.quality.validationRmse >= trained.quality.baselineRmse * 0.5 ||
        std::abs(boss::evaluateNeuralCurve(trained.weights, 0.0)) > 1e-9 ||
        std::abs(boss::evaluateNeuralCurve(trained.weights, 1.0)) > 1e-9)
    {
        std::puts("neural curve fit, validation, or endpoint contract failed");
        return 1;
    }

    boss::AimPathDriver path;
    HotkeyProfile profile;
    profile.aim_path_mode = 4;
    profile.aim_path_neural_trained = true;
    profile.aim_path_neural_weights = trained.weights;
    const auto params = runtime::aim_loop::pathParamsFrom(profile);
    if (params.mode != boss::AimPathDriver::Mode::Custom ||
        !params.neural_enabled || params.neural_weights != trained.weights)
    {
        std::puts("trained profile was not wired to live neural path");
        return 1;
    }
    path.configure(params);
    bool bent = false;
    for (int i = 0; i < 60; ++i)
    {
        const auto move = path.step(90, 0, 0, 0, 1.0 / 120.0, 1, 8, 0);
        bent = bent || std::abs(move.move_y) > 0.01;
        if (std::hypot(move.move_x, move.move_y) > 8.01)
        {
            std::puts("trained path changed PID movement magnitude");
            return 1;
        }
    }
    if (!bent || boss::trainNeuralCurve({ demos[0] }).success)
    {
        std::puts("trained path was straight or insufficient data accepted");
        return 1;
    }
    profile.aim_path_neural_trained = false;
    if (runtime::aim_loop::pathParamsFrom(profile).mode !=
        boss::AimPathDriver::Mode::Linear)
    {
        std::puts("untrained neural mode did not fall back to linear");
        return 1;
    }

    auto inconsistent = demos;
    for (size_t index : { size_t{0}, size_t{5}, size_t{10} })
        for (auto& point : inconsistent[index])
            point.deviation = 0.75 * std::sin(3.141592653589793 * point.progress);
    const auto poor = boss::trainNeuralCurve(inconsistent);
    if (!poor.success || poor.quality.validationRmse <=
                         poor.quality.trainingRmse + 0.15 || poor.weights != trained.weights)
    {
        std::puts("held-out trajectories did not expose inconsistent training data");
        return 1;
    }
    // Real users draw in both directions. Whole-stroke alignment must preserve
    // both arches and S shapes, even with noise and a minority of detours.
    for (int count : {100, 200}) for (bool sShape : {false, true}) {
        std::vector<boss::NeuralTrajectory> mixed;
        for (int j = 0; j < count; ++j) {
            boss::NeuralTrajectory stroke;
            for (int i = 0; i < 256; ++i) {
                const double t = i / 255.0;
                const double shape = std::sin((sShape ? 2.0 : 1.0) * 3.141592653589793 * t);
                const double amplitude = j % 13 == 0 ? .55 : .12 + .015 * std::sin(j);
                stroke.push_back({t, (j % 2 ? -1 : 1) * amplitude * shape
                    + .003 * std::sin(31 * t + j) * std::sin(3.141592653589793 * t)});
            }
            mixed.push_back(std::move(stroke));
        }
        const auto model = boss::trainNeuralCurve(mixed);
        const double first = boss::evaluateNeuralCurve(model.weights, sShape ? .25 : .5);
        const double last = boss::evaluateNeuralCurve(model.weights, .75);
        std::printf("%d strokes, S=%d: bend=%.5f, validation=%.5f, baseline=%.5f\n",
            count, sShape, first, model.quality.validationRmse, model.quality.baselineRmse);
        if (!model.success || std::abs(first) < .09 || std::abs(first) > .16 ||
            (sShape && first * last >= 0) ||
            model.quality.validationRmse >= model.quality.baselineRmse * .85) {
            std::puts("mixed directions lost shape or outliers dominated the fit");
            return 1;
        }
    }
    std::vector<boss::NeuralTrajectory> straight(100, demos.front());
    for (auto& stroke : straight) for (auto& p : stroke) p.deviation = 0;
    const auto flat = boss::trainNeuralCurve(straight);
    for (int i = 0; i <= 100; ++i)
        if (!flat.success || std::abs(boss::evaluateNeuralCurve(flat.weights, i / 100.0)) > .005)
            return 1; // Do not invent a personal bend from truly straight data.

    auto previous = boss::randomNeuralCurve(0);
    for (unsigned seed = 1; seed <= 200; ++seed) {
        const auto random = boss::randomNeuralCurve(seed);
        double peak = 0;
        for (int i = 0; i <= 1000; ++i) {
            const double y = boss::evaluateNeuralCurve(random.weights, i / 1000.0);
            if (!std::isfinite(y) || std::abs(y) > .34) return 1;
            peak = std::max(peak, std::abs(y));
        }
        if (!random.success || random.weights == previous.weights || peak < .06 ||
            random.quality.trainingTrajectories != 0 ||
            boss::evaluateNeuralCurve(random.weights, 0) != 0 ||
            boss::evaluateNeuralCurve(random.weights, 1) != 0) return 1;
        previous = random;
    }
    return 0;
}
