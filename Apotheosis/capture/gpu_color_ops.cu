#include "gpu_color_ops.h"
#include "crosshair/am_centroid.h"
#include "crosshair/centroid_cluster.h"

#include <cuda_runtime.h>

static __global__ void bgra_to_bgr_u8_kernel(
    const unsigned char* __restrict__ src, int srcStep,
    unsigned char* __restrict__ dst, int dstStep,
    int width, int height)
{
    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    const int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= width || y >= height) return;

    const unsigned char* sp = src + y * srcStep + x * 4;
    unsigned char* dp = dst + y * dstStep + x * 3;
    dp[0] = sp[0];
    dp[1] = sp[1];
    dp[2] = sp[2];
}

void launch_bgra_to_bgr_u8(
    const unsigned char* src, size_t srcStep,
    unsigned char* dst, size_t dstStep,
    int width, int height,
    cudaStream_t stream)
{
    if (!src || !dst || width <= 0 || height <= 0)
        return;
    const dim3 block(32, 8);
    const dim3 grid((width + block.x - 1) / block.x,
                    (height + block.y - 1) / block.y);
    bgra_to_bgr_u8_kernel<<<grid, block, 0, stream>>>(
        src, static_cast<int>(srcStep), dst, static_cast<int>(dstStep), width, height);
}

static __global__ void resize_bgr_u8_bilinear_kernel(
    const unsigned char* __restrict__ src, int srcStep, int srcW, int srcH,
    unsigned char* __restrict__ dst, int dstStep, int dstW, int dstH)
{
    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    const int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= dstW || y >= dstH) return;

    const float scaleX = static_cast<float>(srcW) / static_cast<float>(dstW);
    const float scaleY = static_cast<float>(srcH) / static_cast<float>(dstH);
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

    const unsigned char* p00 = src + y0 * srcStep + x0 * 3;
    const unsigned char* p01 = src + y0 * srcStep + x1 * 3;
    const unsigned char* p10 = src + y1 * srcStep + x0 * 3;
    const unsigned char* p11 = src + y1 * srcStep + x1 * 3;

    unsigned char* dp = dst + y * dstStep + x * 3;
    #pragma unroll
    for (int c = 0; c < 3; ++c)
    {
        const float v = p00[c] * w00 + p01[c] * w01 + p10[c] * w10 + p11[c] * w11;
        const int iv = max(0, min(255, static_cast<int>(v + 0.5f)));
        dp[c] = static_cast<unsigned char>(iv);
    }
}

void launch_resize_bgr_u8_bilinear(
    const unsigned char* src, size_t srcStep, int srcW, int srcH,
    unsigned char* dst, size_t dstStep, int dstW, int dstH,
    cudaStream_t stream)
{
    if (!src || !dst || srcW <= 0 || srcH <= 0 || dstW <= 0 || dstH <= 0)
        return;
    const dim3 block(32, 8);
    const dim3 grid((dstW + block.x - 1) / block.x,
                    (dstH + block.y - 1) / block.y);
    resize_bgr_u8_bilinear_kernel<<<grid, block, 0, stream>>>(
        src, static_cast<int>(srcStep), srcW, srcH,
        dst, static_cast<int>(dstStep), dstW, dstH);
}

static __device__ __forceinline__ unsigned char clamp_byte(int v) {
    return (unsigned char)(v < 0 ? 0 : (v > 255 ? 255 : v));
}

static __global__ void nv12_to_bgr_u8_kernel(
    const unsigned char* __restrict__ y_plane, int yStep,
    const unsigned char* __restrict__ uv_plane, int uvStep,
    unsigned char* __restrict__ bgr, int bgrStep,
    int width, int height)
{
    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    const int yy = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= width || yy >= height) return;

    const int Y = (int)y_plane[yy * yStep + x];
    const int uvRow = yy >> 1;
    const int uvCol = (x >> 1) << 1;
    const int U = (int)uv_plane[uvRow * uvStep + uvCol]     - 128;
    const int V = (int)uv_plane[uvRow * uvStep + uvCol + 1] - 128;

    int R = (Y * 1024 +              1436 * V + 512) >> 10;
    int G = (Y * 1024 -  352 * U -    731 * V + 512) >> 10;
    int B = (Y * 1024 + 1815 * U              + 512) >> 10;

    unsigned char* dp = bgr + yy * bgrStep + x * 3;
    dp[0] = clamp_byte(B);
    dp[1] = clamp_byte(G);
    dp[2] = clamp_byte(R);
}

