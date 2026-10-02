#ifndef CROSSHAIR_RUNTIME_H
#define CROSSHAIR_RUNTIME_H

#include <atomic>
#include <chrono>
#include <mutex>

#include <opencv2/opencv.hpp>

#include "crosshair_detector.h"

class GpuImage;

namespace crosshair_runtime
{

struct PivotSnapshot
{
    double x = 0.0;
    double y = 0.0;
    std::chrono::steady_clock::time_point ts{};
    bool valid = false;
    int active_hotkey = -1;
};

inline constexpr int kFreshnessMs = 20;

PivotSnapshot read();
void publish(const PivotSnapshot& snap);

PivotSnapshot read_static_ref();
void publish_static_ref(const PivotSnapshot& ref);

PivotSnapshot process_frame(const cv::Mat& bgrFrame, int64_t captured_ns = 0,
                            bool crosshair_only = false);

// Both original and centroid algorithms run on the retained inference image,
// whether that image resides on the GPU or CPU.
bool same_frame_crosshair_active();
bool cpu_path_active();
PivotSnapshot process_gpu_frame(const GpuImage& bgrFrame);

}

#endif // CROSSHAIR_RUNTIME_H
