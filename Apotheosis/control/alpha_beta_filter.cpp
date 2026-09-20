#include "alpha_beta_filter.h"

#include <algorithm>
#include <cmath>

namespace control {

namespace {

double clampDtTicks(double tauSeconds, double dtSeconds, double& ticksOut)
{
    if (tauSeconds <= 0.0 || dtSeconds <= 0.0)
    {
        ticksOut = 1.0;
        return 1.0;
    }
    ticksOut = tauSeconds / dtSeconds;
    return ticksOut;
}

}

AlphaBetaFilter::AlphaBetaFilter(const AlphaBetaParams& params)
    : params_(params)
{
}

double AlphaBetaFilter::alphaForTau(double tauSeconds, double dtSeconds)
{
    if (tauSeconds <= 0.0)
        return 1.0;
    if (dtSeconds <= 0.0)
        return 0.0;
    const double a = 1.0 - std::exp(-dtSeconds / tauSeconds);
    return std::clamp(a, 0.0, 1.0);
}

double AlphaBetaFilter::betaForTau(double tauSeconds, double dtSeconds)
{
    const double a = alphaForTau(tauSeconds, dtSeconds);
    const double denom = 2.0 - a;
    if (denom <= 0.0)
        return 1.0;
    return std::clamp((a * a) / denom, 0.0, 1.0);
}

void AlphaBetaFilter::observe(const Vec2& center, double dtSeconds)
{
    if (!initialized_)
    {
        estimate_ = center;
        velocity_ = Vec2{ 0.0, 0.0 };
        initialized_ = true;
        return;
    }

    double ticks = 0.0;
    const double tauSeconds = params_.tauMs * 0.001;
    if (dtSeconds <= 0.0)
    {
        const double a = 0.5;
        estimate_ = estimate_ + (center - estimate_) * a;
        return;
    }
    clampDtTicks(tauSeconds, dtSeconds, ticks);

    const Vec2 predicted = estimate_ + velocity_ * dtSeconds;

    const double a = alphaForTau(tauSeconds, dtSeconds);
    const double b = betaForTau(tauSeconds, dtSeconds);
    const Vec2 residual = center - predicted;

    estimate_ = predicted + residual * a;
    velocity_ = velocity_ + residual * (b / dtSeconds);
}

Vec2 AlphaBetaFilter::position() const
{
    return estimate_;
}

Vec2 AlphaBetaFilter::velocity() const
{
    return initialized_ ? velocity_ : Vec2{ 0.0, 0.0 };
}

void AlphaBetaFilter::reset()
{
    initialized_ = false;
    estimate_ = Vec2{ 0.0, 0.0 };
    velocity_ = Vec2{ 0.0, 0.0 };
}

}
