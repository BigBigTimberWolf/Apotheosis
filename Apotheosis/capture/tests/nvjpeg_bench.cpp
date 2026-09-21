
// nvJPEG 解码路径微基准 —— 用一张真实采集帧比较 4 条解码路径的每帧开销。
//
// 背景: 本机 CPU 只有 4 核 (i5-4590, 无超线程), 且已被 TensorRT 推理占满,
//   "多加解码 worker" 是死路。唯一治本的方向是把熵解码从 CPU 上拿掉,
//   即改用 NVJPEG_BACKEND_GPU_HYBRID / NVJPEG_BACKEND_HARDWARE。
//
// 对比路径:
//   A. split + ROI      : 现有 GpuJpegDecoder::decodeCropped 的做法 (DEFAULT 后端)
//   B. nvjpegDecode     : 现有 decode() 的做法 (单张简化 API, 整帧)
//   C. batched DEFAULT  : nvjpegDecodeBatched (DEFAULT 后端, 整帧)
//   D. batched HYBRID   : GPU 辅助霍夫曼解码 (backend=2)
//   E. batched HARDWARE : NVJPG 专用解码引擎 (backend=3)
//
// 用法: nvjpeg_bench <frame.jpg> [ROI边长] [迭代次数]
//   例: nvjpeg_bench build\diag\live_frame.jpg 256 200

#include <cuda_runtime.h>
#include <nvjpeg.h>
#include <opencv2/opencv.hpp>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace
{

double NowMs()
{
    using clock = std::chrono::steady_clock;
    static const clock::time_point start = clock::now();
    return std::chrono::duration<double, std::milli>(clock::now() - start).count();
}

int LoadFile(const char* path, std::vector<unsigned char>& out)
{
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in)
        return -1;
    const std::streamsize size = in.tellg();
    in.seekg(0, std::ios::beg);
    out.resize(static_cast<size_t>(size));
    in.read(reinterpret_cast<char*>(out.data()), size);
    return in ? 0 : -1;
}

struct Timing
{
    bool           ok = false;
    int            status = -1;
    double         hostMs = 0.0;   // 纯主机侧调用耗时 (不含同步)
    double         syncMs = 0.0;   // 主机调用 + cudaStreamSynchronize
    int            outW = 0;
    int            outH = 0;
    unsigned long long checksum = 0;
};

} // namespace