void launch_nv12_to_bgr_u8(
    const unsigned char* y, size_t yStep,
    const unsigned char* uv, size_t uvStep,
    unsigned char* bgr, size_t bgrStep,
    int width, int height,
    cudaStream_t stream)
{
    if (!y || !uv || !bgr || width <= 0 || height <= 0) return;
    const dim3 block(32, 8);
    const dim3 grid((width + block.x - 1) / block.x,
                    (height + block.y - 1) / block.y);
    nv12_to_bgr_u8_kernel<<<grid, block, 0, stream>>>(
        y,  (int)yStep,
        uv, (int)uvStep,
        bgr,(int)bgrStep,
        width, height);
}

static __global__ void nv12_to_bgr_bt601_limited_kernel(
    const unsigned char* __restrict__ y_plane, int yStep,
    const unsigned char* __restrict__ uv_plane, int uvStep,
    unsigned char* __restrict__ bgr, int bgrStep,
    int width, int height)
{
    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    const int row = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= width || row >= height) return;

    const int yy = max(0, (int)y_plane[row * yStep + x] - 16);
    const int uvCol = (x >> 1) << 1;
    const int u = (int)uv_plane[(row >> 1) * uvStep + uvCol] - 128;
    const int v = (int)uv_plane[(row >> 1) * uvStep + uvCol + 1] - 128;
    unsigned char* dst = bgr + row * bgrStep + x * 3;
    dst[0] = clamp_byte((298 * yy + 516 * u + 128) >> 8);
    dst[1] = clamp_byte((298 * yy - 100 * u - 208 * v + 128) >> 8);
    dst[2] = clamp_byte((298 * yy + 409 * v + 128) >> 8);
}

void launch_nv12_to_bgr_bt601_limited_u8(
    const unsigned char* y, size_t yStep,
    const unsigned char* uv, size_t uvStep,
    unsigned char* bgr, size_t bgrStep,
    int width, int height,
    cudaStream_t stream)
{
    if (!y || !uv || !bgr || width <= 0 || height <= 0) return;
    const dim3 block(32, 8);
    const dim3 grid((width + block.x - 1) / block.x,
                    (height + block.y - 1) / block.y);
    nv12_to_bgr_bt601_limited_kernel<<<grid, block, 0, stream>>>(
        y, (int)yStep, uv, (int)uvStep, bgr, (int)bgrStep, width, height);
}

static __global__ void yuv444_to_bgr_u8_kernel(
    const unsigned char* __restrict__ y_p,
    const unsigned char* __restrict__ u_p,
    const unsigned char* __restrict__ v_p,
    int stride,
    unsigned char* __restrict__ bgr, int bgrStep,
    int width, int height)
{
    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    const int yy = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= width || yy >= height) return;

    const int row = yy * stride + x;
    const int Y = (int)y_p[row];
    const int U = (int)u_p[row] - 128;
    const int V = (int)v_p[row] - 128;

    int R = (Y * 1024 +              1436 * V + 512) >> 10;
    int G = (Y * 1024 -  352 * U -    731 * V + 512) >> 10;
    int B = (Y * 1024 + 1815 * U              + 512) >> 10;

    unsigned char* dp = bgr + yy * bgrStep + x * 3;
    dp[0] = clamp_byte(B);
    dp[1] = clamp_byte(G);
    dp[2] = clamp_byte(R);
}

