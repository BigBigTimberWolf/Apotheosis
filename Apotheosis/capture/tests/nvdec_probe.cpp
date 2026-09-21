
// NVDEC 能力探针 —— 查这张卡能不能用硬件解码 MJPEG。
//
// 动机: MJPEG 240fps 的解码瓶颈全在 CPU 的 JPEG 熵解码(6~7ms/帧), 而 nvJPEG 的
//   GPU-Huffman 只在 batch>50 时才生效(对瞄准来说等于 208ms 延迟, 不可用)。
//   唯一的硬件出路是 NVDEC(nvcuvid) 的 JPEG/MJPEG 解码器 —— 位流进、像素出,
//   CPU 零参与。
//
//   但 CMP 40HX 是矿卡, 媒体引擎可能被阉割(NVJPG 已确认不可用: status=7)。
//   所以这里必须带对照组: 同时查 H264/HEVC。
//     全都不支持  -> 这张卡的 NVDEC 整个没有
//     只有 JPEG 不支持 -> 有 NVDEC 但缺 MJPEG 解码器
//     JPEG 支持    -> 有救, 可以拿它替掉 CPU 解码
//
// 用法: nvdec_probe

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cuda_runtime.h>

#include <cstdio>
#include <cstring>

namespace
{

typedef int CUresult;
typedef void* CUvideodecoder;

// 取自 Apotheosis/modules/nv-codec-headers/ffnvcodec/dynlink_cuviddec.h
enum cudaVideoCodec
{
    cudaVideoCodec_MPEG1 = 0,
    cudaVideoCodec_MPEG2,
    cudaVideoCodec_MPEG4,
    cudaVideoCodec_VC1,
    cudaVideoCodec_H264,
    cudaVideoCodec_JPEG,      // = 5
    cudaVideoCodec_H264_SVC,
    cudaVideoCodec_H264_MVC,
    cudaVideoCodec_HEVC,
    cudaVideoCodec_VP8,
    cudaVideoCodec_VP9,
    cudaVideoCodec_AV1,
};

enum cudaVideoChromaFormat
{
    cudaVideoChromaFormat_Monochrome = 0,
    cudaVideoChromaFormat_420,
    cudaVideoChromaFormat_422,
    cudaVideoChromaFormat_444,
};

// 逐字段对齐 dynlink_cuviddec.h 的 CUVIDDECODECAPS (别改顺序/类型)
struct CUVIDDECODECAPS
{
    int          eCodecType;          // IN
    int          eChromaFormat;       // IN
    unsigned int nBitDepthMinus8;     // IN
    unsigned int reserved1[3];

    unsigned char  bIsSupported;      // OUT
    unsigned char  nNumNVDECs;        // OUT
    unsigned short nOutputFormatMask; // OUT
    unsigned int   nMaxWidth;         // OUT
    unsigned int   nMaxHeight;        // OUT
    unsigned int   nMaxMBCount;       // OUT
    unsigned short nMinWidth;         // OUT
    unsigned short nMinHeight;        // OUT
    unsigned char  bIsHistogramSupported;
    unsigned char  nCounterBitDepth;
    unsigned short nMaxHistogramBins;
    unsigned int   reserved3[10];
};

typedef CUresult(__stdcall* tCuvidGetDecoderCaps)(CUVIDDECODECAPS* pdc);

struct CodecCase { int codec; const char* name; };

} // namespace

int main()
{
    SetConsoleOutputCP(CP_UTF8);
    setvbuf(stdout, nullptr, _IONBF, 0);

    printf("=== NVDEC 硬件解码能力探针 ===\n\n");

    // 先建 CUDA 上下文 —— nvcuvid 需要它
    if (cudaSetDevice(0) != cudaSuccess)
    {
        printf("[FAIL] cudaSetDevice 失败\n");
        return 2;
    }
    cudaFree(nullptr);   // 触发上下文创建

    cudaDeviceProp props{};
    int driverVer = 0;
    cudaDriverGetVersion(&driverVer);
    if (cudaGetDeviceProperties(&props, 0) == cudaSuccess)
        printf("GPU: %s  (sm_%d%d, CUDA 驱动版本 %d)\n\n",
               props.name, props.major, props.minor, driverVer);

    HMODULE dll = LoadLibraryA("nvcuvid.dll");
    if (!dll)
        dll = LoadLibraryA("C:\\Windows\\System32\\nvcuvid.dll");
    if (!dll)
    {
        printf("[FAIL] 加载 nvcuvid.dll 失败 (GetLastError=%lu)\n", GetLastError());
        printf("       这说明系统里没有 NVDEC 运行时 —— 硬件解码这条路不存在。\n");
        return 3;
    }
    printf("[ok] nvcuvid.dll 已加载\n\n");

    auto getCaps = reinterpret_cast<tCuvidGetDecoderCaps>(
        GetProcAddress(dll, "cuvidGetDecoderCaps"));
    if (!getCaps)
    {
        printf("[FAIL] 找不到 cuvidGetDecoderCaps 导出\n");
        FreeLibrary(dll);
        return 4;
    }

    const CodecCase cases[] = {
        { cudaVideoCodec_H264, "H264 (对照组)" },
        { cudaVideoCodec_HEVC, "HEVC (对照组)" },
        { cudaVideoCodec_JPEG, "JPEG/MJPEG ★ 我们要的" },
    };

    printf("%-24s %-10s %-8s %-12s %-12s %s\n",
           "编解码器", "支持?", "NVDEC数", "最大宽", "最大高", "最大宏块数");
    printf("--------------------------------------------------------------------------\n");

    int jpegSupported = -1;
    for (const CodecCase& c : cases)
    {
        CUVIDDECODECAPS caps{};
        caps.eCodecType = c.codec;
        caps.eChromaFormat = cudaVideoChromaFormat_420;
        caps.nBitDepthMinus8 = 0;

        const CUresult res = getCaps(&caps);
        if (res != 0)
        {
            printf("%-24s 查询失败 (CUresult=%d)\n", c.name, res);
            if (c.codec == cudaVideoCodec_JPEG) jpegSupported = 0;
            continue;
        }

        printf("%-24s %-10s %-8u %-12u %-12u %u\n",
               c.name,
               caps.bIsSupported ? "支持" : "不支持",
               static_cast<unsigned>(caps.nNumNVDECs),
               caps.nMaxWidth, caps.nMaxHeight, caps.nMaxMBCount);

        if (c.codec == cudaVideoCodec_JPEG)
            jpegSupported = caps.bIsSupported ? 1 : 0;
    }

    FreeLibrary(dll);

    printf("\n=== 判读 ===\n");
    if (jpegSupported == 1)
    {
        printf(">>> NVDEC 支持 MJPEG!\n");
        printf("    可以拿它替掉 CPU 侧 6~7ms/帧的 nvJPEG 熵解码, 采集侧天花板\n");
        printf("    就能从 211~227fps 抬到远高于 240 —— 这是唯一还没堵死的硬件出路。\n");
        printf("    下一步: 用 cuvidCreateDecoder + cuvidDecodePicture 做真实解码计时。\n");
    }
    else if (jpegSupported == 0)
    {
        printf(">>> NVDEC 存在, 但不支持 MJPEG。\n");
        printf("    注意: 硬件 JPEG 解码在部分驱动/卡型上就是被裁掉的。\n");
    }
    else
    {
        printf(">>> MJPEG 查询没拿到结果, 看上面的报错。\n");
    }
    return 0;
}
