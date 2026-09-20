#pragma once

#include <cuda_runtime.h>
#include "gpu_ready_event.h"

#include <cstddef>
#include <cstdint>
#include <memory>

namespace cv { class Mat; }

struct GpuFrame
{
    unsigned char* data = nullptr;
    int rows = 0;
    int cols = 0;
    int channels = 0;
    size_t step = 0;

    bool empty() const noexcept { return data == nullptr || rows == 0 || cols == 0; }
};

class GpuImage
{
public:
    GpuImage() = default;
    ~GpuImage() = default;

    GpuImage(const GpuImage&) = default;
    GpuImage& operator=(const GpuImage&) = default;
    GpuImage(GpuImage&&) noexcept = default;
    GpuImage& operator=(GpuImage&&) noexcept = default;

    bool create(int rows, int cols, int channels);

    void release() noexcept;

    bool empty() const noexcept { return data_ == nullptr || rows_ == 0 || cols_ == 0; }
    int rows() const noexcept { return rows_; }
    int cols() const noexcept { return cols_; }
    int channels() const noexcept { return channels_; }
    size_t step() const noexcept { return step_; }
    unsigned char* data() noexcept { return data_; }
    const unsigned char* data() const noexcept { return data_; }

    bool upload(const unsigned char* src,
                int rows,
                int cols,
                int channels,
                size_t srcStep,
                cudaStream_t stream = nullptr);

    void download(cv::Mat& dst, cudaStream_t stream = nullptr) const;

    GpuImage subRect(int x, int y, int w, int h) const;

    GpuFrame view() const noexcept
    {
        return GpuFrame{ data_, rows_, cols_, channels_, step_ };
    }

    void setReadyEvent(std::shared_ptr<GpuReadyEvent> event) noexcept { ready_event_ = std::move(event); }
    cudaEvent_t readyEvent() const noexcept { return ready_event_ ? ready_event_->handle : nullptr; }

    void setCaptureNs(int64_t ns) { capture_ns_ = ns; }
    int64_t captureNs() const { return capture_ns_; }

    static uint64_t allocationCount() noexcept;
    static uint64_t allocationBytes() noexcept;

private:
    struct Storage
    {
        unsigned char* base = nullptr;
        size_t capacity = 0;
        ~Storage();
    };

    int64_t capture_ns_ = 0;
    std::shared_ptr<Storage> storage_;
    unsigned char* data_ = nullptr;
    int rows_ = 0;
    int cols_ = 0;
    int channels_ = 0;
    size_t step_ = 0;
    std::shared_ptr<GpuReadyEvent> ready_event_;
};
