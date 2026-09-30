#include "anchor.h"

#include <algorithm>
#include <cmath>

namespace control {

namespace {

inline uint64_t mix64(uint64_t x)
{
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= x >> 33;
    return x;
}

inline double unitRandom(uint64_t seed)
{
    return static_cast<double>(mix64(seed) >> 11) * (1.0 / 9007199254740992.0);
}

}

Vec2 anchorFromOffset(const Vec2& filteredCenter, const Box& box, double yOffset)
{
    return Vec2{
        filteredCenter.x,
        filteredCenter.y + (0.5 - yOffset) * box.h
    };
}

Vec2 computeAnchor(const Vec2& filteredCenter, const Box& box,
                   const AimPointConfig& cfg, uint64_t sampleIndex)
{
    double lo = cfg.yOffset;
    double hi = cfg.yOffsetMax;
    if (hi < lo)
        std::swap(lo, hi);

    double offset = lo;
    if (hi > lo)
    {
        const uint64_t seed = (cfg.randomSeed != 0 ? cfg.randomSeed : 0x9E3779B97F4A7C15ULL)
                            + sampleIndex * 0xBF58476D1CE4E5B9ULL;
        offset = lo + (hi - lo) * unitRandom(seed);
    }

    double xLo = cfg.xOffset;
    double xHi = cfg.xOffsetMax;
    if (xHi < xLo)
        std::swap(xLo, xHi);
    double xOffset = xLo;
    if (xHi > xLo)
    {
        const uint64_t seed = (cfg.randomSeed != 0 ? cfg.randomSeed : 0x9E3779B97F4A7C15ULL)
                            + sampleIndex * 0xBF58476D1CE4E5B9ULL;
        xOffset = xLo + (xHi - xLo) * unitRandom(seed ^ 0xD6E8FEB86659FD93ULL);
    }

    Vec2 anchor = anchorFromOffset(filteredCenter, box, offset);
    anchor.x += (xOffset - 0.5) * box.w;
    return anchor;
}

}
