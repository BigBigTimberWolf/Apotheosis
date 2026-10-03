#ifndef CAPTURE_AUTO_CAPTURE_H
#define CAPTURE_AUTO_CAPTURE_H

#include <atomic>
#include <vector>
#include "control/types.h"

class GpuImage;
namespace cv { class Mat; }
namespace runtime { struct FrameContext; }

namespace AutoCapture
{

extern std::atomic<int>  g_saved_total;
extern std::atomic<int>  g_saved_session;
extern std::atomic<bool> g_force_held;
extern std::atomic<bool> g_running;

void auto_capture_thread();

void reset_session_counter();
// Keep the source frame until its inference result is published. GPU frames
// share their allocation and are downloaded only when a save is requested.
void submit_frame(const GpuImage& frame, runtime::FrameContext context);
void submit_frame(const cv::Mat& frame, runtime::FrameContext context);
void clear_frames();
// Only called after an automatic trigger down is accepted by the driver.
void notify_trigger(runtime::FrameContext context, std::vector<control::Candidate> detections);

}

#endif // CAPTURE_AUTO_CAPTURE_H
