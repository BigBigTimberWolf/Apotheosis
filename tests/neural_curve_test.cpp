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
            point.deviation = -0.28 * std::sin(3.141592653589793 * point.progress);
    const auto poor = boss::trainNeuralCurve(inconsistent);
    if (!poor.success || poor.quality.validationRmse <=
                         poor.quality.trainingRmse + 0.15)
    {
        std::puts("held-out trajectories did not expose inconsistent training data");
        return 1;
    }
    return 0;
}
