#define WIN32_LEAN_AND_MEAN
#define _WINSOCKAPI_
#include <winsock2.h>
#include <Windows.h>
#include <iostream>
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <filesystem>
#include <limits>
#include <memory>
#include <string>
#include <vector>
#include <NvOnnxParser.h>
#include <cuda_runtime.h>
#include <cuda_fp16.h>
#include <opencv2/opencv.hpp>

#include "nvinf.h"
#include "Apotheosis.h"
#include "trt_monitor.h"

Logger gLogger;

void Logger::log(nvinfer1::ILogger::Severity severity, const char* msg) noexcept
{
    if (severity <= nvinfer1::ILogger::Severity::kWARNING)
    {
        std::string devMsg = msg;

        std::string magicTag = "Serialization assertion plan->header.magicTag == rt::kPLAN_MAGIC_TAG failed.";
        std::string old_deserialization = "Using old deserialization call on a weight-separated plan file.";
        if (devMsg.find(magicTag) != std::string::npos || devMsg.find(old_deserialization) != std::string::npos)
        {
            std::cout << "[TensorRT] ERROR: This engine model is not suitable for execution. Please delete this engine model and set the ONNX version of this model in the settings. The program will export the model automatically." << std::endl;
        }
        else
        {
            std::cout << "[TensorRT] " << severityLevelName(severity) << ": " << msg << std::endl;
        }
    }
}

const char* Logger::severityLevelName(nvinfer1::ILogger::Severity severity)
{
    switch (severity)
    {
        case nvinfer1::ILogger::Severity::kINTERNAL_ERROR: return "INTERNAL_ERROR";
        case nvinfer1::ILogger::Severity::kERROR:          return "ERROR";
        case nvinfer1::ILogger::Severity::kWARNING:        return "WARNING";
        case nvinfer1::ILogger::Severity::kINFO:           return "INFO";
        case nvinfer1::ILogger::Severity::kVERBOSE:        return "VERBOSE";
        default:                                           return "UNKNOWN";
    }
}

nvinfer1::IBuilder* createInferBuilder()
{
    return nvinfer1::createInferBuilder(gLogger);
}

nvinfer1::INetworkDefinition* createNetwork(nvinfer1::IBuilder* builder)
{
    const auto explicitBatch = 1U << static_cast<uint32_t>(nvinfer1::NetworkDefinitionCreationFlag::kEXPLICIT_BATCH);
    return builder->createNetworkV2(explicitBatch);
}

nvinfer1::IBuilderConfig* createBuilderConfig(nvinfer1::IBuilder* builder)
{
    return builder->createBuilderConfig();
}

nvinfer1::ICudaEngine* loadEngineFromFile(const std::string& engineFile, nvinfer1::IRuntime* runtime)
{
    std::ifstream file(std::filesystem::u8path(engineFile), std::ios::binary);
    if (!file.good())
    {
        std::cerr << "[TensorRT] Error opening the engine file: " << engineFile << std::endl;
        return nullptr;
    }

    file.seekg(0, std::ios::end);
    size_t size = file.tellg();
    file.seekg(0, std::ios::beg);
    std::vector<char> engineData(size);
    file.read(engineData.data(), size);
    file.close();

    nvinfer1::ICudaEngine* engine = runtime->deserializeCudaEngine(engineData.data(), size);
    if (!engine)
    {
        std::cerr << "[TensorRT] Engine deserialization error from file: " << engineFile << std::endl;
        return nullptr;
    }

    if (config.verbose)
    {
        std::cout << "[TensorRT] The engine was successfully loaded from the file: " << engineFile << std::endl;
    }
    return engine;
}

nvinfer1::ICudaEngine* loadEngineFromMemory(const void* data, size_t size, nvinfer1::IRuntime* runtime)
{
    if (!data || size == 0 || !runtime)
    {
        std::cerr << "[TensorRT] Invalid engine memory buffer" << std::endl;
        return nullptr;
    }

    nvinfer1::ICudaEngine* engine = runtime->deserializeCudaEngine(data, size);
    if (!engine)
    {
        std::cerr << "[TensorRT] Engine deserialization error from memory" << std::endl;
        return nullptr;
    }
    return engine;
}

