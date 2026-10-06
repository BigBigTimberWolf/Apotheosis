#pragma once

#include <algorithm>
#include <array>
#include <utility>
#include <vector>

namespace crosshair {

// 检测端一次最多消费这么多条颜色档（GPU 路径的固定预算，见
// crosshair_runtime.cpp）。超过的档不会生效，所以实验室在“加入颜色列表”前
// 用它提示用户：拆段 + 已有档别越过这个上限。
inline constexpr int kMaxColorBands = 16;

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
    // 负样本（背景色）里仍然被范围命中的数量。0 表示这套范围排掉了所有采过的背景色。
    int background_hits = 0;
    // 被当成离群点丢掉的目标样本数（点到了准星边缘混色像素、或点到别的东西）。
    int dropped_samples = 0;
    // 为了挖掉背景色把范围拆成的段数（1 表示没拆过）。
    int carved_bands = 0;
};

namespace detail {

// 一次点击只采一个像素时，点歪一格就会采到准星边缘的抗锯齿混色像素：色相可能
// 偏出主色一大截、亮度也会掉很多。这类样本会把“最小外接矩形”整体撑大，矩形一
// 大就会开始圈背景 —— 实机表现就是“经常锁背景”。这里先把它们剔掉。
constexpr int kHueClusterGap = 20;   // 色相间隔超过它就算另一簇
constexpr int kOutlierSvDelta = 70;  // 与中位 S/V 差超过它算离群
constexpr int kCarveGuard = 3;       // 挖洞时留的余量，防止抗锯齿像素从缝里漏回来
// 检测端（GPU 路径）最多只吃 16 条颜色档，挖洞拆出来的段数是有限的：
// 拆到这个数就不再把剩下的背景色硬拆，它们会照旧计入“背景误命中”提示。
constexpr int kMaxCarvedBands = 8;

inline int normalizeHue(int hue) { return ((hue % 180) + 180) % 180; }

// 保留最大的一簇色相；比最大簇小、而且自己只有一个样本的簇算离群点。
inline std::vector<ColorLabSample> keepDominantHueCluster(
    const std::vector<ColorLabSample>& samples, int& dropped)
{
    if (samples.size() <= 1) return samples;
    std::vector<int> hues;
    hues.reserve(samples.size());
    for (const auto& s : samples) hues.push_back(normalizeHue(s.h));
    std::sort(hues.begin(), hues.end());

    // 按循环间隔切簇。
    std::vector<std::vector<int>> clusters;
    for (size_t i = 0; i < hues.size(); ++i) {
        if (clusters.empty()) { clusters.push_back({hues[i]}); continue; }
        if (hues[i] - clusters.back().back() > kHueClusterGap) clusters.push_back({hues[i]});
        else clusters.back().push_back(hues[i]);
    }
    if (clusters.size() > 1) {
        // 首尾两簇在色环上其实是相邻的：间隔按 +180 算。
        const int wrapGap = clusters.front().front() + 180 - clusters.back().back();
        if (wrapGap <= kHueClusterGap) {
            clusters.front().insert(clusters.front().begin(),
                                    clusters.back().begin(), clusters.back().end());
            clusters.pop_back();
        }
    }

    size_t best = 0;
    for (size_t i = 1; i < clusters.size(); ++i)
        if (clusters[i].size() > clusters[best].size()) best = i;
    const size_t largest = clusters[best].size();

    std::vector<ColorLabSample> kept;
    kept.reserve(samples.size());
    for (const auto& sample : samples) {
        bool inDroppedCluster = false;
        for (size_t i = 0; i < clusters.size(); ++i) {
            if (i == best) continue;
            // 双色准星（比如红+青各采两次）两簇一样大，都要保留；
            // 只有“比最大簇小、且只有自己一个点”的簇才当误点丢掉。
            if (clusters[i].size() >= largest || clusters[i].size() >= 2) continue;
            if (std::find(clusters[i].begin(), clusters[i].end(), normalizeHue(sample.h)) !=
                clusters[i].end()) inDroppedCluster = true;
        }
        if (inDroppedCluster) ++dropped;
        else kept.push_back(sample);
    }
    return kept.empty() ? samples : kept;
}

// S/V 也剔离群：至少 3 个样本才做，并且保证至少留 2 个。
inline std::vector<ColorLabSample> trimSvOutliers(
    const std::vector<ColorLabSample>& samples, int& dropped)
{
    if (samples.size() < 3) return samples;
    std::vector<int> sv, vv;
    sv.reserve(samples.size());
    vv.reserve(samples.size());
    for (const auto& s : samples) { sv.push_back(s.s); vv.push_back(s.v); }
    const auto median = [](std::vector<int>& values) {
        const size_t mid = values.size() / 2;
        std::nth_element(values.begin(), values.begin() + mid, values.end());
        return values[mid];
    };
    const int medS = median(sv);
    const int medV = median(vv);

    std::vector<ColorLabSample> kept;
    int removed = 0;
    for (const auto& sample : samples) {
        const bool outlier = std::abs(sample.s - medS) > kOutlierSvDelta ||
                             std::abs(sample.v - medV) > kOutlierSvDelta;
        if (outlier && samples.size() - removed > 2) { ++removed; continue; }
        kept.push_back(sample);
    }
    dropped += removed;
    return kept;
}

inline std::vector<ColorLabBand> makeBands(const std::vector<ColorLabSample>& samples,
                                           int hueMargin, int svMargin)
{
    int minS = 255, maxS = 0, minV = 255, maxV = 0;
    std::vector<int> hues;
    hues.reserve(samples.size());
    for (const auto& sample : samples) {
        hues.push_back(normalizeHue(sample.h));
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
    const int normalizedLow = normalizeHue(low);
    const int normalizedHigh = normalizeHue(high);
    if (normalizedLow <= normalizedHigh) {
        base.h_low = normalizedLow; base.h_high = normalizedHigh;
        return std::vector<ColorLabBand>{base};
    }
    ColorLabBand lowBand = base, highBand = base;
    lowBand.h_low = 0; lowBand.h_high = normalizedHigh;
    highBand.h_low = normalizedLow; highBand.h_high = 179;
    return std::vector<ColorLabBand>{lowBand, highBand};
}

inline bool coversAll(const std::vector<ColorLabBand>& bands,
                      const std::vector<ColorLabSample>& samples)
{
    for (const auto& sample : samples) {
        bool matched = false;
        for (const auto& band : bands) matched |= band.contains(sample);
        if (!matched) return false;
    }
    return true;
}

// 把一条范围拆开，让 `sample` 不再被命中，同时保证每个正样本仍然被覆盖。
// 先在 S 轴上挖，不行再试 V 轴；两个轴都挖不动（背景色夹在正样本中间）就原样返回。
inline std::vector<ColorLabBand> carveBand(const ColorLabBand& band,
                                           const ColorLabSample& sample,
                                           const std::vector<ColorLabSample>& positives)
{
    if (!band.contains(sample)) return {band};
    for (int axis = 0; axis < 2; ++axis) {
        ColorLabBand lower = band, upper = band;
        if (axis == 0) {
            const int cut = std::clamp(sample.s, band.s_min, band.s_max);
            lower.s_max = cut - kCarveGuard;
            upper.s_min = cut + kCarveGuard;
        } else {
            const int cut = std::clamp(sample.v, band.v_min, band.v_max);
            lower.v_max = cut - kCarveGuard;
            upper.v_min = cut + kCarveGuard;
        }
        std::vector<ColorLabBand> parts;
        if (lower.s_min <= lower.s_max && lower.v_min <= lower.v_max) parts.push_back(lower);
        if (upper.s_min <= upper.s_max && upper.v_min <= upper.v_max) parts.push_back(upper);
        if (parts.empty() || !coversAll(parts, positives)) continue;
        // 只保留真的有用（至少覆盖一个正样本）的那一侧。
        std::vector<ColorLabBand> useful;
        for (const auto& part : parts) {
            for (const auto& positive : positives) {
                if (part.contains(positive)) { useful.push_back(part); break; }
            }
        }
        if (useful.empty() || !coversAll(useful, positives)) continue;
        return useful;
    }
    return {band};
}

} // namespace detail

// 从"目标色样本 + 背景色样本"推出可以直接填进准星/镭射颜色档的 HSV 范围。
//
// 1. 先剔掉误点出来的离群样本（准星边缘混色像素最容易把范围撑大）；
// 2. 取盖住剩余样本的色相弧 + S/V 最小外接框，余量在候选里挑"误命中最少、
//    其次最窄"的那组（旧实现同分时反而挑最宽的那组，等于主动放宽）；
// 3. 对仍然落在框里的背景样本，把这个框沿 S（或 V）挖开成两段 —— 以前它只能
//    在外面缩，缩不掉就报"建议手动微调"，也就是"经常锁背景"的来源。
inline ColorLabResult deriveColorLabBands(
    const std::vector<ColorLabSample>& positive,
    const std::vector<ColorLabSample>& negative)
{
    ColorLabResult result;
    if (positive.empty()) return result;

    int dropped = 0;
    auto kept = detail::keepDominantHueCluster(positive, dropped);
    kept = detail::trimSvOutliers(kept, dropped);
    result.dropped_samples = dropped;

    ColorLabResult best;
    bool haveBest = false;
    int bestMargin = 0;
    for (const int hueMargin : std::array<int, 4>{0, 2, 3, 5}) {
        for (const int svMargin : std::array<int, 5>{0, 5, 10, 15, 25}) {
            auto bands = detail::makeBands(kept, hueMargin, svMargin);
            int hits = 0;
            for (const auto& sample : negative) {
                if (std::any_of(bands.begin(), bands.end(),
                                [&](const ColorLabBand& band) { return band.contains(sample); }))
                    ++hits;
            }
            const int margin = hueMargin * 5 + svMargin;
            // 同分时取更窄的那组：宁可窄一点漏几帧，也不要宽一点天天圈背景。
            if (!haveBest || hits < best.background_hits ||
                (hits == best.background_hits && margin < bestMargin)) {
                best.bands = std::move(bands);
                best.background_hits = hits;
                bestMargin = margin;
                haveBest = true;
            }
        }
    }
    if (!haveBest) return result;

    // 挖掉还落在范围里的背景色。
    for (const auto& sample : negative) {
        if (static_cast<int>(best.bands.size()) >= detail::kMaxCarvedBands) break;
        std::vector<ColorLabBand> next;
        for (const auto& band : best.bands) {
            auto parts = detail::carveBand(band, sample, kept);
            next.insert(next.end(), parts.begin(), parts.end());
        }
        best.bands = std::move(next);
    }

    result.bands = std::move(best.bands);
    result.carved_bands = static_cast<int>(result.bands.size());
    int hits = 0;
    for (const auto& sample : negative) {
        if (std::any_of(result.bands.begin(), result.bands.end(),
                        [&](const ColorLabBand& band) { return band.contains(sample); }))
            ++hits;
    }
    result.background_hits = hits;
    return result;
}

} // namespace crosshair
