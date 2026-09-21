
// NVDEC MJPEG 解码基准 —— 量硬解到底能不能替掉 CPU 侧 6~7ms/帧的 nvJPEG 熵解码。
//
// 背景: 采集侧天花板(实测 211~227fps)是 CPU 的 JPEG 熵解码压出来的。
//   nvJPEG 的 GPU-Huffman 只在 batch>50 时生效(对瞄准 = 208ms 延迟, 不可用),
//   NVJPG 后端本机不可用(status=7), 但 nvdec_probe 已确认:
//     NVDEC(nvcuvid) 支持 cudaVideoCodec_JPEG, 1 个解码引擎, 最大 32768x16384。
//   所以这条路是"位流进、像素出、CPU 零参与"。
//
// 本工具用真实采集帧跑:
//   cuvidCreateVideoParser(JPEG) -> cuvidCreateDecoder -> 每帧 cuvidParseVideoData
//   -> displayPicture 回调里 cuvidMapVideoFrame 拿到解码后的 NV12
// 报出 ms/帧, 与 nvjpeg_bench 的 split+ROI GPU_HYBRID(6.07ms) 直接对比。
//
// 用法: nvdec_bench <frame.jpg> [迭代次数]

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cuda_runtime.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

namespace
{

// ── CUVID 定义: 逐字段抄自 Apotheosis/modules/nv-codec-headers/ffnvcodec/ ──
typedef int CUresult;
typedef void* CUvideodecoder;
typedef void* CUvideoparser;
typedef void* CUvideoctxlock;
typedef unsigned long tcu_ulong;
typedef long long CUvideotimestamp;

enum cudaVideoCodec { cudaVideoCodec_MPEG1 = 0, cudaVideoCodec_MPEG2, cudaVideoCodec_MPEG4,
                      cudaVideoCodec_VC1, cudaVideoCodec_H264, cudaVideoCodec_JPEG };
enum cudaVideoChromaFormat { cudaVideoChromaFormat_Monochrome = 0, cudaVideoChromaFormat_420,
                             cudaVideoChromaFormat_422, cudaVideoChromaFormat_444 };
enum cudaVideoSurfaceFormat { cudaVideoSurfaceFormat_NV12 = 0, cudaVideoSurfaceFormat_P016,
                              cudaVideoSurfaceFormat_YUV444, cudaVideoSurfaceFormat_YUV444_16Bit };
enum cudaVideoDeinterlaceMode { cudaVideoDeinterlaceMode_Weave = 0, cudaVideoDeinterlaceMode_Bob,
                                cudaVideoDeinterlaceMode_Adaptive };
enum cudaVideoCreateFlags { cudaVideoCreate_Default = 0x00, cudaVideoCreate_PreferCUDA = 0x01,
                            cudaVideoCreate_PreferDXVA = 0x02, cudaVideoCreate_PreferCUVID = 0x04 };
enum CUvideopacketflags { CUVID_PKT_ENDOFSTREAM = 0x01, CUVID_PKT_TIMESTAMP = 0x02,
                          CUVID_PKT_DISCONTINUITY = 0x04, CUVID_PKT_ENDOFPICTURE = 0x08 };

struct CUVIDDECODECREATEINFO
{
    tcu_ulong ulWidth, ulHeight, ulNumDecodeSurfaces;
    int       CodecType, ChromaFormat;
    tcu_ulong ulCreationFlags, bitDepthMinus8, ulIntraDecodeOnly, ulMaxWidth, ulMaxHeight, Reserved1;
    struct { short left, top, right, bottom; } display_area;
    int       OutputFormat, DeinterlaceMode;
    tcu_ulong ulTargetWidth, ulTargetHeight, ulNumOutputSurfaces;
    CUvideoctxlock vidLock;
    struct { short left, top, right, bottom; } target_rect;
    tcu_ulong enableHistogram, Reserved2[4];
};

struct CUVIDJPEGPICPARAMS { int Reserved; };

struct CUVIDPICPARAMS
{
    int PicWidthInMbs, FrameHeightInMbs, CurrPicIdx, field_pic_flag, bottom_field_flag, second_field;
    unsigned int nBitstreamDataLen;
    const unsigned char* pBitstreamData;
    unsigned int nNumSlices;
    const unsigned int* pSliceDataOffsets;
    int ref_pic_flag, intra_pic_flag;
    unsigned int Reserved[30];
    union { CUVIDJPEGPICPARAMS jpeg; unsigned int CodecReserved[1024]; } CodecSpecific;
};

struct CUVIDPROCPARAMS
{
    int progressive_frame, second_field, top_field_first, unpaired_field;
    unsigned int reserved_flags, reserved_zero;
    unsigned long long raw_input_dptr;
    unsigned int raw_input_pitch, raw_input_format;
    unsigned long long raw_output_dptr;
    unsigned int raw_output_pitch, Reserved1;
    cudaStream_t output_stream;
    unsigned int Reserved[46];
    unsigned long long* histogram_dptr;
    void* Reserved2[1];
};

struct CUVIDPARSERDISPINFO
{
    int picture_index, progressive_frame, top_field_first, repeat_first_field;
    CUvideotimestamp timestamp;
};

struct CUVIDSOURCEDATAPACKET
{
    tcu_ulong flags, payload_size;
    const unsigned char* payload;
    CUvideotimestamp timestamp;
};

typedef int(__stdcall* PFNVIDSEQUENCECALLBACK)(void*, void*);
typedef int(__stdcall* PFNVIDDECODECALLBACK)(void*, CUVIDPICPARAMS*);
typedef int(__stdcall* PFNVIDDISPLAYCALLBACK)(void*, CUVIDPARSERDISPINFO*);

struct CUVIDPARSERPARAMS
{
    int CodecType;
    unsigned int ulMaxNumDecodeSurfaces, ulClockRate, ulErrorThreshold, ulMaxDisplayDelay;
    unsigned int bAnnexb : 1, uReserved : 31;
    unsigned int uReserved1[4];
    void* pUserData;
    PFNVIDSEQUENCECALLBACK pfnSequenceCallback;
    PFNVIDDECODECALLBACK   pfnDecodePicture;
    PFNVIDDISPLAYCALLBACK  pfnDisplayPicture;
    void* pfnGetOperatingPoint;
    void* pfnGetSEIMsg;
    void* pvReserved2[5];
    void* pExtVideoInfo;
};

// 函数签名 (dynlink_cuviddec.h L1060-1129 / dynlink_nvcuvid.h L524-536)
typedef CUresult(__stdcall* tCreateDecoder)(CUvideodecoder*, CUVIDDECODECREATEINFO*);
typedef CUresult(__stdcall* tDestroyDecoder)(CUvideodecoder);
typedef CUresult(__stdcall* tCreateVideoParser)(CUvideoparser*, CUVIDPARSERPARAMS*);
typedef CUresult(__stdcall* tParseVideoData)(CUvideoparser, CUVIDSOURCEDATAPACKET*);
typedef CUresult(__stdcall* tDestroyVideoParser)(CUvideoparser);
typedef CUresult(__stdcall* tMapVideoFrame64)(CUvideodecoder, int, unsigned long long*,
                                              unsigned int*, CUVIDPROCPARAMS*);
typedef CUresult(__stdcall* tUnmapVideoFrame64)(CUvideodecoder, unsigned long long);
typedef CUresult(__stdcall* tCtxLockCreate)(CUvideoctxlock*, void*);

struct Loader
{
    HMODULE              dll = nullptr;
    tCreateDecoder       createDecoder = nullptr;
    tDestroyDecoder      destroyDecoder = nullptr;
    tCreateVideoParser   createParser = nullptr;
    tParseVideoData      parseVideoData = nullptr;
    tDestroyVideoParser  destroyParser = nullptr;
    tMapVideoFrame64     mapVideoFrame = nullptr;
    tUnmapVideoFrame64   unmapVideoFrame = nullptr;
    tCtxLockCreate       ctxLockCreate = nullptr;