namespace
{

// ── INT8 熵校准器 ───────────────────────────────────────────────────────────
// 必须严格复现 cuda_preprocess.cu 喂给网络的张量格式, 否则校准出来的量化区间
// 会系统性偏移:
//   BGR uint8 图 -> 双线性缩放到 side×side -> 每个分量 /255
//   -> 通道重排成 RGB -> NCHW half
// 校准图来源: config.int8_calib_dir (相对 exe 目录或绝对路径)。
// 缓存: 第一次校准后把校准表写到 cachePath, 之后直接复用 (省掉整整一次校准)。
class Int8EntropyCalibrator : public nvinfer1::IInt8EntropyCalibrator2
{
public:
    Int8EntropyCalibrator(int side, const std::string& dir, int maxImages,
                          const std::string& cachePath)
        : side_(std::max(1, side))
        , maxImages_(std::max(1, maxImages))
        , cachePath_(cachePath)
    {
        std::error_code ec;   // 注意: 这几个重载要非常量引用, 别写成 const
        if (std::filesystem::is_directory(std::filesystem::u8path(dir), ec))
        {
            for (const auto& entry : std::filesystem::directory_iterator(std::filesystem::u8path(dir), ec))
            {
                if (!entry.is_regular_file(ec)) continue;
                std::string ext = entry.path().extension().u8string();
                std::transform(ext.begin(), ext.end(), ext.begin(),
                               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                if (ext == ".jpg" || ext == ".jpeg" || ext == ".png" || ext == ".bmp")
                    files_.push_back(entry.path().u8string());
            }
        }
        std::sort(files_.begin(), files_.end());
        if (static_cast<int>(files_.size()) > maxImages_)
            files_.resize(static_cast<size_t>(maxImages_));

        const size_t bytes = static_cast<size_t>(3) * side_ * side_ * sizeof(__half);
        if (cudaMalloc(&deviceInput_, bytes) != cudaSuccess)
            deviceInput_ = nullptr;
    }

    ~Int8EntropyCalibrator() override
    {
        if (deviceInput_) cudaFree(deviceInput_);
    }

    Int8EntropyCalibrator(const Int8EntropyCalibrator&) = delete;
    Int8EntropyCalibrator& operator=(const Int8EntropyCalibrator&) = delete;

    int  getBatchSize() const noexcept override { return 1; }   // 网络 batch 维恒为 1
    bool usable() const { return deviceInput_ != nullptr && !files_.empty(); }
    size_t imageCount() const { return files_.size(); }

    bool getBatch(void* bindings[], const char* names[], int nbBindings) noexcept override
    {
        (void)names;
        if (!deviceInput_ || files_.empty() || nbBindings < 1)
            return false;
        if (next_ >= files_.size())
            return false;                     // 一轮跑完, TRT 会停止调用

        const std::string& path = files_[next_++];
        cv::Mat img = cv::imread(path, cv::IMREAD_COLOR);
        if (img.empty())
        {
            std::cerr << "[INT8 Calib] 读图失败, 跳过: " << path << std::endl;
            return getBatch(bindings, names, nbBindings);   // 换下一张
        }

        cv::Mat resized;
        cv::resize(img, resized, cv::Size(side_, side_), 0, 0, cv::INTER_LINEAR);

        std::vector<__half> host(static_cast<size_t>(3) * side_ * side_);
        const int hw = side_ * side_;
        constexpr float kInv255 = 1.0f / 255.0f;
        for (int y = 0; y < side_; ++y)
        {
            const cv::Vec3b* row = resized.ptr<cv::Vec3b>(y);
            for (int x = 0; x < side_; ++x)
            {
                const cv::Vec3b p = row[x];
                // OpenCV 读进来是 BGR, 网络要的是 RGB
                host[0 * hw + y * side_ + x] = __float2half(p[2] * kInv255);
                host[1 * hw + y * side_ + x] = __float2half(p[1] * kInv255);
                host[2 * hw + y * side_ + x] = __float2half(p[0] * kInv255);
            }
        }
        if (cudaMemcpy(deviceInput_, host.data(), host.size() * sizeof(__half),
                       cudaMemcpyHostToDevice) != cudaSuccess)
            return false;

        bindings[0] = deviceInput_;
        return true;
    }

    const void* readCalibrationCache(std::size_t& length) noexcept override
    {
        cache_.clear();
        std::ifstream in(std::filesystem::u8path(cachePath_), std::ios::binary);
        if (!in.good()) { length = 0; return nullptr; }
        in.seekg(0, std::ios::end);
        const std::streamsize n = in.tellg();
        in.seekg(0, std::ios::beg);
        if (n <= 0) { length = 0; return nullptr; }
        cache_.resize(static_cast<size_t>(n));
        in.read(reinterpret_cast<char*>(cache_.data()), n);
        if (!in) { length = 0; return nullptr; }
        std::cout << "[INT8 Calib] 命中校准缓存: " << cachePath_
                  << " (" << cache_.size() << " 字节)" << std::endl;
        length = cache_.size();
        return cache_.data();
    }

    void writeCalibrationCache(const void* cache, std::size_t length) noexcept override
    {
        std::ofstream out(std::filesystem::u8path(cachePath_), std::ios::binary);
        if (!out.good()) return;
        out.write(static_cast<const char*>(cache), static_cast<std::streamsize>(length));
        std::cout << "[INT8 Calib] 校准表已写入: " << cachePath_ << std::endl;
    }

private:
    int side_ = 1;
    int maxImages_ = 200;
    std::string cachePath_;
    std::vector<std::string> files_;
    size_t next_ = 0;
    void* deviceInput_ = nullptr;
    std::vector<char> cache_;
};

// 判断当前 GPU 有没有 INT8 tensor core。Turing(sm_75)/Volta(sm_70) 及以上有;
// 更老的卡只能跑 DLA 或直接不支持, 这里明确拒绝而不是让它偷偷降级。
bool gpuSupportsInt8TensorCore(std::string& why)
{
    int dev = 0;
    if (cudaGetDevice(&dev) != cudaSuccess)
    {
        why = "拿不到 CUDA 设备";
        return false;
    }
    cudaDeviceProp props{};
    if (cudaGetDeviceProperties(&props, dev) != cudaSuccess)
    {
        why = "拿不到设备属性";
        return false;
    }
    const int cc = props.major * 10 + props.minor;
    if (cc < 70)
    {
        why = "compute capability " + std::to_string(props.major) + "." + std::to_string(props.minor)
            + " 没有 INT8 tensor core (需要 sm_70+)";
        return false;
    }
    why = std::string(props.name) + " (sm_" + std::to_string(cc) + ")";
    return true;
}

std::unique_ptr<nvinfer1::IHostMemory> buildSerializedEngine(nvinfer1::INetworkDefinition* network,
                                                            nvinfer1::IBuilder* builder,
                                                            nvinfer1::IBuilderConfig* cfg)
{
    nvinfer1::ITensor* inputTensor = network->getInput(0);
    if (!inputTensor)
    {
        std::cerr << "[TensorRT] ERROR: ONNX model has no input tensor" << std::endl;
        return nullptr;
    }
    const char* inName = inputTensor->getName();

    for (int i = 0; i < network->getNbInputs(); ++i)
    {
        nvinfer1::ITensor* t = network->getInput(i);
        if (t) t->setType(nvinfer1::DataType::kHALF);
    }
    for (int i = 0; i < network->getNbOutputs(); ++i)
    {
        nvinfer1::ITensor* t = network->getOutput(i);
        if (t) t->setType(nvinfer1::DataType::kHALF);
    }

    nvinfer1::Dims inDims = inputTensor->getDimensions();
    auto dimension_to_int = [](std::int64_t value) -> int {
        if (value < -1 || value > static_cast<std::int64_t>(std::numeric_limits<int>::max()))
            return -1;
        return static_cast<int>(value);
    };
    int H = (inDims.nbDims >= 4) ? dimension_to_int(inDims.d[2]) : -1;
    int W = (inDims.nbDims >= 4) ? dimension_to_int(inDims.d[3]) : -1;

    bool fixedByModel = (H > 0 && W > 0);
    bool fixedByConfig = config.fixed_input_size;
    bool makeStatic = fixedByModel || fixedByConfig;

    if (fixedByConfig && (H <= 0 || W <= 0))
        H = W = config.detection_resolution;

    nvinfer1::IOptimizationProfile* profile = builder->createOptimizationProfile();
    if (makeStatic)
    {
        nvinfer1::Dims4 d{ 1, 3, H, W };
        profile->setDimensions(inName, nvinfer1::OptProfileSelector::kMIN, d);
        profile->setDimensions(inName, nvinfer1::OptProfileSelector::kOPT, d);
        profile->setDimensions(inName, nvinfer1::OptProfileSelector::kMAX, d);
        if (config.verbose)
            std::cout << "[TensorRT] Static profile " << H << "x" << W << std::endl;
    }
    else
    {
        profile->setDimensions(inName, nvinfer1::OptProfileSelector::kMIN, nvinfer1::Dims4{ 1, 3, 160, 160 });
        profile->setDimensions(inName, nvinfer1::OptProfileSelector::kOPT, nvinfer1::Dims4{ 1, 3, 320, 320 });
        profile->setDimensions(inName, nvinfer1::OptProfileSelector::kMAX, nvinfer1::Dims4{ 1, 3, 640, 640 });
        if (config.verbose)
            std::cout << "[TensorRT] Dynamic profile 160/320/640" << std::endl;
    }

    cfg->addOptimizationProfile(profile);

    // ── 精度选择 ────────────────────────────────────────────────────────────
    // 网络 IO 保持 kHALF 不变(两种精度都是), 所以检测器的预处理与 IO 校验不用改:
    // 隐式量化会在网络内部插入 Quantize/Dequantize, IO 张量类型仍是 kHALF。
    const bool wantInt8 = (config.engine_precision == "int8");
    std::unique_ptr<Int8EntropyCalibrator> calibrator;

    if (wantInt8)
    {
        std::string why;
        if (!gpuSupportsInt8TensorCore(why))
        {
            std::cerr << "[TensorRT] ERROR: engine_precision=int8 但本机不支持: "
                      << why << "\n"
                      << "[TensorRT] 请把 engine_precision 改回 fp16。"
                         "(FP8 需要 Ada/Hopper, 本机更不可能)"
                      << std::endl;
            return nullptr;
        }
        std::cout << "[TensorRT] INT8 build: 判据 GPU = " << why << std::endl;

        // 校准表的缓存名带上输入边长, 换分辨率不会复用错的表。
        const std::string cachePath = "int8_calib_" + std::to_string(H > 0 ? H : 0) + ".cache";
        calibrator = std::make_unique<Int8EntropyCalibrator>(
            (H > 0 ? H : 320), config.int8_calib_dir, config.int8_calib_images, cachePath);

        if (!calibrator->usable())
        {
            std::cerr << "[TensorRT] ERROR: engine_precision=int8 但找不到校准图。\n"
                      << "[TensorRT]   期望目录: " << config.int8_calib_dir << "\n"
                      << "[TensorRT]   里面需要放 jpg/jpeg/png/bmp 截图 (建议 100~200 张真实游戏画面)。\n"
                      << "[TensorRT]   拒绝在缺校准图的情况下悄悄退回 FP16 —— 那样出来的\n"
                      << "[TensorRT]   引擎精度不是你要的那个, 而且不会有任何提示。\n"
                      << "[TensorRT]   现成的图可以来自 screenshots/auto (自动采集)。"
                      << std::endl;
            return nullptr;
        }
        std::cout << "[TensorRT] INT8 校准: " << calibrator->imageCount()
                  << " 张图, 目录 = " << config.int8_calib_dir
                  << ", 缓存 = " << cachePath << std::endl;

        cfg->setFlag(nvinfer1::BuilderFlag::kINT8);
        // INT8 + FP16 同时开: 可量化的层走 INT8, 其余(含 IO/Sigmoid/Resize 等)走 FP16。
        cfg->setFlag(nvinfer1::BuilderFlag::kFP16);
        cfg->clearFlag(nvinfer1::BuilderFlag::kTF32);
        cfg->setInt8Calibrator(calibrator.get());
    }
    else
    {
        std::cout << "[TensorRT] FP16 build: kFP16 (IO pinned to kHALF, kTF32 disabled)" << std::endl;
        cfg->setFlag(nvinfer1::BuilderFlag::kFP16);
        cfg->clearFlag(nvinfer1::BuilderFlag::kTF32);
    }

#if NV_TENSORRT_MAJOR > 8 || (NV_TENSORRT_MAJOR == 8 && NV_TENSORRT_MINOR >= 6)
    cfg->setBuilderOptimizationLevel(5);
    std::cout << "[TensorRT] Builder optimization level: 5" << std::endl;
#endif

    cudaStream_t stream;
    cudaStreamCreate(&stream);

    std::cout << "[TensorRT] Building engine (this may take several minutes)..." << std::endl;

    auto plan = builder->buildSerializedNetwork(*network, *cfg);
    if (!plan)
    {
        std::cerr << "[TensorRT] ERROR: Could not build the engine" << std::endl;
        return nullptr;
    }

    cudaStreamSynchronize(stream);
    cudaStreamDestroy(stream);

    return std::unique_ptr<nvinfer1::IHostMemory>(plan);
}

struct ScopedExportState
{
    ScopedExportState()
    {
        TrtExportResetState();
        gIsTrtExporting = true;
    }