void launch_yuv444_to_bgr_u8(
    const unsigned char* y,
    const unsigned char* u,
    const unsigned char* v,
    size_t stride,
    unsigned char* bgr, size_t bgrStep,
    int width, int height,
    cudaStream_t stream)
{
    if (!y || !u || !v || !bgr || width <= 0 || height <= 0) return;
    const dim3 block(32, 8);
    const dim3 grid((width + block.x - 1) / block.x,
                    (height + block.y - 1) / block.y);
    yuv444_to_bgr_u8_kernel<<<grid, block, 0, stream>>>(
        y, u, v, (int)stride,
        bgr, (int)bgrStep,
        width, height);
}

static __global__ void circle_mask_bgr_u8_kernel(
    unsigned char* __restrict__ img, int step,
    int width, int height,
    int cx, int cy, int radius_sq)
{
    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    const int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= width || y >= height) return;
    const int dx = x - cx;
    const int dy = y - cy;
    if (dx * dx + dy * dy > radius_sq)
    {
        unsigned char* p = img + y * step + x * 3;
        p[0] = 0; p[1] = 0; p[2] = 0;
    }
}

void launch_circle_mask_bgr_u8(
    unsigned char* img, size_t step,
    int width, int height,
    cudaStream_t stream)
{
    if (!img || width <= 0 || height <= 0) return;
    const int cx = width / 2;
    const int cy = height / 2;
    const int r  = (width < height ? width : height) / 2;
    const int r2 = r * r;
    const dim3 block(32, 8);
    const dim3 grid((width + block.x - 1) / block.x,
                    (height + block.y - 1) / block.y);
    circle_mask_bgr_u8_kernel<<<grid, block, 0, stream>>>(
        img, (int)step, width, height, cx, cy, r2);
}

static __device__ __forceinline__ bool hsv_band_match_bgr(
    const unsigned char* p, const GpuHsvBand* bands, int band_count)
{
    const float b = static_cast<float>(p[0]);
    const float g = static_cast<float>(p[1]);
    const float r = static_cast<float>(p[2]);
    const float vmax = fmaxf(r, fmaxf(g, b));
    const float vmin = fminf(r, fminf(g, b));
    const float delta = vmax - vmin;

    float hue_deg = 0.0f;
    if (delta > 0.0f)
    {
        if (vmax == r)      hue_deg = 60.0f * fmodf((g - b) / delta, 6.0f);
        else if (vmax == g) hue_deg = 60.0f * ((b - r) / delta + 2.0f);
        else                hue_deg = 60.0f * ((r - g) / delta + 4.0f);
        if (hue_deg < 0.0f) hue_deg += 360.0f;
    }
    const int h = max(0, min(179, static_cast<int>(hue_deg * 0.5f + 0.5f)));
    const int s = vmax > 0.0f
        ? max(0, min(255, static_cast<int>(delta * 255.0f / vmax + 0.5f)))
        : 0;
    const int v = max(0, min(255, static_cast<int>(vmax + 0.5f)));

    for (int i = 0; i < band_count; ++i)
    {
        const GpuHsvBand q = bands[i];
        const int hlo = min(q.h_low, q.h_high), hhi = max(q.h_low, q.h_high);
        const int slo = min(q.s_min, q.s_max),   shi = max(q.s_min, q.s_max);
        const int vlo = min(q.v_min, q.v_max),   vhi = max(q.v_min, q.v_max);
        if (h >= hlo && h <= hhi && s >= slo && s <= shi && v >= vlo && v <= vhi)
            return true;
    }
    return false;
}

static __global__ void crosshair_hsv_mask_kernel(
    const unsigned char* __restrict__ img, int step,
    int roi_x, int roi_y, int roi_w, int roi_h,
    const GpuHsvBand* __restrict__ bands, int band_count,
    unsigned char* __restrict__ mask, int algorithm)
{
    const int lx = blockIdx.x * blockDim.x + threadIdx.x;
    const int ly = blockIdx.y * blockDim.y + threadIdx.y;
    if (lx >= roi_w || ly >= roi_h) return;
    const int x = roi_x + lx, y = roi_y + ly;
    const unsigned char* p = img + static_cast<size_t>(y) * step + x * 3;
    if (algorithm == 1) {
        const auto hsv = crosshair::amHsv(p[0], p[1], p[2]);
        bool hit = false;
        for (int i = 0; i < band_count; ++i)
            if (crosshair::amHsvMatches(hsv, bands[i])) { hit = true; break; }
        mask[ly * roi_w + lx] = hit ? 1 : 0;
    } else {
        mask[ly * roi_w + lx] = hsv_band_match_bgr(p, bands, band_count) ? 1 : 0;
    }
}

