#pragma once

#include "types.h"

namespace control {

struct AimPointConfig
{
    double yOffset = 0.5;
    double yOffsetMax = 0.5;
    uint64_t randomSeed = 0;
    double xOffset = 0.5;
    double xOffsetMax = 0.5;
};

// The caller keeps sampleIndex unchanged for the lifetime of a target lock.
Vec2 computeAnchor(const Vec2& filteredCenter, const Box& box,
                   const AimPointConfig& cfg, uint64_t sampleIndex);

Vec2 anchorFromOffset(const Vec2& filteredCenter, const Box& box, double yOffset);

}
