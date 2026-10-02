#include "capture/gpu_color_ops.h"
#include "crosshair/am_centroid.h"
#include "crosshair/centroid_cluster.h"

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
              int min_pixels = 4, int algorithm = 0,
              GpuHsvBand band = {0, 10, 80, 255, 80, 255})
{
    unsigned char *device_image = nullptr, *mask = nullptr, *scratch = nullptr;
    GpuHsvBand* bands = nullptr;
    int* result = nullptr;
    unsigned long long* key = nullptr;
    int* labels = nullptr;
    crosshair::CentroidComponent* components = nullptr;
    check(cudaMalloc(reinterpret_cast<void**>(&device_image), image.size()));
    check(cudaMalloc(reinterpret_cast<void**>(&mask), 512 * 512));
    check(cudaMalloc(reinterpret_cast<void**>(&scratch), 512 * 512));
    check(cudaMalloc(reinterpret_cast<void**>(&bands), sizeof(GpuHsvBand)));
    check(cudaMalloc(reinterpret_cast<void**>(&result), 4 * sizeof(int)));
    check(cudaMalloc(reinterpret_cast<void**>(&key), sizeof(unsigned long long)));
    check(cudaMalloc(reinterpret_cast<void**>(&labels),512*512*sizeof(int)));
    check(cudaMalloc(reinterpret_cast<void**>(&components),512*512*sizeof(crosshair::CentroidComponent)));

    check(cudaMemcpy(device_image, image.data(), image.size(), cudaMemcpyHostToDevice));
    check(cudaMemcpy(bands, &band, sizeof(band), cudaMemcpyHostToDevice));
    check(cudaMemset(result, 0, 4 * sizeof(int)));
    check(cudaMemset(key, 0, sizeof(unsigned long long)));
    launch_crosshair_hsv_reduce_bgr_u8(
        device_image, static_cast<size_t>(side) * 3, side, side,
        roi_x, roi_y, roi_w, roi_h, bands, 1, result, key, mask, scratch,
        labels, components,
        close_radius, min_pixels, reference_x, reference_y, algorithm, nullptr);
    check(cudaGetLastError());
    int out[4]{};
    check(cudaMemcpy(out, result, sizeof(out), cudaMemcpyDeviceToHost));

    check(cudaFree(key));
    check(cudaFree(labels));
    check(cudaFree(components));
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

    std::fill(image.begin(), image.end(), 0);
    red_pixel(image, side, 140, 150);
    red_pixel(image, side, 160, 150);
    red_pixel(image, side, 150, 140);
    red_pixel(image, side, 150, 160);
    hit = detect(image, side, 130, 130, 40, 40, 7, 150, 150, 4, 1);
    if (hit.count != 4 || hit.x != 600 || hit.y != 600) return 7;
    std::fill(image.begin(), image.end(), 0);
    hit = detect(image, side, 130, 130, 40, 40, 0, 150, 150, 4, 1);
    if (hit.count != 0) return 8;
    red_pixel(image, side, 140, 150);
    red_pixel(image, side, 141, 151);
    hit = detect(image, side, 130,130,40,40,7,150,150,2,1,{170,10,80,255,80,255});
    if (hit.count != 2 || crosshair::amCentroidCoordinate(hit.x, hit.count) != 140
        || crosshair::amCentroidCoordinate(hit.y, hit.count) != 150) return 9;
    std::fill(image.begin(),image.end(),0);
    const size_t pixel = (150 * side + 140) * 3;
    image[pixel+1]=5; image[pixel+2]=255; // AM truncates H to 0, not round to 1.
    image[pixel+4]=5; image[pixel+5]=255;
    hit = detect(image,side,130,130,40,40,7,150,150,2,1,{0,0,255,255,255,255});
    if(hit.count!=2 || hit.x!=281 || hit.y!=300)return 10;
    for(int bg : {130,190}) {
        std::fill(image.begin(),image.end(),0);
        for(int y=159;y<=161;++y) for(int x=159;x<=161;++x)red_pixel(image,side,x,y);
        for(int y=156;y<=164;++y) for(int x=bg-4;x<=bg+4;++x)red_pixel(image,side,x,y);
        hit=detect(image,side,110,110,100,100,0,160,160,4,1);
        if(hit.count!=9 || hit.x!=1440 || hit.y!=1440)return 11;
    }
    std::fill(image.begin(),image.end(),0);
    for(int y=159;y<=161;++y) for(int x : {159,160,161,178,179,180})red_pixel(image,side,x,y);
    hit=detect(image,side,110,110,100,100,0,160,160,4,1);
    if(hit.count!=9 || hit.x!=1440 || hit.y!=1440)return 12;
    std::fill(image.begin(),image.end(),0);
    red_pixel(image,side,160,160);
    hit=detect(image,side,110,110,100,100,0,160,160,1,1);
    if(hit.count)return 13;
    for(int y=0;y<side;++y)for(int x=0;x<side;++x)red_pixel(image,side,x,y);
    hit=detect(image,side,0,0,side,side,0,160,160,4,1);
    if(hit.count)return 14;
    std::fill(image.begin(),image.end(),0);
    for(int y=159;y<=161;++y)for(int x=110;x<=115;++x)red_pixel(image,side,x,y);
    hit=detect(image,side,110,110,100,100,0,160,160,4,1);
    if(hit.count)return 15;
    std::fill(image.begin(),image.end(),0);
    for(int y=174;y<=176;++y)for(int x=159;x<=161;++x)red_pixel(image,side,x,y);
    hit=detect(image,side,110,110,100,100,0,160,160,4,1);
    if(hit.count!=9 || hit.x!=1440 || hit.y!=1575)return 16;
    hit=detect(image,side,110,110,100,100,0,160,160,10,1);
    if(hit.count)return 17;
    std::puts("crosshair GPU tests passed");
    return 0;
}
