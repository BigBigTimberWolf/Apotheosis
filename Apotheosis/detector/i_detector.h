#ifndef I_DETECTOR_H
#define I_DETECTOR_H

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <string>
#include <vector>

#include <opencv2/opencv.hpp>

#include "postProcess.h"
#include "runtime/frame_context.h"
#include "../mem/gpu_image.h"

enum class DetectorBackend
{
    DirectML,
    TensorRT
};

class IDetector
{
public:
    virtual ~IDetector() = default;

    virtual DetectorBackend backend() const noexcept = 0;
    virtual const char* backendName() const noexcept = 0;

    virtual bool initialize(const std::string& model_path) = 0;

    virtual void processFrame(const cv::Mat& frame, runtime::FrameContext context = runtime::FrameContext{}) = 0;
    virtual void processFrameGpu(GpuImage  , runtime::FrameContext   = runtime::FrameContext{}) {}
    virtual void inferenceThread() = 0;

    virtual int numberOfClasses() const = 0;

    virtual std::vector<std::string> classNames() const = 0;

    virtual std::chrono::duration<double, std::milli> lastPreprocessTime() const = 0;
    virtual std::chrono::duration<double, std::milli> lastInferenceTime() const = 0;
    virtual std::chrono::duration<double, std::milli> lastCopyTime() const = 0;
    virtual std::chrono::duration<double, std::milli> lastPostprocessTime() const = 0;
    virtual std::chrono::duration<double, std::milli> lastNmsTime() const = 0;

    virtual void requestExit() = 0;

protected:
    IDetector() = default;

private:
    IDetector(const IDetector&) = delete;
    IDetector& operator=(const IDetector&) = delete;
    IDetector(IDetector&&) = delete;
    IDetector& operator=(IDetector&&) = delete;
};

#endif // I_DETECTOR_H
