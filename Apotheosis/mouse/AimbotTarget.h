#ifndef AIMBOTTARGET_H
#define AIMBOTTARGET_H

#include <opencv2/opencv.hpp>

struct TrackDebugInfo
{
    int trackId = -1;
    int classId = -1;
    cv::Rect box;
    double pivotX = 0.0;
    double pivotY = 0.0;
    bool observedThisFrame = false;
    int missedFrames = 0;
    bool isLocked = false;
    float threat = 0.5f;
    float confidence = 0.0f;
    float depth_at_pivot = -1.0f;
};

#endif // AIMBOTTARGET_H