    bool load()
    {
        dll = LoadLibraryA("nvcuvid.dll");
        if (!dll) return false;
        auto R = [this](const char* n) { return GetProcAddress(dll, n); };
        createDecoder    = reinterpret_cast<tCreateDecoder>(R("cuvidCreateDecoder"));
        destroyDecoder   = reinterpret_cast<tDestroyDecoder>(R("cuvidDestroyDecoder"));
        createParser     = reinterpret_cast<tCreateVideoParser>(R("cuvidCreateVideoParser"));
        parseVideoData   = reinterpret_cast<tParseVideoData>(R("cuvidParseVideoData"));
        destroyParser    = reinterpret_cast<tDestroyVideoParser>(R("cuvidDestroyVideoParser"));
        mapVideoFrame    = reinterpret_cast<tMapVideoFrame64>(R("cuvidMapVideoFrame64"));
        unmapVideoFrame  = reinterpret_cast<tUnmapVideoFrame64>(R("cuvidUnmapVideoFrame64"));
        ctxLockCreate    = reinterpret_cast<tCtxLockCreate>(R("cuvidCtxLockCreate"));
        return createDecoder && destroyDecoder && createParser && parseVideoData
            && destroyParser && mapVideoFrame && unmapVideoFrame;
    }
};

double NowMs()
{
    using clock = std::chrono::steady_clock;
    static const clock::time_point start = clock::now();
    return std::chrono::duration<double, std::milli>(clock::now() - start).count();
}

struct Bench
{
    Loader        api;
    CUvideodecoder decoder = nullptr;
    CUvideoctxlock lock = nullptr;
    cudaStream_t  stream = nullptr;

