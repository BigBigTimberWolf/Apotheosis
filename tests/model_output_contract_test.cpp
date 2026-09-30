#include "detector/raw_yolo_postprocess.h"

#include <cmath>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <vector>

static bool near(float a, float b) { return std::abs(a - b) < 0.001f; }

int main()
{
    using detector::ModelOutputKind;
    using detector::classify_model_output;
    if (classify_model_output({1, 100, 6}) != ModelOutputKind::End2End) return 1;
    if (classify_model_output({1, -1, 6}) != ModelOutputKind::End2End) return 2;
    if (classify_model_output({1, 9, 8400}) != ModelOutputKind::RawChannelsFirst) return 3;
    if (classify_model_output({1, 8400, 9}) != ModelOutputKind::RawRowsFirst) return 4;
    if (classify_model_output({1, 100, 5}) != ModelOutputKind::RawRowsFirst) return 5;

    // Two overlapping boxes of class 0; the same box of class 1 must survive class-aware NMS.
    const std::vector<int64_t> shape{1, 9, 8400};
    std::vector<float> data(9 * 8400, 0.0f);
    const auto put = [&](int channel, int box, float value) { data[channel * 8400 + box] = value; };
    for (int box = 0; box < 3; ++box) {
        put(0, box, 20.0f); put(1, box, 20.0f);
        put(2, box, 10.0f); put(3, box, 10.0f);
    }
    put(4, 0, 0.9f); put(4, 1, 0.8f); put(5, 2, 0.85f);
    std::vector<detector::RawYoloCandidate> candidates, kept;
    detector::decode_raw_yolo(shape, ModelOutputKind::RawChannelsFirst,
        [&](size_t i) { return data[i]; }, 0.25f, 2.0f, 640.0f, 640.0f, candidates);
    if (candidates.size() != 3 || !near(candidates[0].x1, 30.0f)
        || !near(candidates[0].x2, 50.0f)) return 6;
    detector::nms_raw_yolo(candidates, 0.5f, 20, kept);
    if (kept.size() != 2 || kept[0].class_id != 0 || kept[1].class_id != 1) return 7;

    const std::vector<int64_t> rowShape{1, 8, 7};
    std::vector<float> rows(8 * 7, 0.0f);
    rows[0] = 20.0f; rows[1] = 20.0f; rows[2] = 10.0f; rows[3] = 10.0f;
    rows[6] = 0.9f;
    detector::decode_raw_yolo(rowShape, ModelOutputKind::RawRowsFirst,
        [&](size_t i) { return rows[i]; }, 0.25f, 1.0f, 640.0f, 640.0f, candidates);
    if (candidates.size() != 1 || candidates[0].class_id != 2) return 8;

    if (std::getenv("APOTHEOSIS_BENCH_RAW_YOLO")) {
        const auto measure = [&](const char* label) {
            const auto begin = std::chrono::steady_clock::now();
            for (int frame = 0; frame < 1000; ++frame) {
                detector::decode_raw_yolo(shape, ModelOutputKind::RawChannelsFirst,
                    [&](size_t i) { return data[i]; }, 0.25f, 2.0f, 640.0f, 640.0f, candidates);
                detector::nms_raw_yolo(candidates, 0.5f, 20, kept);
            }
            const double us = std::chrono::duration<double, std::micro>(
                std::chrono::steady_clock::now() - begin).count() / 1000.0;
            std::cout << label << ": " << us << " us/frame (synthetic float input)\n";
        };
        measure("sparse");
        for (int box = 3; box < 503; ++box) {
            put(0, box, 20.0f + static_cast<float>((box % 20) * 25));
            put(1, box, 20.0f + static_cast<float>((box / 20) * 20));
            put(2, box, 10.0f); put(3, box, 10.0f);
            put(4, box, 0.3f + static_cast<float>(box % 10) * 0.01f);
        }
        measure("500 candidates");
    }
    return 0;
}
