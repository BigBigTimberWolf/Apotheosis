#pragma once

#include "model_inspector.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace detector {

struct RawYoloCandidate {
    float x1 = 0, y1 = 0, x2 = 0, y2 = 0;
    float score = 0;
    int class_id = 0;
    int index = 0;
};

// Ultralytics raw detect output: [1, 4 + classes, boxes] or [1, boxes, 4 + classes].
// Reading directly from the pinned tensor avoids a full transpose or FP32 copy.
template<class Read>
void decode_raw_yolo(const std::vector<int64_t>& shape, ModelOutputKind kind,
                     Read&& read, float confidence, float scale,
                     float input_width, float input_height,
                     std::vector<RawYoloCandidate>& candidates)
{
    candidates.clear();
    const bool channels_first = kind == ModelOutputKind::RawChannelsFirst;
    if (!channels_first && kind != ModelOutputKind::RawRowsFirst) return;
    const int64_t boxes = channels_first ? shape[2] : shape[1];
    const int64_t channels = channels_first ? shape[1] : shape[2];
    const auto at = [&](int64_t channel, int64_t box) {
        const size_t offset = channels_first
            ? static_cast<size_t>(channel * boxes + box)
            : static_cast<size_t>(box * channels + channel);
        return read(offset);
    };

    for (int64_t box = 0; box < boxes; ++box)
    {
        float score = confidence;
        int best_class = -1;
        for (int64_t c = 4; c < channels; ++c)
        {
            const float value = at(c, box);
            if (value > score) { score = value; best_class = static_cast<int>(c - 4); }
        }
        if (best_class < 0 || !std::isfinite(score)) continue;

        const float cx = at(0, box), cy = at(1, box);
        const float width = at(2, box), height = at(3, box);
        if (!(std::isfinite(cx) && std::isfinite(cy)
              && std::isfinite(width) && std::isfinite(height)
              && width > 0 && height > 0)) continue;

        RawYoloCandidate candidate;
        candidate.x1 = std::clamp(cx - width * 0.5f, 0.0f, input_width) * scale;
        candidate.y1 = std::clamp(cy - height * 0.5f, 0.0f, input_height) * scale;
        candidate.x2 = std::clamp(cx + width * 0.5f, 0.0f, input_width) * scale;
        candidate.y2 = std::clamp(cy + height * 0.5f, 0.0f, input_height) * scale;
        if (!(candidate.x2 > candidate.x1 && candidate.y2 > candidate.y1)) continue;
        candidate.score = score;
        candidate.class_id = best_class;
        candidate.index = static_cast<int>(box);
        candidates.push_back(candidate);
    }
}

inline void nms_raw_yolo(std::vector<RawYoloCandidate>& candidates,
                         float iou_threshold, int max_detections,
                         std::vector<RawYoloCandidate>& selected)
{
    selected.clear();
    if (max_detections <= 0) return;
    std::sort(candidates.begin(), candidates.end(),
              [](const RawYoloCandidate& a, const RawYoloCandidate& b) {
                  return a.score == b.score ? a.index < b.index : a.score > b.score;
              });
    for (const auto& candidate : candidates)
    {
        bool suppressed = false;
        const float area = (candidate.x2 - candidate.x1) * (candidate.y2 - candidate.y1);
        for (const auto& kept : selected)
        {
            if (candidate.class_id != kept.class_id) continue;
            const float overlap_width = std::min(candidate.x2, kept.x2) - std::max(candidate.x1, kept.x1);
            if (overlap_width <= 0) continue;
            const float overlap_height = std::min(candidate.y2, kept.y2) - std::max(candidate.y1, kept.y1);
            if (overlap_height <= 0) continue;
            const float intersection = overlap_width * overlap_height;
            const float kept_area = (kept.x2 - kept.x1) * (kept.y2 - kept.y1);
            if (intersection > iou_threshold * (area + kept_area - intersection))
            {
                suppressed = true;
                break;
            }
        }
        if (!suppressed)
        {
            selected.push_back(candidate);
            if (selected.size() >= static_cast<size_t>(max_detections)) break;
        }
    }
}

} // namespace detector
