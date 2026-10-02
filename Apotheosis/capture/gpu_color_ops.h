#pragma once

#include <cuda_runtime.h>

void launch_motion_thumbnail(const unsigned char* src, size_t step, int width, int height,
    int channels, unsigned char* dst, size_t dstStep, int outWidth, int outHeight, cudaStream_t stream);

void launch_bgra_to_bgr_u8(
    const unsigned char* src, size_t srcStep,
    unsigned char* dst, size_t dstStep,
    int width, int height,
    cudaStream_t stream);

void launch_resize_bgr_u8_bilinear(
    const unsigned char* src, size_t srcStep, int srcW, int srcH,
    unsigned char* dst, size_t dstStep, int dstW, int dstH,
    cudaStream_t stream);

void launch_nv12_to_bgr_u8(
    const unsigned char* y, size_t yStep,
    const unsigned char* uv, size_t uvStep,
    unsigned char* bgr, size_t bgrStep,
    int width, int height,
    cudaStream_t stream);

// Matches OpenCV COLOR_YUV2BGR_NV12 (BT.601 studio/limited range).
void launch_nv12_to_bgr_bt601_limited_u8(
    const unsigned char* y, size_t yStep,
    const unsigned char* uv, size_t uvStep,
    unsigned char* bgr, size_t bgrStep,
    int width, int height,
    cudaStream_t stream);

void launch_yuv444_to_bgr_u8(
    const unsigned char* y,
    const unsigned char* u,
    const unsigned char* v,
    size_t stride,
    unsigned char* bgr, size_t bgrStep,
    int width, int height,
    cudaStream_t stream);

void launch_circle_mask_bgr_u8(
    unsigned char* img, size_t step,
    int width, int height,
    cudaStream_t stream);

struct GpuHsvBand
{
    int h_low, h_high;
    int s_min, s_max;
    int v_min, v_max;
};

namespace crosshair { struct CentroidComponent; }

void launch_crosshair_hsv_reduce_bgr_u8(
    const unsigned char* img, size_t step,
    int width, int height,
    int roi_x, int roi_y, int roi_w, int roi_h,
    const GpuHsvBand* bands, int band_count,
    int* result, unsigned long long* candidate_key,
    unsigned char* mask, unsigned char* scratch,
    int* component_labels, crosshair::CentroidComponent* components,
    int close_radius, int min_pixels, int reference_x, int reference_y, int algorithm,
    cudaStream_t stream);
