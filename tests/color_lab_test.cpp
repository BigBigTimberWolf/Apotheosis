#include "crosshair/color_lab.h"

#include <cstdio>

int main() {
    using namespace crosshair;
    const auto wraps = deriveColorLabBands({{178, 210, 220}, {2, 215, 225}}, {});
    if (wraps.bands.size() != 2 ||
        !wraps.bands[0].contains({2, 215, 225}) ||
        !wraps.bands[1].contains({178, 210, 220})) return 1;

    const auto filtered = deriveColorLabBands(
        {{70, 200, 210}, {72, 205, 215}},
        {{71, 130, 210}, {115, 200, 210}});
    if (filtered.bands.empty() || filtered.background_hits != 0) return 2;
    for (const auto& sample : {ColorLabSample{70, 200, 210}, {72, 205, 215}}) {
        bool matched = false;
        for (const auto& band : filtered.bands) matched |= band.contains(sample);
        if (!matched) return 3;
    }
    const auto impossible = deriveColorLabBands({{70, 200, 210}}, {{70, 200, 210}});
    if (impossible.background_hits != 1) return 4;
    if (!deriveColorLabBands({}, {}).bands.empty()) return 5;
    std::puts("color lab samples and exclusions passed");
    return 0;
}
