#ifndef MOUSE_AIM_PATH_H
#define MOUSE_AIM_PATH_H

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include "neural_curve.h"

namespace boss
{

class AimPathDriver
{
public:
    enum class Mode : int
    {
        Linear = 0,
        Bezier = 1,
        Custom = 2,
        WindMouse = 3,
    };

    static constexpr int kCustomSamples = 32768;
    static constexpr int kWindSamples = 256;
    static constexpr int kWindMaxSteps = 4096;

    struct Params
    {
        Mode  mode = Mode::Linear;
        double strength = 0.25;

        double cx1 = 0.30, cy1 = 0.00;
        double cx2 = 0.70, cy2 = 0.00;

        std::shared_ptr<const std::vector<float>> custom_samples;
        bool neural_enabled = false;
        std::array<float, 25> neural_weights{};

        double wind_gravity    = 5.0;
        double wind_wind       = 2.0;
        double wind_step       = 10.0;
        double wind_distance   = 8.0;
        double wind_threshold_px = 10.0;

        double reanchor_px = 40.0;
    };

    struct Result
    {
        double move_x = 0;
        double move_y = 0;
    };

    void configure(const Params& p)
    {
        const bool shape_changed =
            p.mode != p_.mode ||
            p.cx1 != p_.cx1 || p.cy1 != p_.cy1 ||
            p.cx2 != p_.cx2 || p.cy2 != p_.cy2 ||
            p.custom_samples != p_.custom_samples ||
            p.neural_enabled != p_.neural_enabled ||
            p.neural_weights != p_.neural_weights ||
            p.wind_gravity != p_.wind_gravity ||
            p.wind_wind != p_.wind_wind ||
            p.wind_step != p_.wind_step ||
            p.wind_distance != p_.wind_distance;
        if (shape_changed)
            reset();
        p_ = p;
    }

    void reset()
    {
        engaged_ = false;
        last_id_ = -1;
        progress_ = 0.0;
        reference_length_ = 1.0;
        axis_x_ = 1.0;
        axis_y_ = 0.0;
        smoothed_slope_ = 0.0;
        rx_ = ry_ = 0.0;
        wind_profile_.clear();
    }

    void applyMove(int dx, int dy)
    {
        (void)dx;
        (void)dy;
    }

