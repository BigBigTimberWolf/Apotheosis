#pragma once
#include <cuda_runtime.h>
#include <cuda_fp16.h>

#include "../mem/gpu_image.h"

void launch_resize_bgr_u8_to_chw_rgb_f16(
    const GpuFrame& src,
    __half* dstChw,
    int side,
    cudaStream_t stream
);
