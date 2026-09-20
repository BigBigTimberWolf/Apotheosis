#ifndef POSTPROCESS_H
#define POSTPROCESS_H

#include <chrono>
#include <vector>
#include <opencv2/opencv.hpp>

struct Detection
{
    cv::Rect box;
    float confidence;
    int classId;
    cv::Rect2f preciseBox;
};

void applyDeleteBucketFilter(std::vector<Detection>& detections);

struct SmallTargetDecode
{
    float  decodeFloor = 0.0f;
    float  baseConf = -1.0f;
    float  smallConf = -1.0f;
    double areaThreshPx = 0.0;
};

struct DetectorRuntimeSettings
{
    float confidenceThreshold = 0.25f;
    float nmsThreshold = 0.45f;
    int maxDetections = 100;
    int detectionResolution = 320;
};

DetectorRuntimeSettings detectorRuntimeSettings();

SmallTargetDecode computeSmallTargetDecode();

void capDetectionsToMax(std::vector<Detection>& detections, int maxDetections);

#endif // POSTPROCESS_H