    Result step(double aim_x, double aim_y,
                double cur_x, double cur_y,
                double  , int target_id,
                double base_dx, double base_dy,
                double settle_radius_px = 0.0)
    {
        Result out;

        if (p_.mode == Mode::Linear)
        {
            out.move_x = base_dx;
            out.move_y = base_dy;
            return out;
        }

        const double screen_err_x = aim_x - cur_x;
        const double screen_err_y = aim_y - cur_y;
        const double screen_err_mag = std::hypot(screen_err_x, screen_err_y);

        if (p_.mode == Mode::WindMouse)
        {
            const double gate = std::max(0.0, p_.wind_threshold_px);
            if (std::abs(screen_err_x) <= gate && std::abs(screen_err_y) <= gate)
            {
                engaged_ = false;
                wind_profile_.clear();
                smoothed_slope_ = 0.0;
                rx_ = ry_ = 0.0;
                out.move_x = base_dx;
                out.move_y = base_dy;
                return out;
            }
        }

        const bool id_changed = (target_id >= 0 && target_id != last_id_);
        if (!engaged_ || id_changed)
        {
            engaged_ = true;
            last_id_ = target_id;
            progress_ = 0.0;
            reference_length_ = std::max(1.0, screen_err_mag);
            if (screen_err_mag > 1e-6)
            {
                axis_x_ = screen_err_x / screen_err_mag;
                axis_y_ = screen_err_y / screen_err_mag;
            }
            smoothed_slope_ = 0.0;
            rx_ = ry_ = 0.0;
            if (p_.mode == Mode::WindMouse)
                build_wind_profile(reference_length_, target_id);
        }
        last_id_ = target_id;

        const double base_mag = std::hypot(static_cast<double>(base_dx),
                                           static_cast<double>(base_dy));
        if (base_mag < 0.5)
        {
            rx_ = ry_ = 0.0;
            return out;
        }

        if (screen_err_mag < 1.0)
        {
            out.move_x = base_dx;
            out.move_y = base_dy;
            return out;
        }

        const double settle_radius = std::max(0.0, settle_radius_px);
        if (settle_radius > 0.0 && screen_err_mag <= settle_radius)
        {
            rx_ = ry_ = 0.0;
            out.move_x = base_dx;
            out.move_y = base_dy;
            return out;
        }

        const double forward = static_cast<double>(base_dx) * screen_err_x
                             + static_cast<double>(base_dy) * screen_err_y;
        if (forward <= 0.0)
        {
            out.move_x = base_dx;
            out.move_y = base_dy;
            return out;
        }

        if (progress_ >= 1.0 - 1e-9)
        {
            progress_ = 0.0;
            reference_length_ = std::max(1.0, screen_err_mag);
            if (screen_err_mag > 1e-6)
            {
                axis_x_ = screen_err_x / screen_err_mag;
                axis_y_ = screen_err_y / screen_err_mag;
            }
            smoothed_slope_ = 0.0;
            rx_ = ry_ = 0.0;
            if (p_.mode == Mode::WindMouse)
                build_wind_profile(reference_length_, last_id_);
        }

        const double raw_slope = std::clamp(
            curve_derivative(progress_), -3.0, 3.0);
        constexpr double kSlopeAlpha = 0.18;
        smoothed_slope_ += (raw_slope - smoothed_slope_) * kSlopeAlpha;
        const double dy_dt = smoothed_slope_;

        const auto smoothstep01 = [](double value) {
            const double u = clamp01(value);
            return u * u * (3.0 - 2.0 * u);
        };
        const double entry_fade = smoothstep01(progress_ / 0.10);
        const double exit_fade = smoothstep01((1.0 - progress_) / 0.15);
        double influence = std::clamp(p_.strength, 0.0, 1.0)
                         * entry_fade * exit_fade;
        if (settle_radius > 0.0)
        {
            const double fade_end = settle_radius * 2.5;
            const double u = clamp01((screen_err_mag - settle_radius) /
                                     std::max(1.0, fade_end - settle_radius));
            const double smooth = u * u * (3.0 - 2.0 * u);
            influence *= smooth;
        }
        const double local_scale = std::sqrt(1.0 + dy_dt * dy_dt);
        progress_ = clamp01(progress_ + base_mag /
            std::max(1.0, reference_length_ * local_scale));

        if (std::abs(dy_dt) < 1e-9 || influence < 1e-4)
        {
            rx_ = ry_ = 0.0;
            out.move_x = base_dx;
            out.move_y = base_dy;
            return out;
        }

        const double perp_x = -axis_y_;
        const double perp_y =  axis_x_;
        const double local_x = static_cast<double>(base_dx) * axis_x_
                             + static_cast<double>(base_dy) * axis_y_;
        const double local_y = static_cast<double>(base_dx) * perp_x
                             + static_cast<double>(base_dy) * perp_y;
        const double inv_scale = 1.0 / local_scale;
        const double cos_a = inv_scale;
        const double sin_a = dy_dt * inv_scale;
        const double shaped_local_x = local_x * cos_a - local_y * sin_a;
        const double shaped_local_y = local_x * sin_a + local_y * cos_a;
        const double curved_x = shaped_local_x * axis_x_ + shaped_local_y * perp_x;
        const double curved_y = shaped_local_x * axis_y_ + shaped_local_y * perp_y;

        double step_x = static_cast<double>(base_dx)
                      + (curved_x - static_cast<double>(base_dx)) * influence;
        double step_y = static_cast<double>(base_dy)
                      + (curved_y - static_cast<double>(base_dy)) * influence;
        const double mixed_mag = std::hypot(step_x, step_y);
        if (mixed_mag > 1e-9)
        {
            const double restore = base_mag / mixed_mag;
            step_x *= restore;
            step_y *= restore;
        }

        const double raw_x = step_x + rx_;
        const double raw_y = step_y + ry_;

        out.move_x = raw_x;
        out.move_y = raw_y;
        rx_ = 0;
        ry_ = 0;
        return out;
    }

private:
    static double clamp01(double v) { return v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v); }

