#ifndef CAPTURE_H
#define CAPTURE_H

#include <opencv2/opencv.hpp>
#include <atomic>
#include <chrono>
#include <mutex>
#include <condition_variable>
#include <deque>

#include "../mem/gpu_image.h"

extern std::atomic<bool> detection_resolution_changed;
extern std::atomic<bool> capture_method_changed;
extern std::atomic<bool> capture_fps_changed;
extern std::deque<cv::Mat> frameQueue;

void captureThread(int CAPTURE_WIDTH, int CAPTURE_HEIGHT);
extern int screenWidth;
extern int screenHeight;

extern std::atomic<int> captureFrameCount;
extern std::atomic<int> captureFps;
extern std::atomic<int> captureSourceFps;
extern std::atomic<int> captureSenderSpanFps;
extern std::atomic<int> captureWireLostFps;
extern std::atomic<int> capturePartialLostFps;
extern std::atomic<int> capturePcapKernelDroppedFps;
extern std::atomic<int> capturePcapIfDroppedFps;
extern std::chrono::time_point<std::chrono::high_resolution_clock> captureFpsStartTime;

extern cv::Mat latestFrame;

extern std::mutex frameMutex;
extern std::condition_variable frameCV;
extern std::atomic<bool> shouldExit;

class IScreenCapture
{
public:
    virtual ~IScreenCapture() {}
    virtual cv::Mat GetNextFrameCpu() = 0;

    virtual GpuImage GetNextFrameGpu() { return GpuImage(); }
    virtual bool HasStopped() const { return false; }
    virtual int GetSourceFpsEstimate() const { return 0; }
    virtual int64_t GetLastFrameCaptureNs() const { return 0; }

    virtual int GetDeviceFrameAgeUs() const { return -1; }

    virtual bool WaitFrame(int  ) { return false; }

    virtual bool SupportsEventWait() const { return false; }

    virtual void SetTargetFps(int  ) {}

    virtual bool HandlesTargetFps() const { return false; }

    virtual int GetSenderSpanFps()          const { return 0; }
    virtual int GetWireLostFps()            const { return 0; }
    virtual int GetPartialLostFps()         const { return 0; }
    virtual int GetPcapKernelDroppedFps()   const { return 0; }
    virtual int GetPcapIfDroppedFps()       const { return 0; }
};

#endif // CAPTURE_H
