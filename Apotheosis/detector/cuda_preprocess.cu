#include <cuda_runtime.h>
#include <cuda_fp16.h>

#include "cuda_preprocess.h"

static __global__ void resize_bgr_u8_to_chw_rgb_f16_kernel(
    const unsigned char* __restrict__ src,
    int srcStepBytes, int srcW, int srcH, int srcChannels,
    __half* __restrict__ dst, int side)
{
    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    const int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= side || y >= side) return;

    const float scaleX = static_cast<float>(srcW) / static_cast<float>(side);
    const float scaleY = static_cast<float>(srcH) / static_cast<float>(side);
    const float fx = (static_cast<float>(x) + 0.5f) * scaleX - 0.5f;
    const float fy = (static_cast<float>(y) + 0.5f) * scaleY - 0.5f;

    const int x0 = max(0, min(srcW - 1, static_cast<int>(floorf(fx))));
    const int y0 = max(0, min(srcH - 1, static_cast<int>(floorf(fy))));
    const int x1 = min(srcW - 1, x0 + 1);
    const int y1 = min(srcH - 1, y0 + 1);
    const float ax = fx - floorf(fx);
    const float ay = fy - floorf(fy);

    const float w00 = (1.0f - ax) * (1.0f - ay);
    const float w01 = ax * (1.0f - ay);
    const float w10 = (1.0f - ax) * ay;
    const float w11 = ax * ay;

    const unsigned char* p00 = src + y0 * srcStepBytes + x0 * srcChannels;
    const unsigned char* p01 = src + y0 * srcStepBytes + x1 * srcChannels;
    const unsigned char* p10 = src + y1 * srcStepBytes + x0 * srcChannels;
    const unsigned char* p11 = src + y1 * srcStepBytes + x1 * srcChannels;

    constexpr float kInv255 = 1.0f / 255.0f;
    const int hw = side * side;
    const int idx = y * side + x;

    float r;
    float g;
    float b;
    if (srcChannels == 1)
    {
        const float v = (p00[0] * w00 + p01[0] * w01 + p10[0] * w10 + p11[0] * w11) * kInv255;
        r = v;
        g = v;
        b = v;
    }
    else
    {
        const float bv = (p00[0] * w00 + p01[0] * w01 + p10[0] * w10 + p11[0] * w11) * kInv255;
        const float gv = (p00[1] * w00 + p01[1] * w01 + p10[1] * w10 + p11[1] * w11) * kInv255;
        const float rv = (p00[2] * w00 + p01[2] * w01 + p10[2] * w10 + p11[2] * w11) * kInv255;
        r = rv;
        g = gv;
        b = bv;
    }

    dst[0 * hw + idx] = __float2half(r);
    dst[1 * hw + idx] = __float2half(g);
    dst[2 * hw + idx] = __float2half(b);
}

void launch_resize_bgr_u8_to_chw_rgb_f16(
    const GpuFrame& src,
    __half* dstChw,
    int side,
    cudaStream_t stream)
{
    if (src.empty()) return;
    const dim3 block(16, 16);
    const dim3 grid((side + block.x - 1) / block.x, (side + block.y - 1) / block.y);

    resize_bgr_u8_to_chw_rgb_f16_kernel<<<grid, block, 0, stream>>>(
        src.data, static_cast<int>(src.step),
        src.cols, src.rows, src.channels,
        dstChw, side
    );
}
