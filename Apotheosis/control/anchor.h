#pragma once

#include "types.h"

namespace control {

struct AimPointConfig
{
    double yOffset = 0.5;
    double yOffsetMax = 0.5;
    uint64_t randomSeed = 0;
};

Vec2 computeAnchor(const Vec2& filteredCenter, const Box& box,
                   const AimPointConfig& cfg, uint64_t frameIndex);

Vec2 anchorFromOffset(const Vec2& filteredCenter, const Box& box, double yOffset);

}
