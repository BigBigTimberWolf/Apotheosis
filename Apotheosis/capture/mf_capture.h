#ifndef MF_CAPTURE_H
#define MF_CAPTURE_H

#include "capture.h"
#include "gpu_jpeg_decoder.h"

#include <cuda_runtime.h>
#include <npp.h>
#include <opencv2/opencv.hpp>

#include "../mem/gpu_image.h"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

#include "capture_card_caps.h"
#include "device_frame_age.h"

class MFCapture : public IScreenCapture
{
public:
    enum class Format
    {
        Nv12,
        Mjpg,
        Yuy2,
        Rgb32
    };

    MFCapture(int src_width,
              int src_height,
              int out_side,
              bool crop_enabled,
              int capture_fps,
              const std::string& format,
              int device_index,
              bool gpu_decode);
    ~MFCapture() override;

    MFCapture(const MFCapture&) = delete;
    MFCapture& operator=(const MFCapture&) = delete;

    cv::Mat GetNextFrameCpu() override;
    GpuImage GetNextFrameGpu() override;
    int GetSourceFpsEstimate() const override { return source_fps_.load(); }

    int GetDeviceFrameAgeUs() const override;
    bool HasStopped() const override { return receive_finished_.load(); }
    int64_t GetLastFrameCaptureNs() const override { return last_dequeued_capture_ns_; }
    bool WaitFrame(int timeoutMs) override;
    bool SupportsEventWait() const override { return true; }
    void SetTargetFps(int fps) override;
    bool HandlesTargetFps() const override { return true; }

    bool IsOpen() const { return is_open_.load(); }

    const std::string& LastError() const { return open_error_; }

    static std::vector<MFDeviceInfo> EnumerateDevices();

    static std::vector<MFDeviceInfo> EnumerateDevicesWithCaps(int probe_index = -1);

    static bool ProbeCapabilities(MFDeviceInfo& dev);
    static Format ParseFormat(const std::string& s);
    static const char* FormatLabel(Format f);

private:
    void ReceiveThread();
    bool EnsureGpuContext();
    void TickFps();

    void StartProcessWorker();
    void StopProcessWorker();
    void ProcessWorkerLoop();
    void EnqueueProcessJob(const uint8_t* data, size_t size, int64_t capture_ns);

    void StartDecodeWorkers();
    void StopDecodeWorkers();
    void DecodeWorkerLoop();
    void EnqueueJpegJob(const uint8_t* data, size_t size, int64_t capture_ns);
    bool EnqueueGpuOrdered(GpuImage&& frame, uint64_t seq, int64_t capture_ns);
    bool ShouldDispatchFrame();

    bool PushNv12Gpu(const uint8_t* data, int width, int height, int stride);
    bool PushYuy2Gpu(const uint8_t* data, int width, int height, int stride);
    bool PushRgb32Gpu(const uint8_t* data, int width, int height, int stride);
    bool PushMjpgGpu(const uint8_t* data, size_t size);

    void ResolveRoi(int width, int height, int& left, int& top, int& roiW, int& roiH) const;
    GpuImage& nextOutSlot();
    GpuImage ResizeToOut(const GpuImage& src);

    bool PushNv12Cpu(const uint8_t* data, int width, int height, int stride);
    bool PushYuy2Cpu(const uint8_t* data, int width, int height, int stride);
    bool PushRgb32Cpu(const uint8_t* data, int width, int height, int stride);
    bool PushMjpgCpu(const uint8_t* data, size_t size);
    cv::Mat FinalizeCpu(cv::Mat bgr);

    void EnqueueCpu(cv::Mat&& frame);
    void EnqueueGpu(GpuImage&& frame);

    int src_width_;
    int src_height_;
    int out_side_;
    bool crop_enabled_;
    int capture_fps_;
    Format format_;
    int device_index_;
    bool gpu_decode_;

    std::atomic<bool> is_open_{ false };
    std::atomic<bool> should_stop_{ false };
    std::string open_error_;
    std::atomic<int> source_fps_{ 0 };
    std::atomic<int> negotiated_fps_{ 0 };
    int source_frame_count_{ 0 };
    double source_fps_smoothed_{ 0.0 };
    std::chrono::steady_clock::time_point source_fps_start_;

    capture::DeviceFrameAge device_age_;
    bool device_age_missing_logged_ = false;
    std::atomic<bool> receive_finished_{false};