int main(int argc, char** argv)
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    const char* path = (argc > 1) ? argv[1] : "build\\diag\\live_frame.jpg";
    const int   roiSide = (argc > 2) ? std::atoi(argv[2]) : 256;
    const int   iters   = (argc > 3) ? std::atoi(argv[3]) : 200;

    std::vector<unsigned char> jpeg;
    if (LoadFile(path, jpeg) != 0 || jpeg.empty())
    {
        printf("读不到帧文件: %s\n", path);
        return 2;
    }
    printf("=== nvJPEG 解码路径微基准 ===\n");
    printf("样本帧: %s  (%zu 字节, %.1f KB)\n", path, jpeg.size(), jpeg.size() / 1024.0);
    printf("ROI: %dx%d   迭代: %d\n\n", roiSide, roiSide, iters);

    cudaStream_t stream = nullptr;
    if (cudaStreamCreateWithFlags(&stream, cudaStreamNonBlocking) != cudaSuccess)
    {
        printf("cudaStreamCreate 失败\n");
        return 3;
    }

    // 全帧输出缓冲 (1920x1080 BGR)
    unsigned char* dFull = nullptr;
    const size_t fullBytes = static_cast<size_t>(1920) * 1080 * 3;
    cudaMalloc(&dFull, fullBytes);

    // ── A. split + ROI (现有路径), 分别用 DEFAULT / GPU_HYBRID 后端 ─────────
    // 关键问题: GPU_HYBRID 把霍夫曼熵解码搬到 GPU, 但前面的对比里它走的是整帧
    // 输出, 多出的设备端开销掩盖了后端优势。这里把"后端"和"ROI"两个变量分开,
    // 用同一条 split+ROI 路径分别测, 才能看出后端本身值不值得换。
    {
        struct RoiVariant { const char* name; nvjpegBackend_t backend; };
        const RoiVariant roiVariants[] = {
            { "A1. split+ROI  DEFAULT   ", NVJPEG_BACKEND_DEFAULT    },
            { "A2. split+ROI  GPU_HYBRID", NVJPEG_BACKEND_GPU_HYBRID },
        };

        for (const RoiVariant& rv : roiVariants)
        {
            Timing t;
            nvjpegHandle_t handle = nullptr;
            nvjpegJpegDecoder_t decoder = nullptr;
            nvjpegDecodeParams_t params = nullptr;
            nvjpegJpegState_t state = nullptr;
            nvjpegBufferPinned_t pinned = nullptr;
            nvjpegBufferDevice_t device = nullptr;
            nvjpegJpegStream_t jstream = nullptr;

            nvjpegStatus_t init = nvjpegCreateEx(rv.backend, nullptr, nullptr, 0, &handle);
            if (init == NVJPEG_STATUS_SUCCESS)
                init = nvjpegDecoderCreate(handle, rv.backend, &decoder);
            if (init == NVJPEG_STATUS_SUCCESS)
                init = nvjpegDecodeParamsCreate(handle, &params);
            if (init == NVJPEG_STATUS_SUCCESS)
                init = nvjpegDecoderStateCreate(handle, decoder, &state);
            if (init == NVJPEG_STATUS_SUCCESS)
                init = nvjpegBufferPinnedCreate(handle, nullptr, &pinned);
            if (init == NVJPEG_STATUS_SUCCESS)
                init = nvjpegBufferDeviceCreate(handle, nullptr, &device);
            if (init == NVJPEG_STATUS_SUCCESS)
                init = nvjpegJpegStreamCreate(handle, &jstream);

            if (init != NVJPEG_STATUS_SUCCESS)
            {
                printf("%s: 初始化失败 status=%d\n", rv.name, static_cast<int>(init));
                if (handle) nvjpegDestroy(handle);
                continue;
            }

            nvjpegDecodeParamsSetOutputFormat(params, NVJPEG_OUTPUT_BGRI);
            nvjpegStateAttachPinnedBuffer(state, pinned);
            nvjpegStateAttachDeviceBuffer(state, device);

            unsigned char* dRoi = nullptr;
            cudaMalloc(&dRoi, static_cast<size_t>(roiSide) * roiSide * 3);

            nvjpegImage_t img{};
            img.channel[0] = dRoi;
            img.pitch[0] = static_cast<unsigned int>(roiSide * 3);

            unsigned int fw = 0, fh = 0;
            nvjpegJpegStreamParse(handle, jpeg.data(), jpeg.size(), 0, 0, jstream);
            nvjpegJpegStreamGetFrameDimensions(jstream, &fw, &fh);
            nvjpegDecodeParamsSetROI(params,
                                     static_cast<int>((fw - roiSide) / 2) & ~1,
                                     static_cast<int>((fh - roiSide) / 2) & ~1,
                                     roiSide, roiSide);

            const int warm = 3;
            for (int i = 0; i < warm + iters; ++i)
            {
                const double a = NowMs();
                nvjpegStatus_t st = nvjpegJpegStreamParse(handle, jpeg.data(), jpeg.size(), 0, 0, jstream);
                if (st == NVJPEG_STATUS_SUCCESS)
                    st = nvjpegDecodeJpegHost(handle, decoder, state, params, jstream);
                if (st == NVJPEG_STATUS_SUCCESS)
                    st = nvjpegDecodeJpegTransferToDevice(handle, decoder, state, jstream, stream);
                if (st == NVJPEG_STATUS_SUCCESS)
                    st = nvjpegDecodeJpegDevice(handle, decoder, state, &img, stream);
                const double b = NowMs();
                cudaStreamSynchronize(stream);
                const double c = NowMs();

                if (i == warm - 1 && st == NVJPEG_STATUS_SUCCESS)
                {
                    std::vector<unsigned char> host(static_cast<size_t>(roiSide) * roiSide * 3);
                    cudaMemcpy(host.data(), dRoi, host.size(), cudaMemcpyDeviceToHost);
                    for (size_t k = 0; k < host.size(); k += 97)
                        t.checksum += host[k];
                    t.outW = roiSide; t.outH = roiSide;
                }
                if (i >= warm)
                {
                    t.hostMs += (b - a);
                    t.syncMs += (c - a);
                    t.status = static_cast<int>(st);
                }
                t.ok = (st == NVJPEG_STATUS_SUCCESS);
            }
            cudaFree(dRoi);
            nvjpegDestroy(handle);

            if (t.outW)
            {
                t.hostMs /= iters;
                t.syncMs /= iters;
                printf("%s: OK    host=%.2f ms  host+sync=%.2f ms  (%.0f fps, 校验和 %llu)\n",
                       rv.name, t.hostMs, t.syncMs, 1000.0 / t.syncMs, t.checksum);
                if (t.hostMs < 8.0)
                    printf("                       ^^^ 比现有路径快 %.1f ms/帧, 值得换\n", 8.12 - t.hostMs);
            }
            else
            {
                printf("%s: 解码失败 status=%d\n", rv.name, t.status);
            }
        }
    }

    // ── B/C/D/E. 逐帧 API 路径 ───────────────────────────────────────────────
    struct Variant { const char* name; nvjpegBackend_t backend; bool batched; };
    const Variant variants[] = {
        { "B. nvjpegDecode 整帧  ", NVJPEG_BACKEND_DEFAULT,    false },
        { "C. batched DEFAULT   ", NVJPEG_BACKEND_DEFAULT,    true  },
        { "D. batched GPU_HYBRID", NVJPEG_BACKEND_GPU_HYBRID, true  },
        { "E. batched HARDWARE  ", NVJPEG_BACKEND_HARDWARE,   true  },
    };

    for (const Variant& v : variants)
    {
        Timing t;
        int frames = 0;
        nvjpegHandle_t handle = nullptr;
        nvjpegJpegState_t state = nullptr;

        nvjpegStatus_t st = nvjpegCreateEx(v.backend, nullptr, nullptr, 0, &handle);
        if (st == NVJPEG_STATUS_SUCCESS)
            st = nvjpegJpegStateCreate(handle, &state);
        if (st == NVJPEG_STATUS_SUCCESS && v.batched)
            st = nvjpegDecodeBatchedInitialize(handle, state, 1, 1, NVJPEG_OUTPUT_BGRI);

        if (st != NVJPEG_STATUS_SUCCESS || !handle || !state)
        {
            printf("%s: 初始化失败 status=%d  <<<< 该后端在本机不可用\n",
                   v.name, static_cast<int>(st));
            if (handle) nvjpegDestroy(handle);
            continue;
        }
        if (v.batched)
        {
            // 让 nvJPEG 按真实帧尺寸预分配内部缓冲 (采样方式从样本帧读)
            int nComp = 0;
            nvjpegChromaSubsampling_t css = NVJPEG_CSS_420;
            int w[NVJPEG_MAX_COMPONENT] = {0}, h[NVJPEG_MAX_COMPONENT] = {0};
            if (nvjpegGetImageInfo(handle, jpeg.data(), jpeg.size(), &nComp, &css, w, h) == NVJPEG_STATUS_SUCCESS)
                nvjpegDecodeBatchedPreAllocate(handle, state, 1, w[0], h[0], css, NVJPEG_OUTPUT_BGRI);
        }

        nvjpegImage_t img{};
        img.channel[0] = dFull;
        img.pitch[0] = 1920 * 3;

        const unsigned char* dataPtr[1] = { jpeg.data() };
        const size_t lenPtr[1] = { jpeg.size() };

        const int warm = 3;
        for (int i = 0; i < warm + iters; ++i)
        {
            const double a = NowMs();
            nvjpegStatus_t s2 = NVJPEG_STATUS_SUCCESS;
            if (v.batched)
                s2 = nvjpegDecodeBatched(handle, state, dataPtr, lenPtr, &img, stream);
            else
                s2 = nvjpegDecode(handle, state, jpeg.data(), jpeg.size(), NVJPEG_OUTPUT_BGRI, &img, stream);
            const double b = NowMs();
            cudaStreamSynchronize(stream);
            const double c = NowMs();

            if (s2 != NVJPEG_STATUS_SUCCESS)
            {
                printf("%s: 解码失败 status=%d (第 %d 次)\n", v.name, static_cast<int>(s2), i);
                break;
            }
            ++frames;
            if (i == warm - 1)
            {
                std::vector<unsigned char> host(1920 * 1080 * 3);
                cudaMemcpy(host.data(), dFull, host.size(), cudaMemcpyDeviceToHost);
                for (size_t k = 0; k < host.size(); k += 9973)
                    t.checksum += host[k];
                t.outW = 1920; t.outH = 1080;
            }
            if (i >= warm)
            {
                t.hostMs += (b - a);
                t.syncMs += (c - a);
            }
        }
        nvjpegDestroy(handle);

        const int timed = frames > warm ? frames - warm : 0;
        if (timed > 0)
        {
            t.hostMs /= timed;
            t.syncMs /= timed;
            printf("%s: OK    host=%.2f ms  host+sync=%.2f ms  (%.0f fps, 全帧 1920x1080, 校验和 %llu)\n",
                   v.name, t.hostMs, t.syncMs, 1000.0 / t.syncMs, t.checksum);
        }
    }

    cudaFree(dFull);
    cudaStreamDestroy(stream);

    // ── F. libjpeg-turbo (OpenCV) 全帧 CPU 解码 ──────────────────────────────
    // 目的: 比较 CPU 侧熵解码器与 nvJPEG 主机阶段的单帧开销。
    // 本机只有 4 核, 谁的"毫秒/帧"低谁就赢, 不看到底跑在 CPU 还是 GPU 上。
    {
        const int warm = 3;
        double sumMs = 0.0;
        unsigned long long checksum = 0;
        int ok = 0;
        for (int i = 0; i < warm + iters; ++i)
        {
            const double a = NowMs();
            cv::Mat bgr = cv::imdecode(jpeg, cv::IMREAD_COLOR);
            const double b = NowMs();
            if (bgr.empty())
                break;
            ++ok;
            if (i == warm - 1)
                for (int k = 0; k < bgr.rows; k += 37)
                    checksum += bgr.at<cv::Vec3b>(k, k % bgr.cols)[1];
            if (i >= warm)
                sumMs += (b - a);
        }
        if (ok > warm)
            printf("F. cv::imdecode 整帧 : OK    host=%.2f ms  (%.0f fps, 1920x1080 BGR, 校验和 %llu)\n",
                   sumMs / iters, 1000.0 / (sumMs / iters), checksum);
        else
            printf("F. cv::imdecode 整帧 : FAIL\n");

        // 1/2 缩放解码 (libjpeg DCT scaling, 跳过一半 IDCT 工作)
        sumMs = 0.0; ok = 0;
        for (int i = 0; i < warm + iters; ++i)
        {
            const double a = NowMs();
            cv::Mat half = cv::imdecode(jpeg, cv::IMREAD_REDUCED_COLOR_2);
            const double b = NowMs();
            if (half.empty())
                break;
            ++ok;
            if (i == warm - 1)
                for (int k = 0; k < half.rows; k += 19)
                    checksum += half.at<cv::Vec3b>(k, k % half.cols)[1];
            if (i >= warm)
                sumMs += (b - a);
        }
        if (ok > warm)
            printf("G. cv::imdecode 1/2  : OK    host=%.2f ms  (%.0f fps, 960x540 BGR)\n",
                   sumMs / iters, 1000.0 / (sumMs / iters));

        // 1/4 缩放解码
        sumMs = 0.0; ok = 0;
        for (int i = 0; i < warm + iters; ++i)
        {
            const double a = NowMs();
            cv::Mat quart = cv::imdecode(jpeg, cv::IMREAD_REDUCED_COLOR_4);
            const double b = NowMs();
            if (quart.empty())
                break;
            ++ok;
            if (i >= warm)
                sumMs += (b - a);
        }
        if (ok > warm)
            printf("H. cv::imdecode 1/4  : OK    host=%.2f ms  (%.0f fps, 480x270 BGR)\n",
                   sumMs / iters, 1000.0 / (sumMs / iters));
    }

    // ── I. libjpeg-turbo (turbojpeg.dll) ────────────────────────────────────
    // 参考实现 (C:\Users\Administrator\Desktop\GenshinImpact) 的目录里就放着
    // turbojpeg.dll, 所以直接量它。动态加载, 不需要 import lib。
    {
        HMODULE dll = LoadLibraryA("C:\\Users\\Administrator\\Desktop\\GenshinImpact\\turbojpeg.dll");
        if (!dll)
            dll = LoadLibraryA("turbojpeg.dll");

        if (!dll)
        {
            printf("I. turbojpeg 整帧    : 加载 turbojpeg.dll 失败\n");
        }
        else
        {
            typedef void* tj_handle;
            auto pInit  = reinterpret_cast<tj_handle (*)()>(GetProcAddress(dll, "tjInitDecompress"));
            auto pHead  = reinterpret_cast<int (*)(tj_handle, const unsigned char*, unsigned long,
                                                   int*, int*, int*, int*)>(
                              GetProcAddress(dll, "tjDecompressHeader3"));
            auto pDec   = reinterpret_cast<int (*)(tj_handle, const unsigned char*, unsigned long,
                                                   unsigned char*, int, int, int, int, int)>(
                              GetProcAddress(dll, "tjDecompress2"));
            auto pFree  = reinterpret_cast<int (*)(tj_handle)>(GetProcAddress(dll, "tjDestroy"));

            if (!pInit || !pHead || !pDec || !pFree)
            {
                printf("I. turbojpeg 整帧    : 找不到所需导出函数\n");
            }
            else
            {
                tj_handle h = pInit();
                int w = 0, hgt = 0, sub = 0, cs = 0;
                if (h && pHead(h, jpeg.data(), jpeg.size(), &w, &hgt, &sub, &cs) == 0)
                {
                    std::vector<unsigned char> dst(static_cast<size_t>(w) * hgt * 3);
                    const int  TJPF_BGR       = 1;
                    const int  TJFLAG_FASTDCT = 2048;
                    const int  warm = 3;
                    double sumMs = 0.0;
                    int ok = 0;
                    unsigned long long checksum = 0;

                    for (int i = 0; i < warm + iters; ++i)
                    {
                        const double a = NowMs();
                        const int rc = pDec(h, jpeg.data(), jpeg.size(), dst.data(),
                                            w, w * 3, hgt, TJPF_BGR, TJFLAG_FASTDCT);
                        const double b = NowMs();
                        if (rc != 0)
                            break;
                        ++ok;
                        if (i == warm - 1)
                            for (size_t k = 0; k < dst.size(); k += 9991)
                                checksum += dst[k];
                        if (i >= warm)
                            sumMs += (b - a);
                    }
                    if (ok > warm)
                        printf("I. turbojpeg 整帧    : OK    host=%.2f ms  (%.0f fps, %dx%d BGR, 校验和 %llu)\n",
                               sumMs / iters, 1000.0 / (sumMs / iters), w, hgt, checksum);
                    else
                        printf("I. turbojpeg 整帧    : 解码失败\n");
                }
                else
                {
                    printf("I. turbojpeg 整帧    : 读头失败\n");
                }
                if (h) pFree(h);
            }
            FreeLibrary(dll);
        }
    }

    printf("\n说明:\n");
    printf("  host     = 纯主机侧调用时间 (不含 GPU 同步), 决定 CPU 占用\n");
    printf("  host+sync= 单帧端到端延迟 (串行); 流水线吞吐看 host 与 GPU 时间的较大者\n");
    printf("  本机只有 4 核: host 时间是关键指标, 它直接和 TensorRT 推理抢核\n");
    return 0;
}
