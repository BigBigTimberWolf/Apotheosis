#pragma once

#include "filter.h"

namespace control {

inline constexpr double kAnchorFilterTauMs = 30.0;

struct AlphaBetaParams
{
    double tauMs = kAnchorFilterTauMs;
};

class AlphaBetaFilter : public IFilter
{
public:
    explicit AlphaBetaFilter(const AlphaBetaParams& params = {});

    void observe(const Vec2& center, double dtSeconds) override;
    Vec2 position() const override;
    Vec2 velocity() const override;
    bool initialized() const override { return initialized_; }
    void reset() override;

    static double alphaForTau(double tauSeconds, double dtSeconds);
    static double betaForTau(double tauSeconds, double dtSeconds);

private:
    AlphaBetaParams params_;
    Vec2 estimate_;
    Vec2 velocity_;
    bool initialized_ = false;
};

}
