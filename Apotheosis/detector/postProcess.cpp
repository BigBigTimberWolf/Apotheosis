#include <algorithm>
#include <cmath>
#include <numeric>
#include <chrono>
#include <limits>

#include "postProcess.h"
#include "Apotheosis.h"
#include "trt_detector.h"
#include "runtime/config_snapshot.h"

void applyDeleteBucketFilter(std::vector<Detection>& detections)
{
    if (detections.empty())
        return;

    const auto snapshot = runtime_config::read();
    const auto& filters = snapshot->class_filters;

    detections.erase(
        std::remove_if(detections.begin(), detections.end(),
            [&filters](const Detection& d) {
                return std::any_of(filters.begin(), filters.end(),
                    [&d](const auto& filter) {
                        return filter.bucket == ClassBucket::Delete
                            && filter.class_id == d.classId;
                    });
            }),
        detections.end());
}

DetectorRuntimeSettings detectorRuntimeSettings()
{
    DetectorRuntimeSettings out;
    const auto snapshot = runtime_config::read();
    out.confidenceThreshold = snapshot->confidence_threshold;
    if (snapshot->auto_capture_enabled)
    {
        // The capture thresholds must be able to see detections that the aim
        // threshold would otherwise discard. The aim loop applies its own
        // original threshold before selecting a target.
        if (snapshot->auto_capture_use_high)
            out.confidenceThreshold = std::min(out.confidenceThreshold,
                std::nextafter(snapshot->auto_capture_high_conf, 0.0f));
        if (snapshot->auto_capture_use_low && snapshot->auto_capture_low_conf > 0.0f)
            out.confidenceThreshold = std::min(out.confidenceThreshold,
                std::nextafter(snapshot->auto_capture_low_conf, 0.0f));
    }
    out.nmsThreshold = snapshot->nms_threshold;
    out.maxDetections = kFixedMaxDetections;
    out.detectionResolution = snapshot->detection_resolution;
    return out;
}

SmallTargetDecode computeSmallTargetDecode()
{
    SmallTargetDecode out;
    float base = 0.25f;
    bool enabled = false;
    float small_conf = 0.15f;
    int resolution = 320;
    float area_frac = 0.0025f;
    const auto snapshot = runtime_config::read();
    base = detectorRuntimeSettings().confidenceThreshold;
    enabled = snapshot->small_target_enabled;
    small_conf = snapshot->small_target_confidence;
    resolution = snapshot->detection_resolution;
    area_frac = snapshot->small_target_area_frac;
    if (!enabled)
    {
        out.decodeFloor = base;
        out.baseConf = -1.0f;
        out.smallConf = -1.0f;
        out.areaThreshPx = 0.0;
        return out;
    }
    const float smallConf = small_conf;
    const double res = static_cast<double>(resolution);
    out.decodeFloor = std::min(base, smallConf);
    out.baseConf = base;
    out.smallConf = smallConf;
    out.areaThreshPx = static_cast<double>(area_frac) * res * res;
    return out;
}

void capDetectionsToMax(std::vector<Detection>& detections, int maxDetections)
{
    if (maxDetections <= 0)
        return;
    if (detections.size() <= static_cast<size_t>(maxDetections))
        return;
    std::nth_element(
        detections.begin(),
        detections.begin() + maxDetections,
        detections.end(),
        [](const Detection& a, const Detection& b) { return a.confidence > b.confidence; });
    detections.resize(static_cast<size_t>(maxDetections));
}