    long long decoded = 0;
    long long mapFail = 0;
    double    mapSumMs = 0.0;
    int       frameW = 0, frameH = 0;
    int       displayCalls = 0;
};

int __stdcall onSequence(void* user, void* fmt)
{
    // 返回 >=1 表示接受这个序列参数。
    (void)user; (void)fmt;
    return 1;
}

int __stdcall onDecode(void* user, CUVIDPICPARAMS* pic)
{
    Bench* b = static_cast<Bench*>(user);
    if (!b || !b->decoder || !pic) return 0;
    // 解析器已经解析好了, 这里把这一帧交给硬件解码器。
    typedef CUresult(__stdcall* tDecPicture)(CUvideodecoder, CUVIDPICPARAMS*);
    static tDecPicture dec = reinterpret_cast<tDecPicture>(
        GetProcAddress(GetModuleHandleA("nvcuvid.dll"), "cuvidDecodePicture"));
    if (!dec) return 0;
    const CUresult r = dec(b->decoder, pic);
    return (r == 0) ? 1 : 0;
}

int __stdcall onDisplay(void* user, CUVIDPARSERDISPINFO* info)
{
    Bench* b = static_cast<Bench*>(user);
    if (!b || !b->decoder || !info) return 1;
    ++b->displayCalls;

    const double t0 = NowMs();
    unsigned long long devPtr = 0;
    unsigned int       pitch = 0;
    CUVIDPROCPARAMS    proc{};
    proc.progressive_frame = 1;
    proc.output_stream = b->stream;   // 解码挂到我们的 stream 上

    const CUresult r = b->api.mapVideoFrame(b->decoder, info->picture_index, &devPtr, &pitch, &proc);
    if (r == 0)
    {
        // 这里拿到的是 NV12 的 CUdeviceptr + pitch。真实管线要接 NPP 做
        // NV12->BGR, 但那一步现有 nvJPEG 路径同样要付, 所以不算增量成本。
        if (b->frameW == 0) { b->frameW = static_cast<int>(pitch); b->frameH = 1080; }
        b->api.unmapVideoFrame(b->decoder, devPtr);
        ++b->decoded;
    }
    else
    {
        ++b->mapFail;
    }
    b->mapSumMs += (NowMs() - t0);
    return 1;
}

// 全局单例: main 和回调都要用同一个实例
Bench g_bench;

} // namespace