static __global__ void crosshair_morph_kernel(
    const unsigned char* __restrict__ src,
    unsigned char* __restrict__ dst,
    int roi_w, int roi_h, int radius, bool dilate)
{
    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    const int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= roi_w || y >= roi_h) return;
    bool value = !dilate;
    for (int dy = -radius; dy <= radius; ++dy)
    {
        for (int dx = -radius; dx <= radius; ++dx)
        {
            if (dx * dx + dy * dy > radius * radius) continue;
            const int xx = x + dx, yy = y + dy;
            if (xx < 0 || yy < 0 || xx >= roi_w || yy >= roi_h) continue;
            const bool matched = src[yy * roi_w + xx] != 0;
            if (dilate) value = value || matched;
            else value = value && matched;
        }
    }
    dst[y * roi_w + x] = value ? 1 : 0;
}

static __global__ void crosshair_hsv_reduce_bgr_u8_kernel(
    const unsigned char* __restrict__ mask,
    int reference_x, int reference_y,
    int roi_x, int roi_y, int roi_w, int roi_h,
    int min_pixels, int local_radius,
    unsigned long long* __restrict__ candidate_key)
{
    const int lx = blockIdx.x * blockDim.x + threadIdx.x;
    const int ly = blockIdx.y * blockDim.y + threadIdx.y;
    if (lx >= roi_w || ly >= roi_h) return;
    if (!mask[ly * roi_w + lx]) return;
    const int x = roi_x + lx, y = roi_y + ly;

    int support = 1;
    const int nx[4] = { -1, 1, 0, 0 };
    const int ny[4] = { 0, 0, -1, 1 };
    #pragma unroll
    for (int i = 0; i < 4; ++i)
    {
        const int xx = lx + nx[i], yy = ly + ny[i];
        if (xx < 0 || yy < 0 || xx >= roi_w || yy >= roi_h)
            continue;
        support += mask[yy * roi_w + xx] != 0;
    }
    if (support < 3) return;

    int local_count = 0;
    for (int yy = max(0, ly - local_radius);
         yy <= min(roi_h - 1, ly + local_radius); ++yy)
    {
        for (int xx = max(0, lx - local_radius);
             xx <= min(roi_w - 1, lx + local_radius); ++xx)
            local_count += mask[yy * roi_w + xx] != 0;
    }
    if (local_count < min_pixels) return;

    const int centre_dx = x - reference_x;
    const int centre_dy = y - reference_y;
    const int distance2 = centre_dx * centre_dx + centre_dy * centre_dy;
    const int roi_distance2 = max(1, roi_w * roi_w + roi_h * roi_h);
    const unsigned int scaled_distance = static_cast<unsigned int>(fminf(
        1023.0f, static_cast<float>(distance2) * 1023.0f
            / static_cast<float>(roi_distance2)));
    const unsigned int proximity = 1023u - scaled_distance;

    // Keep colour density important without letting one extra pixel outweigh
    // the entire distance to the image centre.
    const unsigned int quality =
        static_cast<unsigned int>(min(local_count, 63)) * 64u + proximity;
    const unsigned int index = static_cast<unsigned int>(ly * roi_w + lx);
    const unsigned long long key =
        (static_cast<unsigned long long>(quality) << 32)
        | (0xffffffffull - static_cast<unsigned long long>(index));
    atomicMax(candidate_key, key);
}

