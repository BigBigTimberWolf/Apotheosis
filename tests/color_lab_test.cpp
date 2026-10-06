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

    // 背景色落在目标样本的 S/V 框中间时，必须把它挖出去（旧实现只能在外面缩，
    // 缩不掉就只能让人手动微调，实机表现就是“经常锁背景”）。
    const std::vector<ColorLabSample> spreadTargets = {{60, 150, 200}, {62, 250, 240}};
    const auto carved = deriveColorLabBands(spreadTargets, {{61, 200, 220}});
    if (carved.background_hits != 0 || carved.bands.size() < 2) return 6;
    for (const auto& sample : spreadTargets) {
        bool matched = false;
        for (const auto& band : carved.bands) matched |= band.contains(sample);
        if (!matched) return 7;
    }

    // 点歪一格采到准星边缘混色像素时，它不能把整个范围撑大。
    const auto trimmed = deriveColorLabBands(
        {{60, 200, 220}, {61, 205, 225}, {59, 195, 215}, {60, 120, 90}}, {});
    if (trimmed.dropped_samples != 1) return 8;
    for (const auto& band : trimmed.bands) {
        if (band.s_min < 150 || band.v_min < 150) return 9;
    }

    // 同分时取更窄的那组：单个样本不应该被无谓地放宽。
    const auto tight = deriveColorLabBands({{60, 200, 220}}, {});
    if (tight.bands.size() != 1 || tight.bands[0].h_low != 60 || tight.bands[0].h_high != 60 ||
        tight.bands[0].s_min != 200 || tight.bands[0].v_min != 220) return 10;

    std::puts("color lab samples and exclusions passed");
    return 0;
}
