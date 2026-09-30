#include "capture/gpu_color_ops.h"

#include <cuda_runtime.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {

void check(cudaError_t err)
{
    if (err != cudaSuccess) {
        std::fprintf(stderr, "CUDA: %s\n", cudaGetErrorString(err));
        std::exit(1);
    }
}

struct Result { int count, x, y; };

Result detect(const std::vector<unsigned char>& image, int side,
              int roi_x, int roi_y, int roi_w, int roi_h, int close_radius,
              int reference_x = 160, int reference_y = 160,
              int min_pixels = 4)
{
    unsigned char *device_image = nullptr, *mask = nullptr, *scratch = nullptr;
    GpuHsvBand* bands = nullptr;
    int* result = nullptr;
    unsigned long long* key = nullptr;
    check(cudaMalloc(reinterpret_cast<void**>(&device_image), image.size()));
    check(cudaMalloc(reinterpret_cast<void**>(&mask), 512 * 512));
    check(cudaMalloc(reinterpret_cast<void**>(&scratch), 512 * 512));
    check(cudaMalloc(reinterpret_cast<void**>(&bands), sizeof(GpuHsvBand)));
    check(cudaMalloc(reinterpret_cast<void**>(&result), 4 * sizeof(int)));
    check(cudaMalloc(reinterpret_cast<void**>(&key), sizeof(unsigned long long)));

    const GpuHsvBand red{0, 10, 80, 255, 80, 255};
    check(cudaMemcpy(device_image, image.data(), image.size(), cudaMemcpyHostToDevice));
    check(cudaMemcpy(bands, &red, sizeof(red), cudaMemcpyHostToDevice));
    check(cudaMemset(result, 0, 4 * sizeof(int)));
    check(cudaMemset(key, 0, sizeof(unsigned long long)));
    launch_crosshair_hsv_reduce_bgr_u8(
        device_image, static_cast<size_t>(side) * 3, side, side,
        roi_x, roi_y, roi_w, roi_h, bands, 1, result, key, mask, scratch,
        close_radius, min_pixels, reference_x, reference_y, nullptr);
    check(cudaGetLastError());
    int out[4]{};
    check(cudaMemcpy(out, result, sizeof(out), cudaMemcpyDeviceToHost));

    check(cudaFree(key));
    check(cudaFree(result));
    check(cudaFree(bands));
    check(cudaFree(scratch));
    check(cudaFree(mask));
    check(cudaFree(device_image));
    return {out[1], out[2], out[3]};
}

void red_pixel(std::vector<unsigned char>& image, int side, int x, int y)
{
    const size_t p = (static_cast<size_t>(y) * side + x) * 3;
    image[p] = 0;
    image[p + 1] = 0;
    image[p + 2] = 255;
}

}

int main()
{
    constexpr int side = 320;
    std::vector<unsigned char> image(static_cast<size_t>(side) * side * 3, 0);
    for (int y = 148; y <= 152; ++y)
        for (int x = 148; x <= 152; ++x)
            red_pixel(image, side, x, y);
    Result hit = detect(image, side, 130, 130, 40, 40, 0);
    if (hit.count != 25 || hit.x / hit.count != 150 || hit.y / hit.count != 150)
        return 2;

    std::fill(image.begin(), image.end(), 0);
    for (int y = 148; y <= 150; ++y)
        for (int x = 148; x <= 150; ++x)
            if (x != 149 || y != 149) red_pixel(image, side, x, y);
    const Result open_hole = detect(image, side, 130, 130, 40, 40, 0);
    const Result closed_hole = detect(image, side, 130, 130, 40, 40, 1);
    if (open_hole.count != 8 || closed_hole.count != 9)
        return 3;

    std::fill(image.begin(), image.end(), 0);
    for (int y = 249; y <= 251; ++y)
        for (int x = 249; x <= 251; ++x)
            red_pixel(image, side, x, y);
    hit = detect(image, side, 0, 0, 300, 300, 0);
    if (hit.count != 9 || hit.x / hit.count != 250 || hit.y / hit.count != 250)
        return 4;

    std::fill(image.begin(), image.end(), 0);
    for (int y = 148; y <= 152; ++y)
        for (int x = 138; x <= 142; ++x)
            red_pixel(image, side, x, y);
    for (int y = 148; y <= 152; ++y)
        for (int x = 178; x <= 182; ++x)
            red_pixel(image, side, x, y);
    hit = detect(image, side, 120, 130, 80, 40, 0, 180, 150);
    if (hit.count != 25 || hit.x / hit.count != 180 || hit.y / hit.count != 150)
        return 5;

    std::fill(image.begin(), image.end(), 0);
    for (int y = 144; y <= 156; ++y)
        for (int x = 144; x <= 156; ++x)
            red_pixel(image, side, x, y);
    hit = detect(image, side, 130, 130, 40, 40, 0, 150, 150, 150);
    if (hit.count < 150 || hit.x / hit.count != 150 || hit.y / hit.count != 150)
        return 6;

    std::puts("crosshair GPU tests passed");
    return 0;
}