static __global__ void crosshair_hsv_selected_cluster_kernel(
    const unsigned char* __restrict__ mask,
    int width, int height,
    int roi_x, int roi_y, int roi_w, int roi_h,
    int local_radius,
    const unsigned long long* __restrict__ candidate_key,
    int* __restrict__ result)
{
    const unsigned long long key = candidate_key[0];
    if (key == 0) return;

    const unsigned int index =
        0xffffffffu - static_cast<unsigned int>(key & 0xffffffffull);
    if (index >= static_cast<unsigned int>(roi_w * roi_h)) return;
    const int candidate_x = roi_x + static_cast<int>(index % roi_w);
    const int candidate_y = roi_y + static_cast<int>(index / roi_w);

    const int ox = static_cast<int>(threadIdx.x) - local_radius;
    const int oy = static_cast<int>(threadIdx.y) - local_radius;
    const int x = candidate_x + ox;
    const int y = candidate_y + oy;
    if (x < roi_x || y < roi_y || x >= roi_x + roi_w || y >= roi_y + roi_h
        || x < 0 || y < 0 || x >= width || y >= height)
        return;

    if (!mask[(y - roi_y) * roi_w + (x - roi_x)]) return;

    int support = 1;
    const int nx[4] = { -1, 1, 0, 0 };
    const int ny[4] = { 0, 0, -1, 1 };
    #pragma unroll
    for (int i = 0; i < 4; ++i)
    {
        const int xx = x + nx[i], yy = y + ny[i];
        if (xx < roi_x || yy < roi_y || xx >= roi_x + roi_w || yy >= roi_y + roi_h)
            continue;
        support += mask[(yy - roi_y) * roi_w + (xx - roi_x)] != 0;
    }
    if (support < 3) return;

    atomicAdd(result + 1, 1);
    atomicAdd(result + 2, x);
    atomicAdd(result + 3, y);
}

static __global__ void centroid_init(const unsigned char* mask, int n, int* labels,
    crosshair::CentroidComponent* components) {
    const int i=blockIdx.x*blockDim.x+threadIdx.x;
    if(i>=n)return;
    labels[i]=mask[i]?i:-1;
    components[i]=crosshair::CentroidComponent{};
}
static __device__ int centroid_root_atomic(int* labels,int i) {
    int next;
    while((next=atomicAdd(labels+i,0))!=i)i=next;
    return i;
}
static __device__ void centroid_union(int* labels,int a,int b) {
    for (;;) {
        a=centroid_root_atomic(labels,a); b=centroid_root_atomic(labels,b);
        if(a==b)return;
        const int hi=max(a,b),lo=min(a,b);
        if(atomicCAS(labels+hi,hi,lo)==hi)return;
    }
}
static __global__ void centroid_connect(const unsigned char* mask,int w,int h,int* labels) {
    const int i=blockIdx.x*blockDim.x+threadIdx.x;
    if(i>=w*h || !mask[i])return;
    const int x=i%w,y=i/w;
    if(x && mask[i-1])centroid_union(labels,i,i-1);
    if(y) for(int dx=-1;dx<=1;++dx)
        if(x+dx>=0 && x+dx<w && mask[i-w+dx])centroid_union(labels,i,i-w+dx);
}
static __global__ void centroid_stats(const unsigned char* mask,int w,int h,
    const int* labels,crosshair::CentroidComponent* components) {
    const int i=blockIdx.x*blockDim.x+threadIdx.x;
    if(i>=w*h || !mask[i])return;
    int root=i; while(labels[root]!=root)root=labels[root];
    auto* c=components+root; const int x=i%w,y=i/w;
    atomicAdd(&c->count,1); atomicAdd(&c->sumX,x); atomicAdd(&c->sumY,y);
    atomicMin(&c->left,x); atomicMax(&c->right,x);
    atomicMin(&c->top,y); atomicMax(&c->bottom,y);
}
static __global__ void centroid_select(int w,int h,int reference_x,int reference_y,
    const crosshair::CentroidComponent* components,unsigned long long* key) {
    const int i=blockIdx.x*blockDim.x+threadIdx.x;
    if(i>=w*h || !crosshair::centroidComponentValid(components[i],w,h))return;
    atomicMax(key,crosshair::centroidComponentKey(components[i],i,reference_x,reference_y));
}
static __global__ void centroid_selected_sum(int rx,int ry,int w,int h,
    const crosshair::CentroidComponent* components,const unsigned long long* key,int* result) {
    const int i=blockIdx.x*blockDim.x+threadIdx.x;
    if(i>=w*h || !*key)return;
    const unsigned int seed=0xffffffffu-static_cast<unsigned int>(*key);
    if(seed>=static_cast<unsigned int>(w*h))return;
    const auto c=components[i];
    if(i!=seed && !crosshair::centroidSameCluster(c,components[seed],w,h))return;
    atomicAdd(result+1,c.count);
    atomicAdd(result+2,c.sumX+rx*c.count); atomicAdd(result+3,c.sumY+ry*c.count);
}
static __global__ void centroid_validate(int* result,int min_pixels) {
    if(result[1]<max(2,min_pixels))result[1]=result[2]=result[3]=0;
}

