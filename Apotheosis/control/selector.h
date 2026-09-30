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

}
