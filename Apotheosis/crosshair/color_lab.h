#pragma once

#include <algorithm>
#include <array>
#include <utility>
#include <vector>

namespace crosshair {

struct ColorLabSample {
    int h = 0;
    int s = 0;
    int v = 0;
};

struct ColorLabBand {
    int h_low = 0;
    int h_high = 179;
    int s_min = 0;
    int s_max = 255;
    int v_min = 0;
    int v_max = 255;

    bool contains(const ColorLabSample& sample) const {
        return sample.h >= h_low && sample.h <= h_high &&
            sample.s >= s_min && sample.s <= s_max &&
            sample.v >= v_min && sample.v <= v_max;
    }
};

struct ColorLabResult {
    std::vector<ColorLabBand> bands;
    int background_hits = 0;
};

inline ColorLabResult deriveColorLabBands(
    const std::vector<ColorLabSample>& positive,
    const std::vector<ColorLabSample>& negative)
{
    if (positive.empty()) return {};
    std::vector<int> hues;
    hues.reserve(positive.size());
    int minS = 255, maxS = 0, minV = 255, maxV = 0;
    for (const auto& sample : positive) {
        hues.push_back(std::clamp(sample.h, 0, 179));
        minS = std::min(minS, std::clamp(sample.s, 0, 255));
        maxS = std::max(maxS, std::clamp(sample.s, 0, 255));
        minV = std::min(minV, std::clamp(sample.v, 0, 255));
        maxV = std::max(maxV, std::clamp(sample.v, 0, 255));
    }
    std::sort(hues.begin(), hues.end());
    int widestGap = -1;
    size_t arcStartIndex = 0;
    for (size_t i = 0; i < hues.size(); ++i) {
        const int next = i + 1 < hues.size() ? hues[i + 1] : hues.front() + 180;
        const int gap = next - hues[i];
        if (gap > widestGap) {
            widestGap = gap;
            arcStartIndex = (i + 1) % hues.size();
        }
    }
    const int arcStart = hues[arcStartIndex];
    int arcEnd = arcStart;
    for (const int hue : hues)
        arcEnd = std::max(arcEnd, hue < arcStart ? hue + 180 : hue);

    auto makeBands = [&](int hueMargin, int svMargin) {
        ColorLabBand base;
        base.s_min = std::max(0, minS - svMargin);
        base.s_max = std::min(255, maxS + svMargin);
        base.v_min = std::max(0, minV - svMargin);
        base.v_max = std::min(255, maxV + svMargin);
        const int low = arcStart - hueMargin;
        const int high = arcEnd + hueMargin;
        if (high - low >= 179) {
            base.h_low = 0; base.h_high = 179;
            return std::vector<ColorLabBand>{base};
        }
        const int normalizedLow = ((low % 180) + 180) % 180;
        const int normalizedHigh = ((high % 180) + 180) % 180;
        if (normalizedLow <= normalizedHigh) {
            base.h_low = normalizedLow; base.h_high = normalizedHigh;
            return std::vector<ColorLabBand>{base};
        }
        ColorLabBand lowBand = base, highBand = base;
        lowBand.h_low = 0; lowBand.h_high = normalizedHigh;
        highBand.h_low = normalizedLow; highBand.h_high = 179;
        return std::vector<ColorLabBand>{lowBand, highBand};
    };

    ColorLabResult best;
    bool haveBest = false;
    int bestMargin = -1;
    for (const int hueMargin : std::array<int, 4>{0, 2, 3, 5}) {
        for (const int svMargin : std::array<int, 5>{0, 5, 10, 15, 25}) {
            auto bands = makeBands(hueMargin, svMargin);
            int hits = 0;
            for (const auto& sample : negative) {
                if (std::any_of(bands.begin(), bands.end(),
                                [&](const ColorLabBand& band) { return band.contains(sample); }))
                    ++hits;
            }
            const int margin = hueMargin * 5 + svMargin;
            if (!haveBest || hits < best.background_hits ||
                (hits == best.background_hits && margin > bestMargin)) {
                best.bands = std::move(bands);
                best.background_hits = hits;
                bestMargin = margin;
                haveBest = true;
            }
        }
    }
    return best;
}

} // namespace crosshair