    ~ScopedExportState()
    {
        std::lock_guard<std::mutex> lock(gProgressMutex);
        gProgressPhases.clear();
        gIsTrtExporting = false;
        gTrtExportCancelRequested = false;
        gTrtExportLastUpdateMs = TrtNowMs();
    }
};
}

std::unique_ptr<nvinfer1::IHostMemory> buildSerializedEngineFromOnnxMemory(const void* data, size_t size, nvinfer1::ILogger& logger)
{
    if (!data || size == 0)
    {
        std::cerr << "[TensorRT] ERROR: Empty ONNX memory buffer" << std::endl;
        return nullptr;
    }
    if (size > static_cast<size_t>((std::numeric_limits<int>::max)()))
    {
        std::cerr << "[TensorRT] ERROR: ONNX memory buffer is too large" << std::endl;
        return nullptr;
    }

    nvinfer1::IBuilder* builder = nvinfer1::createInferBuilder(logger);
    const auto explicitBatch = 1U << static_cast<uint32_t>(nvinfer1::NetworkDefinitionCreationFlag::kEXPLICIT_BATCH);
    nvinfer1::INetworkDefinition* network = builder ? builder->createNetworkV2(explicitBatch) : nullptr;
    nvinfer1::IBuilderConfig* cfg = builder ? builder->createBuilderConfig() : nullptr;
    nvonnxparser::IParser* parser = network ? nvonnxparser::createParser(*network, logger) : nullptr;

    if (!builder || !network || !cfg || !parser)
    {
        std::cerr << "[TensorRT] ERROR: Could not create TensorRT builder objects" << std::endl;
        delete parser;
        delete network;
        delete cfg;
        delete builder;
        return nullptr;
    }

    TrtProgressMonitor progressMonitor;
    cfg->setProgressMonitor(&progressMonitor);
    ScopedExportState exportState;

    if (!parser->parse(data, static_cast<size_t>(size)))
    {
        std::cerr << "[TensorRT] ERROR: Error parsing ONNX model from memory" << std::endl;
        delete parser;
        delete network;
        delete cfg;
        delete builder;
        return nullptr;
    }

    auto plan = buildSerializedEngine(network, builder, cfg);

    delete parser;
    delete network;
    delete cfg;
    delete builder;

    return plan;
}

nvinfer1::ICudaEngine* buildEngineFromOnnxMemory(const void* data, size_t size, nvinfer1::ILogger& logger)
{
    auto plan = buildSerializedEngineFromOnnxMemory(data, size, logger);
    if (!plan)
        return nullptr;

    nvinfer1::IRuntime* runtime = nvinfer1::createInferRuntime(logger);
    nvinfer1::ICudaEngine* engine = runtime ? runtime->deserializeCudaEngine(plan->data(), plan->size()) : nullptr;
    if (!engine)
    {
        std::cerr << "[TensorRT] ERROR: Could not create engine" << std::endl;
        delete runtime;
        return nullptr;
    }

    delete runtime;
    std::cout << "[TensorRT] The FP16 engine was built successfully." << std::endl;
    return engine;
}

nvinfer1::ICudaEngine* buildEngineFromOnnx(const std::string& onnxFile, nvinfer1::ILogger& logger)
{
    std::ifstream file(std::filesystem::u8path(onnxFile), std::ios::binary);
    if (!file.good())
    {
        std::cerr << "[TensorRT] ERROR: Error opening the ONNX file: " << onnxFile << std::endl;
        return nullptr;
    }
    file.seekg(0, std::ios::end);
    const size_t size = static_cast<size_t>(file.tellg());
    file.seekg(0, std::ios::beg);
    std::vector<uint8_t> onnxData(size);
    if (!onnxData.empty())
        file.read(reinterpret_cast<char*>(onnxData.data()), static_cast<std::streamsize>(onnxData.size()));
    if (!file && !onnxData.empty())
    {
        std::cerr << "[TensorRT] ERROR: Error reading the ONNX file: " << onnxFile << std::endl;
        return nullptr;
    }
    return buildEngineFromOnnxMemory(onnxData.data(), onnxData.size(), logger);
}
