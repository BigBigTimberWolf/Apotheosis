#pragma once
#include "runtime/frame_context.h"
#include <algorithm>
#include <chrono>
#include <vector>
#include <mutex>
#include <condition_variable>
#include <opencv2/opencv.hpp>

struct DetectionBuffer
{
    std::mutex mutex;
    std::condition_variable cv;
    int version = 0;
    std::vector<cv::Rect> boxes;
    std::vector<cv::Rect2f> precise_boxes;
    std::vector<int> classes;
    std::vector<float> confidences;

    std::chrono::steady_clock::time_point stamp{};
    double last_interval_ms = 0.0;

    int64_t frame_stamp_ns = 0;
    runtime::FrameContext frame_context;

    void bumpVersionLocked(int64_t frame_capture_ns = 0)
    {
        const auto now = std::chrono::steady_clock::now();
        if (version > 0 && stamp.time_since_epoch().count() != 0)
            last_interval_ms =
                std::chrono::duration<double, std::milli>(now - stamp).count();
        stamp = now;
        frame_stamp_ns = frame_capture_ns;
        frame_context = {0, frame_capture_ns, 0, 0};
        ++version;
    }

    void bumpVersionLocked(runtime::FrameContext context)
    {
        bumpVersionLocked(context.captured_ns);
        frame_context = context;
    }

    bool staleLocked() const
    {
        if (version <= 1 || last_interval_ms <= 0.0)
            return false;
        const double thresholdMs =
            std::clamp(2.0 * last_interval_ms, 50.0, 600.0);
        const double ageMs = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - stamp).count();
        return ageMs > thresholdMs;
    }

    void set(const std::vector<cv::Rect>& newBoxes,
             const std::vector<int>& newClasses,
             const std::vector<float>& newConfidences)
    {
        std::lock_guard<std::mutex> lock(mutex);
        boxes = newBoxes;
        precise_boxes.clear();
        precise_boxes.reserve(newBoxes.size());
        for (const auto& box : newBoxes)
            precise_boxes.emplace_back(
                static_cast<float>(box.x), static_cast<float>(box.y),
                static_cast<float>(box.width), static_cast<float>(box.height));
        classes = newClasses;
        confidences = newConfidences;
        bumpVersionLocked();
        cv.notify_all();
    }

    void get(std::vector<cv::Rect>& outBoxes,
             std::vector<int>& outClasses,
             std::vector<float>& outConfidences,
             int& outVersion)
    {
        std::lock_guard<std::mutex> lock(mutex);
        outBoxes = boxes;
        outClasses = classes;
        outConfidences = confidences;
        outVersion = version;
    }
};
