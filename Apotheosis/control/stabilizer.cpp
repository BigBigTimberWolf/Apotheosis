#include "stabilizer.h"

#include <algorithm>
#include <cmath>

namespace control {

bool aspectRatioPlausible(const Box& box, const StabilizerConfig& cfg)
{
    if (!box.valid())
        return false;
    const double aspect = box.w / box.h;
    return aspect >= cfg.minAspect && aspect <= cfg.maxAspect;
}

StabilizerResult stabilize(const Candidate& candidate,
                           const StabilizerConfig& cfg,
                           StabilizerState& state)
{
    StabilizerResult result;

    if (!aspectRatioPlausible(candidate.box, cfg))
    {
        result.verdict = StabilizerVerdict::Rejected;
        result.accepted = false;
        return result;
    }

    result.box = candidate.box;
    result.accepted = true;

    if (!state.hasLast)
    {
        result.verdict = StabilizerVerdict::NoHistory;
        state.lastBox = candidate.box;
        state.hasLast = true;
        return result;
    }

    const double centerDist = (candidate.box.center() - state.lastBox.center()).norm();
    const double lastDiag = state.lastBox.diagonal();

    const double areaA = candidate.box.area();
    const double areaB = state.lastBox.area();
    const double ratio = (areaB > 0.0) ? (areaA / areaB) : 0.0;
    const bool sizeOk = ratio >= (1.0 / cfg.areaRatioTol) && ratio <= cfg.areaRatioTol;

    const bool snapped = (centerDist > lastDiag * cfg.kSnapMult) || !sizeOk;

    if (snapped)
    {
        result.verdict = StabilizerVerdict::Snap;
    }
    else
    {
        const bool nearEnough = (centerDist <= lastDiag * cfg.matchCenterRatio);
        result.verdict = nearEnough ? StabilizerVerdict::Ok : StabilizerVerdict::Snap;
    }

    state.lastBox = candidate.box;
    state.hasLast = true;

    return result;
}

}
