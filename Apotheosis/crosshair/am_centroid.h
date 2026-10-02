#pragma once
#include <cstdint>

#ifdef __CUDACC__
#define AM_CENTROID_HD __host__ __device__
#else
#define AM_CENTROID_HD
#endif

namespace crosshair {
struct AmHsv { int h, s, v; };
// Reimplemented from AM 1.0.30's 0x140009330 scalar color predicate:
// integer S, truncating H, and a wrapping hue interval. Shared by CPU/CUDA.
AM_CENTROID_HD inline AmHsv amHsv(int b, int g, int r)
{
    const int hi = r > g ? (r > b ? r : b) : (g > b ? g : b);
    const int lo = r < g ? (r < b ? r : b) : (g < b ? g : b);
    const int delta = hi - lo;
    const int saturation = hi ? delta * 255 / hi : 0;
    float hue = 0;
    if (saturation && delta) {
        if (hi == r) hue = float(g - b) / delta;
        else if (hi == g) hue = float(b - r) / delta + 2.0f;
        else hue = float(r - g) / delta + 4.0f;
        hue *= 60.0f;
        if (hue < 0) hue += 360.0f;
        hue *= 0.5f;
    }
    return {static_cast<int>(hue), saturation, hi};
}
template<class Band>
AM_CENTROID_HD inline bool amHsvMatches(AmHsv p, const Band& b)
{
    const bool hue = b.h_low <= b.h_high ? p.h >= b.h_low && p.h <= b.h_high
                                          : p.h >= b.h_low || p.h <= b.h_high;
    return hue && p.s >= b.s_min && p.s <= b.s_max && p.v >= b.v_min && p.v <= b.v_max;
}
// AM accepts any nonempty match set and truncates the global-coordinate mean.
AM_CENTROID_HD inline int amCentroidCoordinate(int64_t sum, int count)
{
    return count > 0 ? static_cast<int>(sum / count) : 0;
}
}
#undef AM_CENTROID_HD