int main(int argc, char** argv)
{
    SetConsoleOutputCP(CP_UTF8);
    setvbuf(stdout, nullptr, _IONBF, 0);

    const char* path  = (argc > 1) ? argv[1] : "build\\diag\\live_frame.jpg";
    const int   iters = (argc > 2) ? std::atoi(argv[2]) : 300;

    printf("=== NVDEC MJPEG 解码基准 ===\n");

    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in)
    {
        printf("读不到帧文件: %s\n", path);
        return 2;
    }
    const std::streamsize size = in.tellg();
    in.seekg(0, std::ios::beg);
    std::vector<unsigned char> jpeg(static_cast<size_t>(size));
    in.read(reinterpret_cast<char*>(jpeg.data()), size);
    if (!in || jpeg.empty())
    {
        printf("读取失败\n");
        return 2;
    }
    printf("样本帧: %s  (%zu 字节, %.1f KB)   迭代 %d\n\n", path, jpeg.size(), jpeg.size() / 1024.0, iters);

    if (cudaSetDevice(0) != cudaSuccess) { printf("[FAIL] cudaSetDevice\n"); return 3; }
    cudaFree(nullptr);
    cudaStreamCreateWithFlags(&g_bench.stream, cudaStreamNonBlocking);

    if (!g_bench.api.load())
    {
        printf("[FAIL] nvcuvid.dll 加载或导出解析失败\n");
        return 4;
    }

    // 解码器
    CUVIDDECODECREATEINFO dci{};
    dci.ulWidth = 1920;
    dci.ulHeight = 1080;
    dci.ulNumDecodeSurfaces = 8;
    dci.CodecType = cudaVideoCodec_JPEG;
    dci.ChromaFormat = cudaVideoChromaFormat_420;
    dci.ulCreationFlags = cudaVideoCreate_PreferCUVID;   // 用专用视频引擎
    dci.bitDepthMinus8 = 0;
    dci.OutputFormat = cudaVideoSurfaceFormat_NV12;
    dci.DeinterlaceMode = cudaVideoDeinterlaceMode_Weave;
    dci.ulTargetWidth = 1920;
    dci.ulTargetHeight = 1080;
    dci.ulNumOutputSurfaces = 2;

    CUresult r = g_bench.api.createDecoder(&g_bench.decoder, &dci);
    if (r != 0 || !g_bench.decoder)
    {
        printf("[FAIL] cuvidCreateDecoder 失败 (CUresult=%d)\n", r);
        printf("       → NVDEC 虽然声明支持 MJPEG, 但解码器建不起来, 这条路断了。\n");
        return 5;
    }
    printf("[ok] NVDEC JPEG 解码器已创建 (1920x1080, 4:2:0, NV12 输出)\n");

    // 解析器
    CUVIDPARSERPARAMS pp{};
    pp.CodecType = cudaVideoCodec_JPEG;
    pp.ulMaxNumDecodeSurfaces = 8;
    pp.ulClockRate = 10000000;
    pp.ulErrorThreshold = 0;          // 不因为"疑似损坏"就跳过解码
    pp.ulMaxDisplayDelay = 0;         // 低延迟: 不排队
    pp.pUserData = &g_bench;
    pp.pfnSequenceCallback = onSequence;
    pp.pfnDecodePicture = onDecode;
    pp.pfnDisplayPicture = onDisplay;

    CUvideoparser parser = nullptr;
    r = g_bench.api.createParser(&parser, &pp);
    if (r != 0 || !parser)
    {
        printf("[FAIL] cuvidCreateVideoParser 失败 (CUresult=%d)\n", r);
        g_bench.api.destroyDecoder(g_bench.decoder);
        return 6;
    }
    printf("[ok] 解析器已创建 (JPEG, display delay=0)\n\n");

    // 预热
    const int warm = 5;
    for (int i = 0; i < warm; ++i)
    {
        CUVIDSOURCEDATAPACKET pkt{};
        pkt.flags = CUVID_PKT_ENDOFPICTURE;   // 一个包 = 一张完整 JPEG
        pkt.payload_size = jpeg.size();
        pkt.payload = jpeg.data();
        g_bench.api.parseVideoData(parser, &pkt);
    }
    cudaStreamSynchronize(g_bench.stream);

    g_bench.decoded = 0;
    g_bench.mapSumMs = 0.0;
    g_bench.displayCalls = 0;

    const double t0 = NowMs();
    for (int i = 0; i < iters; ++i)
    {
        CUVIDSOURCEDATAPACKET pkt{};
        pkt.flags = CUVID_PKT_ENDOFPICTURE;
        pkt.payload_size = jpeg.size();
        pkt.payload = jpeg.data();
        g_bench.api.parseVideoData(parser, &pkt);
    }
    const double hostMs = NowMs() - t0;
    cudaStreamSynchronize(g_bench.stream);
    const double totalMs = NowMs() - t0;

    printf("=== 结果 ===\n");
    printf("  迭代           : %d\n", iters);
    printf("  display 回调   : %d 次\n", g_bench.displayCalls);
    printf("  成功解码并映射 : %lld 帧\n", g_bench.decoded);
    printf("  映射失败       : %lld 次\n", g_bench.mapFail);
    printf("  parse 调用耗时 : %.2f ms/帧   (主机侧, 含同步回调)\n", hostMs / iters);
    printf("  parse+同步总计 : %.2f ms/帧\n", totalMs / iters);
    if (g_bench.displayCalls > 0)
        printf("  map/unmap 耗时 : %.3f ms/帧\n", g_bench.mapSumMs / g_bench.displayCalls);

    const double perFrame = totalMs / iters;
    printf("\n=== 与现状对比 ===\n");
    printf("  nvJPEG split+ROI GPU_HYBRID (现状): 6.07 ms/帧\n");
    printf("  NVDEC 硬解                        : %.2f ms/帧\n", perFrame);
    if (perFrame > 0.0 && g_bench.decoded > 0)
    {
        const double speedup = 6.07 / perFrame;
        printf("  → 快 %.2f 倍\n\n", speedup);
        if (perFrame <= 4.17)
            printf("  >>> 单帧 %.2fms 已低于 240fps 的 4.17ms 预算。\n"
                   "      采集侧天花板可以从 211~227fps 抬到 240 以上 —— 值得做完整集成。\n", perFrame);
        else
            printf("  >>> 单帧 %.2fms 仍高于 4.17ms 预算, 采集侧依然是瓶颈。\n", perFrame);
    }
    if (g_bench.decoded == 0)
        printf("\n  ⚠ 一帧都没解出来: NVDEC 声明支持 MJPEG 但这条路径实际不通。\n");

    g_bench.api.destroyParser(parser);
    g_bench.api.destroyDecoder(g_bench.decoder);
    cudaStreamDestroy(g_bench.stream);
    return 0;
}