    void build_wind_profile(double length_px, int target_id)
    {
        wind_profile_.assign(kWindSamples, 0.0f);
        std::vector<char> filled(kWindSamples, 0);

        rng_ ^= static_cast<std::uint32_t>(target_id) * 0x9E3779B1u;
        rng_ |= 1u;

        const double L = std::max(1.0, length_px);
        const double gravity  = std::clamp(p_.wind_gravity, 0.0, 200.0);
        const double wind     = std::clamp(p_.wind_wind, 0.0, 200.0);
        const double dist0    = std::clamp(p_.wind_distance, 0.1, 200.0);
        double step_max       = std::clamp(p_.wind_step, 0.1, 200.0);

        constexpr double kSqrt3 = 1.7320508075688772;
        constexpr double kSqrt5 = 2.2360679774997896;

        auto rand01 = [this]() {
            rng_ ^= rng_ << 13;
            rng_ ^= rng_ >> 17;
            rng_ ^= rng_ << 5;
            return static_cast<double>(rng_) / 4294967296.0;
        };

        auto record = [&](double x, double y) {
            const double t = clamp01(x / L);
            int idx = static_cast<int>(std::lround(t * (kWindSamples - 1)));
            idx = std::clamp(idx, 0, kWindSamples - 1);
            wind_profile_[static_cast<size_t>(idx)] = static_cast<float>(y / L);
            filled[static_cast<size_t>(idx)] = 1;
        };

        double cx = 0.0, cy = 0.0;
        double vx = 0.0, vy = 0.0;
        double wx = 0.0, wy = 0.0;
        double dist = L;
        record(0.0, 0.0);

        for (int guard = 0; guard < kWindMaxSteps && dist >= 1.0; ++guard)
        {
            const double w_mag = std::min(wind, dist);
            if (dist >= dist0)
            {
                wx = wx / kSqrt3 + (2.0 * rand01() - 1.0) * w_mag / kSqrt5;
                wy = wy / kSqrt3 + (2.0 * rand01() - 1.0) * w_mag / kSqrt5;
            }
            else
            {
                wx /= kSqrt3;
                wy /= kSqrt3;
                if (step_max < 3.0)
                    step_max = rand01() * 3.0 + 3.0;
                else
                    step_max /= kSqrt5;
            }

            vx += wx + gravity * (L - cx) / dist;
            vy += wy + gravity * (0.0 - cy) / dist;

            const double v_mag = std::hypot(vx, vy);
            if (v_mag > step_max)
            {
                const double v_clip = step_max / 2.0 + rand01() * step_max / 2.0;
                vx = vx / v_mag * v_clip;
                vy = vy / v_mag * v_clip;
            }

            cx += vx;
            cy += vy;
            dist = std::hypot(L - cx, 0.0 - cy);
            record(cx, cy);
        }

        wind_profile_.front() = 0.0f;
        wind_profile_.back()  = 0.0f;
        filled.front() = 1;
        filled.back()  = 1;

        int prev = -1;
        for (int i = 0; i < kWindSamples; ++i)
        {
            if (!filled[static_cast<size_t>(i)])
                continue;
            if (prev >= 0 && i - prev > 1)
            {
                const double y0 = wind_profile_[static_cast<size_t>(prev)];
                const double y1 = wind_profile_[static_cast<size_t>(i)];
                for (int k = prev + 1; k < i; ++k)
                {
                    const double f = static_cast<double>(k - prev)
                                   / static_cast<double>(i - prev);
                    wind_profile_[static_cast<size_t>(k)] =
                        static_cast<float>(y0 + (y1 - y0) * f);
                }
            }
            prev = i;
        }
    }

