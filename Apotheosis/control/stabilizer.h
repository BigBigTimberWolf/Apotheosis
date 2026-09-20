#pragma once

#include "types.h"

namespace control {

struct StabilizerConfig
{
    double matchCenterRatio = 0.5;
    double areaRatioTol = 2.0;

    double kSnapMult = 1.15;

    double minAspect = 0.2;
    double maxAspect = 5.0;
};

enum class StabilizerVerdict
{
    Ok,
    NoHistory,
    Snap,
    Rejected,
};

struct StabilizerState
{
    Box lastBox;
    bool hasLast = false;

    void reset()
    {
        hasLast = false;
        lastBox = Box{};
    }
};

struct StabilizerResult
{
    StabilizerVerdict verdict = StabilizerVerdict::NoHistory;
    Box box;
    bool accepted = false;
};

StabilizerResult stabilize(const Candidate& candidate,
                           const StabilizerConfig& cfg,
                           StabilizerState& state);

bool aspectRatioPlausible(const Box& box, const StabilizerConfig& cfg);

}
