#include "selector.h"

#include <algorithm>
#include <cmath>

namespace control {

std::vector<size_t> filterAimCandidates(const std::vector<Candidate>& candidates,
                                        const ClassBuckets& buckets,
                                        const SelectorConfig& cfg)
{
    std::vector<size_t> out;
    out.reserve(candidates.size());
    for (size_t i = 0; i < candidates.size(); ++i)
    {
        if (!candidates[i].box.valid())
            continue;
        if (buckets.bucketOf(candidates[i].classId) != Bucket::Aim)
            continue;
        const double need = cfg.minConfOf(candidates[i].classId);
        if (need > 0.0 && candidates[i].confidence < need)
            continue;
        out.push_back(i);
    }
    return out;
}

namespace {

double distanceTo(const Candidate& c, const Vec2& cross)
{
    const Vec2 d = c.box.center() - cross;
    return d.norm();
}

}

TargetSelection selectTarget(const std::vector<Candidate>& candidates,
                             const std::vector<size_t>& aimIndices,
                             const Vec2& cross,
                             const SelectorConfig& cfg,
                             SelectorState& state)
{
    TargetSelection result;

    bool haveNearest = false;
    size_t nearestIdx = 0;
    double nearestDist = 0.0;

    for (size_t idx : aimIndices)
    {
        const Candidate& c = candidates[idx];
        const double d = distanceTo(c, cross);
        if (cfg.maxDistancePx > 0.0 && d > cfg.maxDistancePx)
            continue;
        if (!haveNearest || d < nearestDist)
        {
            haveNearest = true;
            nearestIdx = idx;
            nearestDist = d;
        }
    }

    if (!haveNearest)
    {
        state.reset();
        result.found = false;
        return result;
    }

    size_t chosenIdx = nearestIdx;
    if (state.locked)
    {
        bool lockedStillPresent = false;
        size_t lockedIdx = 0;
        double lockedDistToCross = 0.0;
        for (size_t idx : aimIndices)
        {
            const Candidate& c = candidates[idx];
            const double cd = (c.box.center() - state.lockedBox.center()).norm();
            const double lockDiag = state.lockedBox.diagonal();
            if (lockDiag <= 0.0) break;
            if (cd > lockDiag * 0.5) continue;

            const double areaA = c.box.area();
            const double areaB = state.lockedBox.area();
            if (areaA <= 0.0 || areaB <= 0.0) continue;
            const double ratio = areaA / areaB;
            if (ratio < 0.5 || ratio > 2.0) continue;

            lockedStillPresent = true;
            lockedIdx = idx;
            lockedDistToCross = distanceTo(c, cross);
            break;
        }

        if (lockedStillPresent)
        {
            if (nearestDist * cfg.hysteresisRatio < lockedDistToCross)
                chosenIdx = nearestIdx;
            else
                chosenIdx = lockedIdx;
        }
    }

    const Candidate& chosen = candidates[chosenIdx];
    result.found = true;
    result.index = chosenIdx;
    result.box = chosen.box;
    result.classId = chosen.classId;
    result.confidence = chosen.confidence;
    result.distancePx = distanceTo(chosen, cross);

    state.locked = true;
    state.lockedBox = chosen.box;
    state.lockedClassId = chosen.classId;
    ++state.lockedFrames;

    return result;
}

}
