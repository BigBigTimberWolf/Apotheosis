#pragma once

#include "types.h"

#include <vector>

namespace control {

enum class Bucket
{
    Aim,
    Filter,
    Delete,
};

struct ClassBuckets
{
    std::vector<Bucket> byClassId;

    Bucket bucketOf(int classId) const
    {
        if (classId < 0 || static_cast<size_t>(classId) >= byClassId.size())
            return Bucket::Delete;
        return byClassId[static_cast<size_t>(classId)];
    }
};

struct SelectorConfig
{
    double hysteresisRatio = 1.3;

    double maxDistancePx = 0.0;

    std::vector<double> minConfByClassId;

    double minConfOf(int classId) const
    {
        if (classId < 0 || static_cast<size_t>(classId) >= minConfByClassId.size())
            return 0.0;
        const double v = minConfByClassId[static_cast<size_t>(classId)];
        return v > 0.0 ? v : 0.0;
    }
};

struct SelectorState
{
    Box lockedBox;
    int lockedClassId = -1;
    bool locked = false;
    int lockedFrames = 0;

    void reset()
    {
        locked = false;
        lockedFrames = 0;
        lockedClassId = -1;
        lockedBox = Box{};
    }
};

struct TargetSelection
{
    bool found = false;
    size_t index = 0;
    Box box;
    int classId = -1;
    double confidence = 0.0;
    double distancePx = 0.0;
};

std::vector<size_t> filterAimCandidates(const std::vector<Candidate>& candidates,
                                        const ClassBuckets& buckets,
                                        const SelectorConfig& cfg);

TargetSelection selectTarget(const std::vector<Candidate>& candidates,
                             const std::vector<size_t>& aimIndices,
                             const Vec2& cross,
                             const SelectorConfig& cfg,
                             SelectorState& state);

}