    int frame_width_{ 0 };
    int frame_height_{ 0 };
    int frame_stride_{ 0 };

    std::thread receive_thread_;
    std::mutex frame_mutex_;
    std::condition_variable frame_cv_;
    struct CpuFrame { cv::Mat image; int64_t capture_ns = 0; };
    struct DeviceFrame { GpuImage image; int64_t capture_ns = 0; };
    std::queue<CpuFrame> cpu_frame_queue_;
    std::queue<DeviceFrame> gpu_frame_queue_;
    int64_t last_dequeued_capture_ns_ = 0;
    int64_t process_capture_ns_ = 0;

    struct ProcessJob
    {
        std::vector<uint8_t> bytes;
        int64_t capture_ns = 0;
        int width = 0;
        int height = 0;
        int stride = 0;
    };
    static constexpr int MAX_PROCESS_QUEUE = 1;
    std::thread process_thread_;
    std::mutex process_mutex_;
    std::condition_variable process_cv_;
    std::queue<ProcessJob> process_queue_;
    std::vector<std::vector<uint8_t>> process_free_buffers_;
    std::atomic<bool> process_stop_{ false };
    bool mjpg_cpu_fallback_{ false };

    struct DecodeJob { std::vector<uint8_t> jpeg; uint64_t seq = 0; int64_t capture_ns = 0; };
    // 实测 (build\diag\live_frame.jpg 924KB 真实帧, ROI 416, 本机 i5-4590 4 核,
    // 见 build\cuda\Release\decode_workers_bench.exe):
    //   worker 数    2 核 (0xC, 修复前)    4 核 (0xF, 修复后)
    //      1             ~120 fps              ~133 fps
    //      2             ~193 fps              ~236 fps
    //      3             ~187 fps              ~331 fps   <- 选这个
    //      4             ~190 fps              ~356 fps
    //   注意旧注释里"2 worker 合成约 3.0ms/帧"是在**无信号占位帧 (40KB)** 上测的, 不成立:
    //   真实帧单帧 host 就要 6.07ms。而且修复亲和性 bug 前进程只拿到 2 个核, 解码
    //   天花板约 190fps —— 这正是检测器实测只有 ~180fps 的原因。
    //   3 worker 在 4 核上到 331fps, 对 240fps 留 38% 余量, 同时给 TensorRT 推理与
    //   MF 读循环留下约 2 个核。
    static constexpr int DECODE_WORKERS = 3;
    // 深度必须 >= worker 数, 否则 worker 会空转; 但每加一层排队就多一份延迟
    // (实测: 3 -> 202fps/12.2ms, 6 -> 221fps/18.8ms)。4 层覆盖 3 个 worker, 且不白付延迟。
    static constexpr int MAX_JOB_QUEUE = 4;
    std::mutex job_mutex_;
    std::condition_variable job_cv_;
    std::queue<DecodeJob> job_queue_;
    // JPEG 缓冲回收池: 以前每帧 assign() 都重新 malloc ~1MB, 240fps 下是纯粹的
    // 分配器/缺页开销, 而且这笔开销压在读循环线程上。
    std::vector<std::vector<uint8_t>> job_free_buffers_;
    std::vector<std::thread> decode_workers_;
    std::atomic<bool> workers_stop_{ false };
    uint64_t job_seq_{ 0 };
    std::atomic<uint64_t> enqueued_seq_{ 0 };

    std::atomic<int> target_fps_{ 0 };
    std::chrono::steady_clock::time_point next_dispatch_{};

    cudaStream_t gpu_stream_{ nullptr };
    NppStreamContext npp_ctx_{};
    std::unique_ptr<capture::GpuJpegDecoder> gpu_decoder_;
    uint8_t* pinned_jpeg_buffer_{ nullptr };
    size_t pinned_jpeg_capacity_{ 0 };

    GpuImage scratch_a_;
    GpuImage scratch_b_;
    GpuImage scratch_full_;

    static constexpr int OUT_POOL_SIZE = 8;
    std::array<GpuImage, OUT_POOL_SIZE> out_pool_;
    GpuReadyEventPool<OUT_POOL_SIZE> out_events_;
    size_t out_pool_idx_{ 0 };
    size_t current_out_idx_{ 0 };

    static constexpr int MAX_QUEUE_SIZE = 1;
};

#endif // MF_CAPTURE_H