    double curve_y(double t) const
    {
        if (p_.mode == Mode::Bezier)
        {
            double u = t;
            for (int i = 0; i < 6; ++i)
            {
                const double x  = bezier_axis(u, 0.0, p_.cx1, p_.cx2, 1.0);
                const double dx = bezier_deriv(u, 0.0, p_.cx1, p_.cx2, 1.0);
                if (std::abs(dx) < 1e-6) break;
                u -= (x - t) / dx;
                u  = clamp01(u);
            }
            return bezier_axis(u, 0.0, p_.cy1, p_.cy2, 0.0);
        }
        if (p_.mode == Mode::Custom)
        {
            if (p_.neural_enabled)
                return evaluateNeuralCurve(p_.neural_weights, t);
            if (!p_.custom_samples) return 0.0;
            const auto& samples = *p_.custom_samples;
            const int N = static_cast<int>(samples.size());
            if (N < 2) return 0.0;
            const double pos = t * (N - 1);
            int i0 = static_cast<int>(std::floor(pos));
            int i1 = i0 + 1;
            if (i0 < 0)      { i0 = 0;     i1 = 1; }
            if (i1 > N - 1)  { i1 = N - 1; i0 = i1 - 1; }
            const double f = pos - i0;
            const double y0 = static_cast<double>(samples[i0]);
            const double y1 = static_cast<double>(samples[i1]);
            return y0 + (y1 - y0) * f;
        }
        if (p_.mode == Mode::WindMouse)
        {
            return sample_profile(wind_profile_, t);
        }
        return 0.0;
    }

    static double sample_profile(const std::vector<float>& profile, double t)
    {
        const int n = static_cast<int>(profile.size());
        if (n < 2)
            return 0.0;
        const double pos = clamp01(t) * (n - 1);
        int i0 = static_cast<int>(std::floor(pos));
        int i1 = i0 + 1;
        if (i0 < 0)     { i0 = 0;     i1 = 1; }
        if (i1 > n - 1) { i1 = n - 1; i0 = i1 - 1; }
        const double f = pos - i0;
        return static_cast<double>(profile[static_cast<size_t>(i0)]) * (1.0 - f)
             + static_cast<double>(profile[static_cast<size_t>(i1)]) * f;
    }

    double curve_derivative(double t) const
    {
        const double eps = (p_.mode == Mode::WindMouse)
                               ? 4.0 / (kWindSamples - 1)
                               : 0.003;
        const double ta = std::max(0.0, t - eps);
        const double tb = std::min(1.0, t + eps);
        return (tb > ta) ? (curve_y(tb) - curve_y(ta)) / (tb - ta) : 0.0;
    }

    static double bezier_axis(double u, double a, double b, double c, double d)
    {
        const double m = 1.0 - u;
        return m*m*m*a + 3.0*m*m*u*b + 3.0*m*u*u*c + u*u*u*d;
    }
    static double bezier_deriv(double u, double a, double b, double c, double d)
    {
        const double m = 1.0 - u;
        return 3.0*m*m*(b - a) + 6.0*m*u*(c - b) + 3.0*u*u*(d - c);
    }

    Params p_{};
    bool engaged_ = false;
    int  last_id_ = -1;
    double progress_ = 0.0;
    double reference_length_ = 1.0;
    double axis_x_ = 1.0, axis_y_ = 0.0;
    double smoothed_slope_ = 0.0;
    double rx_ = 0.0, ry_ = 0.0;
    std::vector<float> wind_profile_;
    std::uint32_t rng_ = 0x9E3779B9u;
};

}

#endif // MOUSE_AIM_PATH_H
