#pragma once

#include <cuda_runtime.h>
#include <nvjpeg.h>

#include <cstddef>
#include <cstdint>

#include "../mem/gpu_image.h"

namespace capture {

class GpuJpegDecoder
{
public:
    GpuJpegDecoder();
    ~GpuJpegDecoder();

    GpuJpegDecoder(const GpuJpegDecoder&) = delete;
    GpuJpegDecoder& operator=(const GpuJpegDecoder&) = delete;

    bool init();
    bool ready() const { return initialized_; }

    bool decode(
        const uint8_t* data,
        size_t size,
        GpuImage& dst,
        cudaStream_t stream);

    bool decodeCropped(
        const uint8_t* data,
        size_t size,
        int cropW,
        int cropH,
        GpuImage& dst,
        cudaStream_t stream);

private:
    void cleanup();
    bool initRoi();
    void cleanupRoi();

    bool initialized_{ false };
    nvjpegHandle_t handle_{ nullptr };
    nvjpegJpegState_t state_{ nullptr };

    // 实际生效的后端。默认 GPU_HYBRID (GPU 辅助霍夫曼解码, 把熵解码从 CPU 搬到
    // GPU); 该后端不可用时降级为 DEFAULT。initRoi() 必须与 handle 用同一个后端。
    nvjpegBackend_t backend_{ NVJPEG_BACKEND_DEFAULT };

    static constexpr int ROI_RING = 3;
    bool roi_initialized_{ false };
    nvjpegJpegDecoder_t roi_decoder_{ nullptr };
    nvjpegDecodeParams_t roi_params_{ nullptr };
    nvjpegJpegState_t roi_state_[ROI_RING]{};
    nvjpegBufferPinned_t roi_pinned_[ROI_RING]{};
    nvjpegBufferDevice_t roi_device_[ROI_RING]{};
    nvjpegJpegStream_t roi_stream_[ROI_RING]{};
    cudaEvent_t roi_event_[ROI_RING]{};
    size_t roi_slot_{ 0 };
};

}