void launch_crosshair_hsv_reduce_bgr_u8(
    const unsigned char* img, size_t step,
    int width, int height,
    int roi_x, int roi_y, int roi_w, int roi_h,
    const GpuHsvBand* bands, int band_count,
    int* result, unsigned long long* candidate_key,
    unsigned char* mask, unsigned char* scratch,
    int* component_labels, crosshair::CentroidComponent* components,
    int close_radius, int min_pixels, int reference_x, int reference_y, int algorithm,
    cudaStream_t stream)
{
    if (!img || !bands || !result || !candidate_key || !mask || !scratch
        || width <= 0 || height <= 0
        || roi_w <= 0 || roi_h <= 0 || band_count <= 0
        || roi_x < 0 || roi_y < 0 || roi_w > 512 || roi_h > 512
        || roi_x + roi_w > width || roi_y + roi_h > height)
        return;
    close_radius = max(0, min(close_radius, 7));
    min_pixels = max(1, min_pixels);
    const int local_radius = min_pixels > 121 ? 8 : 5;
    const dim3 block(16, 16);
    const dim3 grid((roi_w + block.x - 1) / block.x,
                    (roi_h + block.y - 1) / block.y);
    crosshair_hsv_mask_kernel<<<grid, block, 0, stream>>>(
        img, static_cast<int>(step),
        roi_x, roi_y, roi_w, roi_h, bands, band_count, mask, algorithm);
    if (algorithm == 1) {
        if (!component_labels || !components) return;
        const int n=roi_w*roi_h, blocks=(n+255)/256;
        centroid_init<<<blocks,256,0,stream>>>(mask,n,component_labels,components);
        centroid_connect<<<blocks,256,0,stream>>>(mask,roi_w,roi_h,component_labels);
        centroid_stats<<<blocks,256,0,stream>>>(mask,roi_w,roi_h,component_labels,components);
        centroid_select<<<blocks,256,0,stream>>>(roi_w,roi_h,reference_x-roi_x,reference_y-roi_y,components,candidate_key);
        centroid_selected_sum<<<blocks,256,0,stream>>>(roi_x,roi_y,roi_w,roi_h,components,candidate_key,result);
        centroid_validate<<<1,1,0,stream>>>(result,min_pixels);
        return;
    }
    if (close_radius > 0)
    {
        crosshair_morph_kernel<<<grid, block, 0, stream>>>(
            mask, scratch, roi_w, roi_h, close_radius, true);
        crosshair_morph_kernel<<<grid, block, 0, stream>>>(
            scratch, mask, roi_w, roi_h, close_radius, false);
    }
    crosshair_hsv_reduce_bgr_u8_kernel<<<grid, block, 0, stream>>>(
        mask, reference_x, reference_y, roi_x, roi_y, roi_w, roi_h,
        min_pixels, local_radius, candidate_key);
    const int local_side = 2 * local_radius + 1;
    crosshair_hsv_selected_cluster_kernel<<<1, dim3(local_side, local_side), 0, stream>>>(
        mask, width, height, roi_x, roi_y, roi_w, roi_h,
        local_radius, candidate_key, result);
}
