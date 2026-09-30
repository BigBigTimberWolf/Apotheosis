#define WIN32_LEAN_AND_MEAN
#define _WINSOCKAPI_
#include <winsock2.h>
#include <Windows.h>
#include <intrin.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <opencv2/opencv.hpp>
#include <opencv2/dnn.hpp>
#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <limits>
#include <numeric>
#include <vector>
#include <queue>
#include <mutex>
#include <sstream>
#include <cstring>

#include "trt_detector.h"
#include "runtime/config_snapshot.h"
#include "nvinf.h"
#include "Apotheosis.h"
#include "other_tools.h"
#include "postProcess.h"
#include "model_inspector.h"
#include "model_crypto/model_crypto.h"
#include "cuda_preprocess.h"
#include "engine_cache_key.h"
#include "trt_monitor.h"
#include "capture.h"
#include "capture/auto_capture.h"
#include "runtime/active_hotkey.h"
#include "runtime/latency_probe.h"
#include "runtime/sched_boost.h"
#include "runtime/aim_loop.h"

int model_quant;
std::vector<float> outputData;

extern std::atomic<bool> detector_model_changed;
extern std::atomic<bool> session_stop_requested;
extern std::atomic<bool> detection_resolution_changed;

static bool error_logged = false;

namespace {
bool tryGetDimInt(int64_t value, int* out)
{
    if (value < std::numeric_limits<int>::min() || value > std::numeric_limits<int>::max())
        return false;
    *out = static_cast<int>(value);
    return true;
}

void publishTrtModelMetadata(const TrtDetector& detector)
{
    detector::ModelMetadata md;
    md.class_count = detector.numberOfClasses();
    md.class_names = detector.classNames();
    detector::pad_class_names(md, md.class_count);
    if (md.source == detector::ClassNamesSource::None && !md.class_names.empty())
        md.source = detector::ClassNamesSource::OnnxCustomMetadata;

    {
        std::lock_guard<std::recursive_mutex> lock(configMutex);
        config.sync_class_filters_from_model(md.class_count, md.class_names);
    }

    {
        std::lock_guard<std::mutex> lock(runtime::g_model_metadata_mutex);
        runtime::g_model_metadata = std::move(md);
    }
}

bool isAsciiPath(const std::string& path)
{
    return std::all_of(path.begin(), path.end(), [](unsigned char ch) {
        return ch < 0x80;
    });
}

std::string makeAsciiEngineStem(const std::filesystem::path& modelPath)
{
    const std::string stem = modelPath.stem().u8string();
    if (isAsciiPath(stem))
        return stem;

    uint64_t hash = 1469598103934665603ull;
    for (unsigned char ch : stem)
    {
        hash ^= static_cast<uint64_t>(ch);
        hash *= 1099511628211ull;
    }

    std::ostringstream oss;
    oss << "model_" << std::hex << hash;
    return oss.str();
}
std::string makeTensorRtParserPath(const std::filesystem::path& onnxPath,
                                   const std::filesystem::path& engineCacheDir,
                                   std::filesystem::path* temporaryPath)
{
    const std::string original = onnxPath.u8string();
    if (isAsciiPath(original))
        return original;

    std::error_code ec;
    const std::filesystem::path asciiDir = engineCacheDir / "_trt_parser_tmp";
    std::filesystem::create_directories(asciiDir, ec);
    if (ec)
    {
        std::cerr << "[Detector] Failed to create TensorRT parser temp dir: "
                  << ec.message() << std::endl;
        return original;
    }

    const std::filesystem::path asciiPath = asciiDir / "model.onnx";
    std::filesystem::copy_file(onnxPath, asciiPath,
                               std::filesystem::copy_options::overwrite_existing,
                               ec);
    if (ec)
    {
        std::cerr << "[Detector] Failed to copy ONNX to ASCII TensorRT parser path: "
                  << ec.message() << std::endl;
        return original;
    }

    if (temporaryPath)
        *temporaryPath = asciiPath;

    std::cout << "[Detector] Copied non-ASCII ONNX path for TensorRT parser: "
              << asciiPath.u8string() << std::endl;
    return asciiPath.u8string();
}
bool tryGetPositiveDimInt(int64_t value, int* out)
{
    if (value <= 0)
        return false;
    return tryGetDimInt(value, out);
}

bool engineHasHalfInput(const std::filesystem::path& enginePath)
{
    std::unique_ptr<nvinfer1::IRuntime> probeRuntime(nvinfer1::createInferRuntime(gLogger));
    if (!probeRuntime)
        return false;

    std::unique_ptr<nvinfer1::ICudaEngine> probeEngine(
        loadEngineFromFile(enginePath.u8string(), probeRuntime.get()));
    if (!probeEngine)
        return false;

    for (int i = 0; i < probeEngine->getNbIOTensors(); ++i)
    {
        const char* name = probeEngine->getIOTensorName(i);
        if (probeEngine->getTensorIOMode(name) == nvinfer1::TensorIOMode::kINPUT)
            return probeEngine->getTensorDataType(name) == nvinfer1::DataType::kHALF;
    }
    return false;
}

bool engineIsFullyHalf(const std::filesystem::path& enginePath)
{
    std::unique_ptr<nvinfer1::IRuntime> probeRuntime(nvinfer1::createInferRuntime(gLogger));
    if (!probeRuntime)
        return false;

    std::unique_ptr<nvinfer1::ICudaEngine> probeEngine(
        loadEngineFromFile(enginePath.u8string(), probeRuntime.get()));
    if (!probeEngine)
        return false;

    for (int i = 0; i < probeEngine->getNbIOTensors(); ++i)
    {
        const char* name = probeEngine->getIOTensorName(i);
        if (probeEngine->getTensorDataType(name) != nvinfer1::DataType::kHALF)
            return false;
    }
    return true;
}

bool engineBytesAreFullyHalf(const void* data, size_t size)
{
    if (!data || size == 0)
        return false;

    std::unique_ptr<nvinfer1::IRuntime> probeRuntime(nvinfer1::createInferRuntime(gLogger));
    if (!probeRuntime)
        return false;

    std::unique_ptr<nvinfer1::ICudaEngine> probeEngine(
        probeRuntime->deserializeCudaEngine(data, size));
    if (!probeEngine)
        return false;

    for (int i = 0; i < probeEngine->getNbIOTensors(); ++i)
    {
        const char* name = probeEngine->getIOTensorName(i);
        if (probeEngine->getTensorDataType(name) != nvinfer1::DataType::kHALF)
            return false;
    }
    return true;
}

}

TrtDetector::TrtDetector()
    : frameReady(false),
    shouldExit(false),
    useCudaGraph(false),
    cudaGraphCaptured(false),
    inputBufferDevice(nullptr),
    img_scale(1.0f),
    numClasses(0)
{
    stream = nullptr;
    {
        int priLow = 0, priHigh = 0;
        if (cudaDeviceGetStreamPriorityRange(&priLow, &priHigh) == cudaSuccess &&
            priHigh != priLow &&
            cudaStreamCreateWithPriority(&stream, cudaStreamNonBlocking, priHigh) == cudaSuccess)
        {
        }
        else
        {
            cudaStreamCreateWithFlags(&stream, cudaStreamNonBlocking);
        }
    }
}

TrtDetector::~TrtDetector()
{
    releaseModelResources();
    for (int s = 0; s < 2; ++s)
    {
        if (preprocessStartEvent[s]) cudaEventDestroy(preprocessStartEvent[s]);
        if (inferenceStartEvent[s]) cudaEventDestroy(inferenceStartEvent[s]);
        if (inferenceCompleteEvent[s]) cudaEventDestroy(inferenceCompleteEvent[s]);
        if (copyCompleteEvent[s]) cudaEventDestroy(copyCompleteEvent[s]);
        if (slotDoneEvent[s]) { cudaEventDestroy(slotDoneEvent[s]); slotDoneEvent[s] = nullptr; }
    }
    if (stream) cudaStreamDestroy(stream);
}

void TrtDetector::releaseModelResources()
{
    if (stream) cudaStreamSynchronize(stream);
    destroyCudaGraph();
    // Deserialized contexts and engines must be destroyed before their runtime.
    context.reset();
    engine.reset();
    runtime.reset();
    freePinnedOutputs();
    freeTransposedBuffers();
    for (auto& binding : inputBindings) if (binding.second) cudaFree(binding.second);
    for (auto& binding : outputBindings) if (binding.second) cudaFree(binding.second);
    inputBindings.clear();
    outputBindings.clear();
    if (inputBufferDevice) cudaFree(inputBufferDevice);
    inputBufferDevice = nullptr;
    gpuFrameBuffer.release();
    for (auto& frame : graphInputBuffers) frame.release();
    inputNames.clear();
    outputNames.clear();
    inputSizes.clear();
    outputSizes.clear();
    outputShapes.clear();
    outputTypes.clear();
    inputName.clear();
    numClasses = 0;
    model_input_width_ = model_input_height_ = 0;
    raw_candidates_.clear();
    raw_selected_.clear();
    detection_scratch_.clear();
}

void TrtDetector::shutdown()
{
    releaseModelResources();
}

void TrtDetector::freePinnedOutputs()
{
    for (auto& kv : pinnedOutputBuffers)
    {
        if (kv.second)
            cudaFreeHost(kv.second);
    }
    pinnedOutputBuffers.clear();
    for (auto& kv : pinnedOutputBuffersB)
    {
        if (kv.second)
            cudaFreeHost(kv.second);
    }
    pinnedOutputBuffersB.clear();
}

void TrtDetector::freeTransposedBuffers()
{
}

void TrtDetector::waitForEvent(cudaEvent_t ev)
{
    if (!ev)
        return;

    const Config& cfg = *runtime_config::read();
    const bool spin = cfg.use_spin_wait_sync;
    const int  timeoutMs = cfg.spin_wait_timeout_ms;

    // ★ 实测结论 (build\cuda\Release\spin_vs_block_bench.exe, 本机 i5-4590 4 核):
    //   自旋等待在 240fps 下约吃掉 1.2 个核, 换来的是每次等待少 ~0.1 ms 墙钟。
    //   本机只有 4 个核, 而解码(CPU 熵解码)就要用掉约 2 个 —— 用 1.2 个核去换
    //   0.1 ms 不划算, 因此 use_spin_wait_sync 的默认值已改为 false。
    //   要用回自旋 (最低延迟、最高 CPU 占用): 把配置里 use_spin_wait_sync 设为 true。
    //   前提: copyCompleteEvent 必须带 cudaEventBlockingSync, 否则下面的
    //   cudaEventSynchronize 分支同样会自旋 (实测 102% CPU/墙钟), 白关。
    const auto t0 = std::chrono::steady_clock::now();

    if (spin)
    {
        const auto deadline = t0 + std::chrono::milliseconds(timeoutMs);
        while (true)
        {
            const cudaError_t q = cudaEventQuery(ev);
            if (q == cudaSuccess)
            {
                lastSyncUsedSpin = true;
                lastSyncSpinMs = std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - t0).count();
                return;
            }
            if (q != cudaErrorNotReady)
            {
                break;
            }
            if (std::chrono::steady_clock::now() >= deadline)
            {
                ++syncFallbackCount;
                break;
            }
            _mm_pause();
        }
    }

    lastSyncUsedSpin = false;
    cudaEventSynchronize(ev);
    lastSyncSpinMs = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - t0).count();
}

void TrtDetector::allocatePinnedOutputs()
{    freePinnedOutputs();

    for (const auto& name : outputNames)
    {
        const size_t bytes = outputSizes[name];
        if (bytes == 0) continue;

        for (int slot = 0; slot < 2; ++slot)
        {
            void* hostPtr = nullptr;
            cudaError_t err = cudaHostAlloc(&hostPtr, bytes, cudaHostAllocDefault);
            if (err != cudaSuccess)
            {
                std::cerr << "[Detector] cudaHostAlloc failed for output " << name
                    << " slot " << slot << " (" << bytes << " bytes): "
                    << cudaGetErrorString(err) << std::endl;
                continue;
            }
            pinnedSlot(slot)[name] = hostPtr;
        }

        if (runtime_config::read()->verbose)
        {
            std::cout << "[Detector] Allocated pinned host buffer(s) for output "
                << name << ": " << bytes << " bytes each" << std::endl;
        }
    }
}

void TrtDetector::destroyCudaGraph()
{
    for (int s = 0; s < 2; ++s)
    {
        if (cudaGraphExecs[s])
        {
            cudaGraphExecDestroy(cudaGraphExecs[s]);
            cudaGraphExecs[s] = nullptr;
        }
        if (cudaGraphs[s])
        {
            cudaGraphDestroy(cudaGraphs[s]);
            cudaGraphs[s] = nullptr;
        }
    }
    cudaGraphCaptured = false;
    graphInputRows = 0;
    graphInputCols = 0;
    graphInputChannels = 0;
    graphInputStep = 0;
}

TrtDetector::GraphStagingResult TrtDetector::ensureGraphStaging(int rows, int cols, int channels)
{
    const bool shapeChanged =
        (rows != graphInputRows) ||
        (cols != graphInputCols) ||
        (channels != graphInputChannels);

    bool needsCapture = !cudaGraphCaptured || shapeChanged;

    for (int s = 0; s < 2; ++s)
    {
        auto& staging = graphInputBuffers[s];
        if (staging.empty() || staging.rows() != rows
            || staging.cols() != cols || staging.channels() != channels)
        {
            if (!staging.create(rows, cols, channels))
            {
                std::cerr << "[Detector] Failed to allocate graph staging buffer ("
                          << rows << "x" << cols << "x" << channels << ")" << std::endl;
                destroyCudaGraph();
                return GraphStagingResult::Failed;
            }
            needsCapture = true;
        }
    }

    if (shapeChanged && cudaGraphCaptured)
    {
        destroyCudaGraph();
    }

    graphInputRows = rows;
    graphInputCols = cols;
    graphInputChannels = channels;
    graphInputStep = graphInputBuffers[0].step();

    return needsCapture ? GraphStagingResult::Capture : GraphStagingResult::Reuse;
}

bool TrtDetector::captureCudaGraph(int slot)
{
    if (!useCudaGraph) return false;
    if (slot < 0 || slot >= 2) return false;
    if (graphInputBuffers[slot].empty()) return false;

    if (cudaGraphExecs[slot])
    {
        cudaGraphExecDestroy(cudaGraphExecs[slot]);
        cudaGraphExecs[slot] = nullptr;
    }
    if (cudaGraphs[slot])
    {
        cudaGraphDestroy(cudaGraphs[slot]);
        cudaGraphs[slot] = nullptr;
    }

    {
        void* warmInput = inputBindings[inputName];
        if (warmInput)
        {
            nvinfer1::Dims wd = context->getTensorShape(inputName.c_str());
            int warmH = 0;
            int warmW = 0;
            if (wd.nbDims >= 4
                && tryGetPositiveDimInt(wd.d[2], &warmH)
                && tryGetPositiveDimInt(wd.d[3], &warmW))
            {
                launch_resize_bgr_u8_to_chw_rgb_f16(
                    graphInputBuffers[slot].view(),
                    reinterpret_cast<__half*>(warmInput),
                    warmW,
                    stream
                );
            }
        }
        context->enqueueV3(stream);
    }

    cudaStreamSynchronize(stream);

    cudaError_t st = cudaStreamBeginCapture(stream, cudaStreamCaptureModeThreadLocal);
    if (st != cudaSuccess) {
        std::cerr << "[Detector] BeginCapture(slot=" << slot << ") failed: "
            << cudaGetErrorString(st) << std::endl;
        return false;
    }

    void* inputBuffer = inputBindings[inputName];
    if (inputBuffer)
    {
        nvinfer1::Dims dims = context->getTensorShape(inputName.c_str());
        int sideH = 0;
        int sideW = 0;
        if (dims.nbDims >= 4
            && tryGetPositiveDimInt(dims.d[2], &sideH)
            && tryGetPositiveDimInt(dims.d[3], &sideW))
        {
            launch_resize_bgr_u8_to_chw_rgb_f16(
                graphInputBuffers[slot].view(),
                reinterpret_cast<__half*>(inputBuffer),
                sideW,
                stream
            );
        }
    }

    context->enqueueV3(stream);

    auto& pinned = pinnedSlot(slot);
    for (const auto& name : outputNames)
    {
        const auto itPinned = pinned.find(name);
        if (itPinned == pinned.end() || !itPinned->second) continue;

        cudaMemcpyAsync(itPinned->second,
            outputBindings[name],
            outputSizes[name],
            cudaMemcpyDeviceToHost,
            stream);
    }

    st = cudaStreamEndCapture(stream, &cudaGraphs[slot]);
    if (st != cudaSuccess) {
        std::cerr << "[Detector] EndCapture(slot=" << slot << ") failed: "
            << cudaGetErrorString(st) << std::endl;
        return false;
    }

    st = cudaGraphInstantiate(&cudaGraphExecs[slot], cudaGraphs[slot], 0);
    if (st != cudaSuccess) {
        std::cerr << "[Detector] GraphInstantiate(slot=" << slot << ") failed: "
            << cudaGetErrorString(st) << std::endl;
        cudaGraphDestroy(cudaGraphs[slot]);
        cudaGraphs[slot] = nullptr;
        return false;
    }

    cudaGraphCaptured = (cudaGraphExecs[0] != nullptr);
    return true;
}

inline void TrtDetector::launchCudaGraph(int slot)
{
    if (slot < 0 || slot >= 2) return;
    if (!cudaGraphExecs[slot]) return;
    auto err = cudaGraphLaunch(cudaGraphExecs[slot], stream);
    if (err != cudaSuccess)
    {
        std::cerr << "[Detector] GraphLaunch(slot=" << slot << ") failed: "
            << cudaGetErrorString(err) << std::endl;
    }
}

void TrtDetector::getInputNames()
{
    inputNames.clear();
    inputSizes.clear();

    for (int i = 0; i < engine->getNbIOTensors(); ++i)
    {
        const char* name = engine->getIOTensorName(i);
        if (engine->getTensorIOMode(name) == nvinfer1::TensorIOMode::kINPUT)
        {
            inputNames.emplace_back(name);
            if (runtime_config::read()->verbose)
            {
                std::cout << "[Detector] Detected input: " << name << std::endl;
            }
        }
    }
}

void TrtDetector::getOutputNames()
{
    outputNames.clear();
    outputSizes.clear();
    outputTypes.clear();
    outputShapes.clear();

    for (int i = 0; i < engine->getNbIOTensors(); ++i)
    {
        const char* name = engine->getIOTensorName(i);
        if (engine->getTensorIOMode(name) == nvinfer1::TensorIOMode::kOUTPUT)
        {
            outputNames.emplace_back(name);
            outputTypes[name] = engine->getTensorDataType(name);

            if (runtime_config::read()->verbose)
            {
                std::cout << "[Detector] Detected output: " << name << std::endl;
            }
        }
    }
}

void TrtDetector::getBindings()
{
    for (auto& binding : inputBindings)
    {
        if (binding.second) cudaFree(binding.second);
    }
    inputBindings.clear();

    for (auto& binding : outputBindings)
    {
        if (binding.second) cudaFree(binding.second);
    }
    outputBindings.clear();

    for (const auto& name : inputNames)
    {
        size_t size = inputSizes[name];
        if (size > 0)
        {
            void* ptr = nullptr;

            cudaError_t err = cudaMalloc(&ptr, size);
            if (err == cudaSuccess)
            {
                inputBindings[name] = ptr;
                if (runtime_config::read()->verbose)
                {
                    std::cout << "[Detector] Allocated " << size << " bytes for input " << name << std::endl;
                }
            }
            else
            {
                std::cerr << "[Detector] Failed to allocate input memory: " << cudaGetErrorString(err) << std::endl;
            }
        }
    }

    for (const auto& name : outputNames)
    {
        size_t size = outputSizes[name];
        if (size > 0) {
            void* ptr = nullptr;
            cudaError_t err = cudaMalloc(&ptr, size);
            if (err == cudaSuccess)
            {
                outputBindings[name] = ptr;
                if (runtime_config::read()->verbose)
                {
                    std::cout << "[Detector] Allocated " << size << " bytes for output " << name << std::endl;
                }
            }
            else
            {
                std::cerr << "[Detector] Failed to allocate output memory: " << cudaGetErrorString(err) << std::endl;
            }
        }
    }
}

bool TrtDetector::initialize(const std::string& model_path)
{
    releaseModelResources();
    last_error_.clear();
    shouldExit = false;
    {
        std::lock_guard<std::mutex> lock(inferenceMutex);
        frameReady = false;
        pendingFrameType = PendingFrameType::None;
        currentFrame.release();
        currentFrameGpu.release();
    }

    class_names_.clear();
    {
        std::string resolved_path = model_path;
        std::filesystem::path maybe(std::filesystem::u8path(model_path));
        if (!maybe.is_absolute())
        {
            std::error_code ec;
            auto abs = std::filesystem::absolute(maybe, ec);
            if (!ec) resolved_path = abs.u8string();
        }

        std::string ext = std::filesystem::u8path(model_path).extension().u8string();
        std::transform(ext.begin(), ext.end(), ext.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (ext == ".onnx" || ext == ".oliver")
        {
            detector::ModelMetadata md = detector::inspect_onnx_model(model_path, runtime_config::read()->verbose);
            if (!md.output_shape.empty()
                && detector::classify_model_output(md.output_shape) == detector::ModelOutputKind::Unknown)
            {
                std::ostringstream shape;
                shape << '[';
                for (size_t i = 0; i < md.output_shape.size(); ++i)
                    shape << (i ? ", " : "") << md.output_shape[i];
                shape << ']';
                last_error_ = "模型输出 " + shape.str()
                    + " 不属于当前支持的目标检测输出格式 [1,N,6] 或 [1,4+C,N]。";
                std::cerr << "[Detector] " << last_error_ << std::endl;
                return false;
            }
            if (!md.class_names.empty())
                class_names_ = std::move(md.class_names);
        }

        if (class_names_.empty())
        {
            auto blob = detector::read_ultralytics_engine_header(resolved_path);
            if (!blob.empty())
                class_names_ = detector::parse_json_names(blob);
        }

        if (class_names_.empty())
        {
            detector::ClassNamesSource src = detector::ClassNamesSource::None;
            auto sidecar = detector::read_sidecar_class_names(resolved_path, &src);
            if (!sidecar.empty())
                class_names_ = std::move(sidecar);
        }
    }

    runtime.reset(nvinfer1::createInferRuntime(gLogger));
    const uint64_t buildGeneration = TrtBuildRead().generation;
    loadEngine(model_path);
    if (!engine)
    {
        const TrtBuildSnapshot build = TrtBuildRead();
        if (build.generation > buildGeneration && build.stage == TrtBuildStage::Failed)
            last_error_ = build.detail;
        if (last_error_.empty()) last_error_ = "TensorRT 引擎加载失败，请查看日志";
        std::cerr << "[Detector] Engine loading failed" << std::endl;
        return false;
    }

    context.reset(engine->createExecutionContext());
    if (!context)
    {
        std::cerr << "[Detector] Context creation failed" << std::endl;
        return false;
    }

    getInputNames();
    getOutputNames();
    if (inputNames.empty())
    {
        std::cerr << "[Detector] No input tensors found" << std::endl;
        return false;
    }
    inputName = inputNames[0];

    nvinfer1::Dims inputDims = context->getTensorShape(inputName.c_str());
    bool isStatic = true;
    for (int i = 0; i < inputDims.nbDims; ++i)
        if (inputDims.d[i] <= 0) isStatic = false;

    if (isStatic != runtime_config::read()->fixed_input_size)
    {
        {
            std::lock_guard<std::recursive_mutex> lock(configMutex);
            config.fixed_input_size = isStatic;
        }
        runtime_config::publish();
        detector_model_changed.store(true);
        std::cout << "[Detector] Automatically set fixed_input_size = " << (isStatic ? "true" : "false") << std::endl;
    }

    const int target = runtime_config::read()->detection_resolution;
    if (!isStatic)
    {
        nvinfer1::Dims4 newShape{ 1, 3, target, target };
        context->setInputShape(inputName.c_str(), newShape);
        if (!context->allInputDimensionsSpecified())
        {
            std::cerr << "[Detector] Failed to set input dimensions" << std::endl;
            return false;
        }
        inputDims = context->getTensorShape(inputName.c_str());
    }

    inputSizes.clear();
    outputSizes.clear();
    outputShapes.clear();
    outputTypes.clear();
    freeTransposedBuffers();

    for (const auto& inName : inputNames)
    {
        nvinfer1::Dims d = context->getTensorShape(inName.c_str());
        nvinfer1::DataType dt = engine->getTensorDataType(inName.c_str());
        inputSizes[inName] = getSizeByDim(d) * getElementSize(dt);
    }
    for (const auto& outName : outputNames)
    {
        nvinfer1::Dims d = context->getTensorShape(outName.c_str());
        nvinfer1::DataType dt = engine->getTensorDataType(outName.c_str());
        outputSizes[outName] = getSizeByDim(d) * getElementSize(dt);
        std::vector<int64_t> shape(d.nbDims);
        for (int j = 0; j < d.nbDims; ++j) shape[j] = d.d[j];
        outputShapes[outName] = std::move(shape);
        outputTypes[outName] = dt;
    }

    getBindings();

    if (!outputNames.empty())
    {
        const std::string& mainOut = outputNames[0];
        nvinfer1::Dims outDims = context->getTensorShape(mainOut.c_str());
        int64_t dim1 = (outDims.nbDims >= 2) ? outDims.d[1] : 0;
        int64_t dim2 = (outDims.nbDims >= 3) ? outDims.d[2] : 0;

        int64_t channels64 = 0;
        bool cnLayout = true;
        if (dim1 > 4 && dim2 > 0 && dim1 <= dim2)
        {
            channels64 = dim1;
            cnLayout = true;
        }
        else if (dim2 > 4 && dim1 > 0 && dim2 < dim1)
        {
            channels64 = dim2;
            cnLayout = false;
        }
        else if (dim1 > 4)
        {
            channels64 = dim1;
            cnLayout = true;
        }
        else if (dim2 > 4)
        {
            channels64 = dim2;
            cnLayout = false;
        }

        const int64_t classes64 = (channels64 > 4) ? (channels64 - 4) : 1;
        int classes = 0;
        if (!tryGetDimInt(classes64, &classes) || classes <= 0)
        {
            if (!class_names_.empty())
            {
                classes = static_cast<int>(class_names_.size());
                std::cerr << "[Detector] Invalid output dimensions for classes; using "
                          << classes << " class names from model metadata." << std::endl;
            }
            else
            {
                std::cerr << "[Detector] Invalid output dimensions for classes" << std::endl;
                return false;
            }
        }

        if (!class_names_.empty() && static_cast<int>(class_names_.size()) > classes)
        {
            classes = static_cast<int>(class_names_.size());
        }
        numClasses = classes;

        if (runtime_config::read()->verbose)
        {
            std::cout << "[Detector] Output '" << mainOut << "' shape=["
                      << outDims.d[0] << "," << dim1 << "," << dim2 << "]"
                      << " layout=" << (cnLayout ? "[1,C,N]" : "[1,N,C]")
                      << " numClasses=" << numClasses << std::endl;
        }
    }

    int c = 0;
    int h = 0;
    int w = 0;
    if (!tryGetPositiveDimInt(inputDims.d[1], &c)
        || !tryGetPositiveDimInt(inputDims.d[2], &h)
        || !tryGetPositiveDimInt(inputDims.d[3], &w))
    {
        std::cerr << "[Detector] Invalid input dimensions" << std::endl;
        return false;
    }

    if (h != w)
    {
        std::cerr << "[Detector] Non-square model input not supported (got "
                  << h << "x" << w << "). Use a square detection model." << std::endl;
        return false;
    }
    model_input_width_ = w;
    model_input_height_ = h;

    {
        nvinfer1::DataType inDt = engine->getTensorDataType(inputName.c_str());
        if (inDt != nvinfer1::DataType::kHALF)
        {
            std::cerr << "[Detector] FP16-only build: engine input dtype must be kHALF. "
                      << "Delete the cached .engine and rebuild with FP16 enabled."
                      << std::endl;
            return false;
        }
        for (const auto& outName : outputNames)
        {
            const nvinfer1::DataType outDt = engine->getTensorDataType(outName.c_str());
            if (outDt != nvinfer1::DataType::kHALF)
            {
                std::cerr << "[Detector] FP16-only build: engine output '" << outName
                          << "' dtype must be kHALF (got "
                          << static_cast<int>(outDt) << "). "
                          << "Delete the cached .engine and rebuild with FP16 enabled."
                          << std::endl;
                return false;
            }
        }
    }

    for (const auto& outName : outputNames)
    {
        const auto& shape = outputShapes[outName];
        const auto kind = detector::classify_model_output(shape);
        if (kind == detector::ModelOutputKind::Unknown
            || shape.size() != 3 || shape[0] != 1 || shape[1] <= 0 || shape[2] <= 0)
        {
            std::cerr << "[Detector] 不支持的模型输出形状: " << outName << " = [";
            for (size_t i = 0; i < shape.size(); ++i)
                std::cerr << (i ? ", " : "") << shape[i];
            std::cerr << "]\n"
                      << "[Detector] 支持 end2end [1,N,6] 和原始 YOLO [1,4+C,N] / [1,N,4+C]。"
                      << std::endl;
            last_error_ = "不支持的模型输出形状；支持 [1,N,6] 或原始 YOLO [1,4+C,N]";
            return false;
        }
        if (kind == detector::ModelOutputKind::RawChannelsFirst
            || kind == detector::ModelOutputKind::RawRowsFirst)
        {
            const auto count = static_cast<size_t>(
                kind == detector::ModelOutputKind::RawChannelsFirst ? shape[2] : shape[1]);
            raw_candidates_.reserve(count);
            raw_selected_.reserve(kFixedMaxDetections);
        }
    }

    std::cout << "[Detector] Pipeline: latest-frame (publish after GPU completion)"
              << " cuda_graph=" << (runtime_config::read()->use_cuda_graph ? "on" : "off")
              << std::endl;

    for (int s = 0; s < 2; ++s)
    {
        if (slotDoneEvent[s]) { cudaEventDestroy(slotDoneEvent[s]); slotDoneEvent[s] = nullptr; }
    }
    for (int s = 0; s < 2; ++s)
        cudaEventCreateWithFlags(&slotDoneEvent[s], cudaEventDisableTiming);

    allocatePinnedOutputs();

    img_scale = static_cast<float>(runtime_config::read()->detection_resolution) / w;

    for (const auto& n : inputNames)
        context->setTensorAddress(n.c_str(), inputBindings[n]);
    for (const auto& n : outputNames)
        context->setTensorAddress(n.c_str(), outputBindings[n]);

    for (int s = 0; s < 2; ++s)
    {
        if (preprocessStartEvent[s]) cudaEventDestroy(preprocessStartEvent[s]);
        if (inferenceStartEvent[s]) cudaEventDestroy(inferenceStartEvent[s]);
        if (inferenceCompleteEvent[s]) cudaEventDestroy(inferenceCompleteEvent[s]);
        if (copyCompleteEvent[s]) cudaEventDestroy(copyCompleteEvent[s]);
        preprocessStartEvent[s] = nullptr;
        inferenceStartEvent[s] = nullptr;
        inferenceCompleteEvent[s] = nullptr;
        copyCompleteEvent[s] = nullptr;
        cudaEventCreate(&preprocessStartEvent[s]);
        cudaEventCreate(&inferenceStartEvent[s]);
        cudaEventCreate(&inferenceCompleteEvent[s]);
        // ★ copyCompleteEvent 是每帧热路径上唯一真正被等待的事件
        //   (waitForEvent(copyCompleteEvent[slot]), 在本帧发布前调用)。
        //   必须加 cudaEventBlockingSync, 否则"关闭自旋"根本不起作用 ——
        //   实测 (build\cuda\Release\spin_vs_block_bench.exe, 5ms 等待, 本机 i5-4590):
        //     自旋 cudaEventQuery + _mm_pause : 5.10 ms CPU / 5.20 ms 墙钟 =  98%
        //     裸 cudaEventSynchronize         : 5.31 ms CPU / 5.19 ms 墙钟 = 102%  <- 也在自旋!
        //     cudaEventSynchronize+BlockingSync: 0.10 ms CPU / 5.32 ms 墙钟 =   2%
        //   换算到 240fps: 自旋要吃掉约 1.2 个核, 阻塞只占 0.03 个核。
        //   本机共 4 核, 解码已用约 2 核 —— 这 1.2 个核直接决定采集能不能跑满 240。
        //   代价: 墙钟多约 0.1 ms/次 (唤醒延迟), 相对 ~4.5ms 的端到端链路可忽略。
        //   注意保留 timing: 下面 copyMs 那行要用 cudaEventElapsedTime。
        cudaEventCreateWithFlags(&copyCompleteEvent[s], cudaEventBlockingSync);
    }

    useCudaGraph = runtime_config::read()->use_cuda_graph;

    if (runtime_config::read()->verbose)
    {
        std::cout << "[Detector] Initialized. ModelStatic=" << std::boolalpha << isStatic
            << ", NetInput=" << h << "x" << w << " (scale=" << img_scale << ")" << std::endl;
    }

    return true;
}

void TrtDetector::requestExit()
{
    {
        std::lock_guard<std::mutex> lock(inferenceMutex);
        shouldExit = true;
        frameReady = false;
        pendingFrameType = PendingFrameType::None;
        currentFrame.release();
        currentFrameGpu.release();
    }
    inferenceCV.notify_all();
}

size_t TrtDetector::getSizeByDim(const nvinfer1::Dims& dims)
{
    size_t size = 1;
    for (int i = 0; i < dims.nbDims; ++i)
    {
        if (dims.d[i] < 0) return 0;
        size *= dims.d[i];
    }
    return size;
}

size_t TrtDetector::getElementSize(nvinfer1::DataType dtype)
{
    switch (dtype)
    {
    case nvinfer1::DataType::kFLOAT: return 4;
    case nvinfer1::DataType::kHALF: return 2;
    case nvinfer1::DataType::kINT32: return 4;
    case nvinfer1::DataType::kINT8: return 1;
    default: return 0;
    }
}

void TrtDetector::loadEngine(const std::string& modelFile)
{
    namespace fs = std::filesystem;

    const fs::path modelPath(fs::u8path(modelFile));
    const std::string extension = modelPath.extension().u8string();

    const fs::path engineCacheDir = fs::path("models") / "engines";
    std::error_code ec;
    fs::create_directories(engineCacheDir, ec);
    if (ec)
    {
        std::cerr << "[Detector] Failed to create engine cache dir '"
                  << engineCacheDir.u8string() << "': " << ec.message() << std::endl;
    }

    fs::path engineFilePath;

    if (extension == ".engine")
    {
        engineFilePath = modelPath;
    }
    else if (extension == ".oliver")
    {
        if (!fs::exists(modelPath, ec) || !fs::is_regular_file(modelPath, ec))
        {
            std::cerr << u8"[Detector] oliver 模型文件不存在: " << modelPath.u8string() << std::endl;
            return;
        }

        const std::string fingerprint = engineCacheFingerprint(modelPath, *runtime_config::read());
        if (fingerprint.empty())
        {
            std::cerr << "[Detector] Failed to fingerprint encrypted model or build settings" << std::endl;
            return;
        }
        const std::string cacheStem = makeAsciiEngineStem(modelPath) + "_" + fingerprint;
        const fs::path encryptedEngineCache = engineCacheDir / (cacheStem + ".engine.olivercache");

        if (fileExists(encryptedEngineCache.u8string()))
        {
            std::cout << "[Detector] Loading encrypted TensorRT engine cache: "
                      << encryptedEngineCache.u8string() << std::endl;

            oliver::Payload cachePayload;
            std::string error;
            bool decrypted = oliver::decrypt_file(encryptedEngineCache.u8string(), cachePayload, error)
                && cachePayload.type == oliver::PayloadType::TensorRtEngine;
            bool acceptCache = false;
            if (decrypted)
            {
                if (engineBytesAreFullyHalf(cachePayload.bytes.data(), cachePayload.bytes.size()))
                {
                    engine.reset(loadEngineFromMemory(cachePayload.bytes.data(), cachePayload.bytes.size(), runtime.get()));
                    if (engine)
                    {
                        acceptCache = true;
                        return;
                    }
                }
                else
                {
                    std::cerr << "[Detector] Encrypted engine cache is not fully FP16; rebuilding from oliver." << std::endl;
                }
            }
            else
            {
                std::cerr << "[Detector] Encrypted engine cache decrypt failed: " << error << std::endl;
            }

            (void)acceptCache;
            fs::remove(encryptedEngineCache, ec);
            if (ec)
            {
                std::cerr << "[Detector] Failed to delete invalid encrypted engine cache: "
                          << ec.message() << std::endl;
            }
        }

        oliver::Payload modelPayload;
        std::string error;
        std::string sourceModelId;
        oliver::read_model_id_from_file(modelPath.u8string(), sourceModelId, error);
        error.clear();
        if (!oliver::decrypt_file(modelPath.u8string(), modelPayload, error))
        {
            std::cerr << u8"[Detector] oliver 模型解密失败: " << error << std::endl;
            return;
        }
        if (modelPayload.type != oliver::PayloadType::Onnx)
        {
            std::cerr << u8"[Detector] oliver 文件不是 ONNX 模型，无法从 TRT 构建。" << std::endl;
            return;
        }

        std::cout << "[Detector] Building engine from encrypted ONNX model -> "
                  << encryptedEngineCache.u8string() << std::endl;
        TrtBuildScope buildProgress(modelPath.filename().u8string());
        auto serializedEngine = buildSerializedEngineFromOnnxMemory(modelPayload.bytes.data(), modelPayload.bytes.size(), gLogger);
        if (!serializedEngine)
            return;

        TrtBuildSetStage(TrtBuildStage::Loading);
        engine.reset(loadEngineFromMemory(serializedEngine->data(), serializedEngine->size(), runtime.get()));
        if (!engine)
            return;

        TrtBuildSetStage(TrtBuildStage::Saving);
        std::vector<uint8_t> engineBytes(static_cast<size_t>(serializedEngine->size()));
        std::memcpy(engineBytes.data(), serializedEngine->data(), engineBytes.size());
        std::vector<uint8_t> encrypted;
        if (sourceModelId.empty())
            sourceModelId = modelPayload.model_id;
        const bool cacheSaved =
            oliver::encrypt_bytes(engineBytes, oliver::PayloadType::TensorRtEngine, sourceModelId, encrypted, error) &&
            oliver::write_file_bytes(encryptedEngineCache.u8string(), encrypted, error);
        if (cacheSaved)
        {
            std::cout << "[Detector] Encrypted engine cache saved to: "
                      << encryptedEngineCache.u8string() << std::endl;
        }
        else
        {
            std::cerr << "[Detector] Failed to save encrypted engine cache: " << error << std::endl;
        }
        buildProgress.succeed(cacheSaved ? "" : "引擎可运行，但缓存未保存");
        return;
    }
    else if (extension == ".onnx")
    {
        if (!fs::exists(modelPath, ec) || !fs::is_regular_file(modelPath, ec))
        {
            std::cerr << "[Detector] ONNX model file not found: " << modelPath.u8string() << std::endl;
            return;
        }

        const std::string fingerprint = engineCacheFingerprint(modelPath, *runtime_config::read());
        if (fingerprint.empty())
        {
            std::cerr << "[Detector] Failed to fingerprint ONNX model or build settings" << std::endl;
            return;
        }
        engineFilePath = engineCacheDir / (makeAsciiEngineStem(modelPath) + "_" + fingerprint + ".engine");

        if (fileExists(engineFilePath.u8string()) && !engineIsFullyHalf(engineFilePath))
        {
            std::cerr << "[Detector] Cached engine has non-FP16 IO tensor(s); deleting and rebuilding: "
                      << engineFilePath.u8string() << std::endl;
            fs::remove(engineFilePath, ec);
            if (ec)
            {
                std::cerr << "[Detector] Failed to delete incompatible cached engine: "
                          << ec.message() << std::endl;
            }
        }

        if (fileExists(engineFilePath.u8string()))
        {
            engine.reset(loadEngineFromFile(engineFilePath.u8string(), runtime.get()));
            if (engine) return;
            std::cerr << "[Detector] Cached engine could not be loaded; rebuilding from ONNX." << std::endl;
            ec.clear();
            fs::remove(engineFilePath, ec);
            if (ec)
            {
                std::cerr << "[Detector] Could not remove invalid engine cache: "
                          << ec.message() << std::endl;
            }
        }

        if (!engine)
        {
            std::cout << "[Detector] Building engine from ONNX model -> "
                      << engineFilePath.u8string() << std::endl;
            TrtBuildScope buildProgress(modelPath.filename().u8string());

            std::filesystem::path temporaryParserPath;
            const std::string parserModelFile = makeTensorRtParserPath(modelPath, engineCacheDir, &temporaryParserPath);
            auto serializedEngine = buildSerializedEngineFromOnnxFile(parserModelFile, gLogger);
            bool cacheSaved = false;
            if (serializedEngine)
            {
                TrtBuildSetStage(TrtBuildStage::Loading);
                engine.reset(loadEngineFromMemory(
                    serializedEngine->data(), serializedEngine->size(), runtime.get()));
                if (engine)
                {
                    TrtBuildSetStage(TrtBuildStage::Saving);
                    fs::path temporaryEnginePath = engineFilePath;
                    temporaryEnginePath += ".tmp";
                    std::ofstream engineFile(temporaryEnginePath, std::ios::binary | std::ios::trunc);
                    if (engineFile)
                    {
                        engineFile.write(
                            reinterpret_cast<const char*>(serializedEngine->data()),
                            serializedEngine->size());
                        engineFile.close();
                        if (engineFile)
                        {
                            ec.clear();
                            fs::rename(temporaryEnginePath, engineFilePath, ec);
                            if (!ec)
                            {
                                cacheSaved = true;
                                std::cout << "[Detector] Engine saved to: "
                                          << engineFilePath.u8string() << std::endl;
                            }
                        }
                    }
                    if (!engineFile || ec)
                    {
                        std::cerr << "[Detector] Could not save engine cache: "
                                  << (ec ? ec.message() : temporaryEnginePath.u8string()) << std::endl;
                        ec.clear();
                        fs::remove(temporaryEnginePath, ec);
                    }
                }
            }

            if (!temporaryParserPath.empty())
            {
                fs::remove(temporaryParserPath, ec);
            }
            if (engine) buildProgress.succeed(cacheSaved ? "" : "引擎可运行，但缓存未保存");
            return;
        }
    }
    else
    {
        std::cerr << "[Detector] Unsupported model format: " << extension << std::endl;
        return;
    }

    std::cout << "[Detector] Loading engine: " << engineFilePath.u8string() << std::endl;
    engine.reset(loadEngineFromFile(engineFilePath.u8string(), runtime.get()));
}

void TrtDetector::processFrame(const cv::Mat& frame, runtime::FrameContext context)
{

    std::unique_lock<std::mutex> lock(inferenceMutex);
    if (shouldExit.load()) return;
    pendingContext = context;
    currentFrame = frame;
    currentFrameGpu.release();
    pendingFrameType = PendingFrameType::Cpu;
    frameReady = true;
    inferenceCV.notify_one();
}

void TrtDetector::processFrameGpu(GpuImage frame, runtime::FrameContext context)
{

    std::unique_lock<std::mutex> lock(inferenceMutex);
    if (shouldExit.load()) return;
    pendingContext = context;
    currentFrame.release();
    currentFrameGpu = std::move(frame);
    pendingFrameType = PendingFrameType::Gpu;
    frameReady = true;
    inferenceCV.notify_one();
}

void TrtDetector::inferenceThread()
{
    std::string mmcssTask;
    std::unique_ptr<sched_boost::ScopedThreadBoost> threadBoost;

    int curr_slot = 0;

    int64_t slotCaptureNs[2] = {0, 0};
    runtime::FrameContext slotContexts[2]{};
    int64_t slotSubmitNs[2]  = {0, 0};
    bool slotUsedGraph[2] = {false, false};
    std::array<GpuImage, 2> inflightGpu;
    std::array<cv::Mat, 2> inflightCpu;

    bool graphCaptureGivenUp = false;

    auto publishSlot = [&](int slot)
    {
        if (slot < 0) return;
        waitForEvent(copyCompleteEvent[slot]);
        publishContext = slotContexts[slot];
        publishCaptureNs = slotCaptureNs[slot];
        publishSubmitNs = slotSubmitNs[slot];
        // The source image is still owned by this inference slot. Refresh its
        // cache entry before publishing detections so the sampler can save the
        // exact frame even if inference took longer than the capture cache.
        if (runtime_config::read()->auto_capture_enabled)
        {
            if (!inflightGpu[slot].empty())
                AutoCapture::submit_frame(inflightGpu[slot], publishContext);
            else if (!inflightCpu[slot].empty())
                AutoCapture::submit_frame(inflightCpu[slot], publishContext);
        }

        const auto postStart = std::chrono::steady_clock::now();
        frameAimTickMs = 0.0;
        auto& pinned = pinnedSlot(slot);
        for (const auto& name : outputNames)
        {
            const auto it = pinned.find(name);
            if (it != pinned.end() && it->second)
                postProcess(it->second, name, outputTypes[name], &lastNmsTimeValue);
        }
        const auto postEnd = std::chrono::steady_clock::now();

        float preprocessMs = 0.0f;
        float inferenceMs = 0.0f;
        float copyMs = 0.0f;
        cudaEventElapsedTime(&preprocessMs, preprocessStartEvent[slot], inferenceStartEvent[slot]);
        cudaEventElapsedTime(&inferenceMs, inferenceStartEvent[slot], inferenceCompleteEvent[slot]);
        cudaEventElapsedTime(&copyMs, inferenceCompleteEvent[slot], copyCompleteEvent[slot]);
        const double postprocessMs = std::max(
            0.0, std::chrono::duration<double, std::milli>(postEnd - postStart).count()
                     - frameAimTickMs);
        const bool usedGraph = slotUsedGraph[slot];
        lastPreprocessTimeValue = std::chrono::duration<double, std::milli>(
            usedGraph ? -1.0 : preprocessMs);
        lastInferenceTimeValue = std::chrono::duration<double, std::milli>(
            usedGraph ? preprocessMs + inferenceMs + copyMs : inferenceMs);
        runtime::latency::notePipelineTimes(
            usedGraph, preprocessMs, inferenceMs, copyMs, postprocessMs, frameAimTickMs);
        runtime::latency::noteSyncWait(lastSyncSpinMs, lastSyncUsedSpin, syncFallbackCount);
        lastCopyTimeValue = std::chrono::duration<double, std::milli>(
            usedGraph ? -1.0 : copyMs);
        lastPostprocessTimeValue = std::chrono::duration<double, std::milli>(postprocessMs);
        inflightGpu[slot].release();
        inflightCpu[slot].release();
    };

    while (!shouldExit)
    {
        const auto scheduling = runtime_config::read();
        if (!scheduling->use_mmcss)
        {
            threadBoost.reset();
            mmcssTask.clear();
        }
        else if (!threadBoost || mmcssTask != scheduling->mmcss_task_name)
        {
            threadBoost.reset();
            mmcssTask = scheduling->mmcss_task_name;
            threadBoost = std::make_unique<sched_boost::ScopedThreadBoost>(mmcssTask.c_str());
            if (threadBoost->active())
                std::cout << "[Detector] Inference thread boosted (MMCSS task="
                          << mmcssTask << ")" << std::endl;
        }
        if (detector_model_changed.load())
        {
            {
                std::unique_lock<std::mutex> lock(inferenceMutex);
                currentFrame.release();
                currentFrameGpu.release();
                frameReady = false;
                pendingFrameType = PendingFrameType::None;
            }
            if (!initialize("models/" + runtime_config::read()->ai_model))
            {
                std::cerr << "[Detector] Model switch failed; stopping inference session." << std::endl;
                session_stop_requested.store(true);
                requestExit();
                break;
            }
            publishTrtModelMetadata(*this);
            detection_resolution_changed.store(true);
            detector_model_changed.store(false);
            slotCaptureNs[0] = slotCaptureNs[1] = 0;
            slotSubmitNs[0]  = slotSubmitNs[1]  = 0;
            curr_slot = 0;
            graphCaptureGivenUp = false;
        }

        if (useCudaGraph != runtime_config::read()->use_cuda_graph)
        {
            useCudaGraph = runtime_config::read()->use_cuda_graph;
            if (!useCudaGraph)
                destroyCudaGraph();
        }

        cv::Mat frame;
        GpuImage frameGpu;
        PendingFrameType frameType = PendingFrameType::None;
        bool hasNewFrame = false;

        {
            std::unique_lock<std::mutex> lock(inferenceMutex);
            if (!frameReady && !shouldExit)
                inferenceCV.wait(lock, [this] { return frameReady || shouldExit; });

            if (shouldExit) break;

            if (frameReady)
            {
                slotContexts[curr_slot] = pendingContext;
                slotCaptureNs[curr_slot] = pendingContext.captured_ns;
                runtime::latency::SubmitStamp consumed{pendingContext.captured_ns, 0, pendingContext.sequence};
                runtime::latency::markDetectorConsume(consumed);
                slotSubmitNs[curr_slot] = consumed.submit_ns;

                frameType = pendingFrameType;
                if (frameType == PendingFrameType::Gpu)
                {
                    frameGpu = std::move(currentFrameGpu);
                    currentFrameGpu.release();
                    currentFrame.release();
                }
                else
                {
                    frame = std::move(currentFrame);
                    currentFrameGpu.release();
                }
                pendingFrameType = PendingFrameType::None;
                frameReady = false;
                hasNewFrame = true;
            }
        }

        if (!context)
        {
            if (!error_logged)
            {
                std::cerr << "[Detector] Context not initialized" << std::endl;
                error_logged = true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }
        else
        {
            error_logged = false;
        }

        if (hasNewFrame)
        {
            const bool hasCpuFrame = (frameType == PendingFrameType::Cpu && !frame.empty());
            const bool hasGpuFrame = (frameType == PendingFrameType::Gpu && !frameGpu.empty());
            if (!hasCpuFrame && !hasGpuFrame)
                continue;

            try
            {
                if (hasGpuFrame)
                {
                    cudaEvent_t frameReadyEvent = frameGpu.readyEvent();
                    if (frameReadyEvent)
                        cudaStreamWaitEvent(stream, frameReadyEvent, 0);
                }

                bool usedGraph = false;
                if (useCudaGraph && !graphCaptureGivenUp)
                {
                    int frameRows = 0;
                    int frameCols = 0;
                    int frameChannels = 0;
                    if (hasGpuFrame)
                    {
                        frameRows = frameGpu.rows();
                        frameCols = frameGpu.cols();
                        frameChannels = frameGpu.channels();
                    }
                    else
                    {
                        frameRows = frame.rows;
                        frameCols = frame.cols;
                        frameChannels = frame.channels();
                    }

                    if (frameRows > 0 && frameCols > 0 && frameChannels > 0)
                    {
                        const GraphStagingResult staging =
                            ensureGraphStaging(frameRows, frameCols, frameChannels);
                        if (staging == GraphStagingResult::Failed)
                        {
                            graphCaptureGivenUp = true;
                        }
                        else if (staging == GraphStagingResult::Capture ||
                                 !cudaGraphExecs[curr_slot])
                        {
                            bool captureOk = captureCudaGraph(curr_slot);
                            if (!captureOk)
                            {
                                std::cerr << "[Detector] CUDA graph capture failed; "
                                             "falling back to direct enqueue for this session."
                                          << std::endl;
                                destroyCudaGraph();
                                graphCaptureGivenUp = true;
                            }
                        }
                    }

                    if (cudaGraphExecs[curr_slot])
                    {
                        cudaEventRecord(preprocessStartEvent[curr_slot], stream);
                        auto& staging = graphInputBuffers[curr_slot];
                        if (hasGpuFrame)
                        {
                            const size_t widthBytes =
                                static_cast<size_t>(frameCols) * static_cast<size_t>(frameChannels);
                            cudaMemcpy2DAsync(
                                staging.data(), staging.step(),
                                frameGpu.data(), frameGpu.step(),
                                widthBytes, static_cast<size_t>(frameRows),
                                cudaMemcpyDeviceToDevice, stream
                            );
                        }
                        else
                        {
                            staging.upload(frame.data, frameRows, frameCols, frameChannels,
                                           frame.step, stream);
                        }

                        cudaEventRecord(inferenceStartEvent[curr_slot], stream);
                        launchCudaGraph(curr_slot);
                        cudaEventRecord(inferenceCompleteEvent[curr_slot], stream);
                        cudaEventRecord(copyCompleteEvent[curr_slot], stream);
                        cudaEventRecord(slotDoneEvent[curr_slot], stream);
                        usedGraph = true;

                    }
                }

                if (!usedGraph)
                {
                    cudaEventRecord(preprocessStartEvent[curr_slot], stream);
                    if (hasGpuFrame)
                        preProcess(frameGpu);
                    else
                        preProcess(frame);
                    cudaEventRecord(inferenceStartEvent[curr_slot], stream);
                    context->enqueueV3(stream);
                    cudaEventRecord(inferenceCompleteEvent[curr_slot], stream);

                    auto& curPinned = pinnedSlot(curr_slot);
                    for (const auto& name : outputNames)
                    {
                        auto itPinned = curPinned.find(name);
                        if (itPinned == curPinned.end() || !itPinned->second)
                            continue;

                        cudaMemcpyAsync(
                            itPinned->second, outputBindings[name],
                            outputSizes[name], cudaMemcpyDeviceToHost, stream
                        );
                    }

                    cudaEventRecord(copyCompleteEvent[curr_slot], stream);
                    cudaEventRecord(slotDoneEvent[curr_slot], stream);

                }

                // Keep input storage alive until its asynchronous GPU work finishes.
                inflightGpu[curr_slot] = std::move(frameGpu);
                inflightCpu[curr_slot] = std::move(frame);
                slotUsedGraph[curr_slot] = usedGraph;
                // 本帧完成就发布；推理期间待提交槽只保留最新到达的帧。
                const int completed_slot = curr_slot;
                curr_slot = 1 - curr_slot;
                publishSlot(completed_slot);
            }
            catch (const std::exception& e)
            {
                std::cerr << "[Detector] Error during inference: " << e.what() << std::endl;
            }
        }
    }
}

void TrtDetector::preProcess(const cv::Mat& frame)
{
    if (frame.empty())
        return;

    if (!gpuFrameBuffer.upload(frame.data, frame.rows, frame.cols, frame.channels(),
                               frame.step, stream))
        return;
    preProcess(gpuFrameBuffer);
}

void TrtDetector::preProcess(const GpuImage& frame)
{
    if (frame.empty())
        return;

    void* inputBuffer = inputBindings[inputName];
    if (!inputBuffer)
        return;

    nvinfer1::Dims dims = context->getTensorShape(inputName.c_str());
    int c = 0;
    int h = 0;
    int w = 0;
    if (!tryGetPositiveDimInt(dims.d[1], &c)
        || !tryGetPositiveDimInt(dims.d[2], &h)
        || !tryGetPositiveDimInt(dims.d[3], &w))
    {
        return;
    }

    if (c != 3)
        return;

    const int srcChannels = frame.channels();
    if (srcChannels != 1 && srcChannels != 3 && srcChannels != 4)
        return;

    launch_resize_bgr_u8_to_chw_rgb_f16(
        frame.view(), reinterpret_cast<__half*>(inputBuffer), w, stream
    );

    if (runtime_config::read()->verbose)
    {
        auto err = cudaGetLastError();
        if (err != cudaSuccess)
        {
            std::cerr << "[Detector] preprocess kernel launch error: " << cudaGetErrorString(err) << std::endl;
        }
    }
}

void TrtDetector::postProcess(const void* output, const std::string& outputName,
                              nvinfer1::DataType dtype,
                              std::chrono::duration<double, std::milli>* nmsTime)
{
    const auto shapeIt = outputShapes.find(outputName);
    if (shapeIt == outputShapes.end())
        return;
    const std::vector<int64_t>& shape = shapeIt->second;
    const auto kind = detector::classify_model_output(shape);
    if (kind == detector::ModelOutputKind::Unknown)
        return;

    const float img_scale_local = img_scale;
    const DetectorRuntimeSettings runtime = detectorRuntimeSettings();
    const float baseConf = std::max(runtime.confidenceThreshold, 0.0f);

    auto& detections = detection_scratch_;
    detections.clear();
    if (detections.capacity() < kFixedMaxDetections)
        detections.reserve(kFixedMaxDetections);

    if (kind == detector::ModelOutputKind::RawChannelsFirst
        || kind == detector::ModelOutputKind::RawRowsFirst)
    {
        const auto read = [&](size_t offset) -> float {
            if (dtype == nvinfer1::DataType::kHALF)
                return __half2float(reinterpret_cast<const __half*>(output)[offset]);
            return reinterpret_cast<const float*>(output)[offset];
        };
        detector::decode_raw_yolo(
            shape, kind, read, baseConf, img_scale_local,
            static_cast<float>(model_input_width_), static_cast<float>(model_input_height_),
            raw_candidates_);
        // Discard excluded classes before the top-20 NMS cap so they cannot
        // crowd out valid detections from other classes.
        const auto filters = runtime_config::read();
        if (std::any_of(filters->class_filters.begin(), filters->class_filters.end(),
                [](const auto& filter) { return filter.bucket == ClassBucket::Delete; }))
        {
            raw_candidates_.erase(
                std::remove_if(raw_candidates_.begin(), raw_candidates_.end(),
                    [&filters](const detector::RawYoloCandidate& candidate) {
                        return std::any_of(filters->class_filters.begin(), filters->class_filters.end(),
                            [&candidate](const auto& filter) {
                                return filter.bucket == ClassBucket::Delete
                                    && filter.class_id == candidate.class_id;
                            });
                    }),
                raw_candidates_.end());
        }
        const auto nmsStart = std::chrono::steady_clock::now();
        detector::nms_raw_yolo(raw_candidates_, runtime.nmsThreshold,
                               kFixedMaxDetections, raw_selected_);
        if (nmsTime)
            *nmsTime = std::chrono::steady_clock::now() - nmsStart;

        for (const auto& raw : raw_selected_)
        {
            Detection d;
            d.preciseBox = cv::Rect2f(raw.x1, raw.y1,
                                      raw.x2 - raw.x1, raw.y2 - raw.y1);
            d.box = cv::Rect(
                static_cast<int>(std::lround(d.preciseBox.x)),
                static_cast<int>(std::lround(d.preciseBox.y)),
                static_cast<int>(std::lround(d.preciseBox.width)),
                static_cast<int>(std::lround(d.preciseBox.height)));
            d.confidence = raw.score;
            d.classId = raw.class_id;
            detections.push_back(d);
        }
    }
    else
    {
        const int64_t rows = shape[1];

        auto rowPtr = [&](int64_t i) -> const float* {
            const size_t off = static_cast<size_t>(i) * 6;
            if (dtype == nvinfer1::DataType::kHALF)
            {
                static thread_local std::array<float, 6> row{};
                const __half* h = reinterpret_cast<const __half*>(output) + off;
                for (int k = 0; k < 6; ++k) row[k] = __half2float(h[k]);
                return row.data();
            }
            return reinterpret_cast<const float*>(output) + off;
        };

        for (int64_t i = 0; i < rows; ++i)
        {
            const float* det = rowPtr(i);
            const float confidence = det[4];
            if (!(confidence > baseConf))
                continue;

            const int classId = static_cast<int>(det[5]);

            Detection d;
            d.preciseBox = cv::Rect2f(
                det[0] * img_scale_local, det[1] * img_scale_local,
                (det[2] - det[0]) * img_scale_local,
                (det[3] - det[1]) * img_scale_local);
            d.box.x = static_cast<int>(std::lround(d.preciseBox.x));
            d.box.y = static_cast<int>(std::lround(d.preciseBox.y));
            d.box.width  = static_cast<int>(std::lround(d.preciseBox.width));
            d.box.height = static_cast<int>(std::lround(d.preciseBox.height));
            d.confidence = confidence;
            d.classId = classId;
            detections.push_back(d);
        }

        capDetectionsToMax(detections, kFixedMaxDetections);
        if (nmsTime)
            *nmsTime = std::chrono::duration<double, std::milli>(0);
    }

    applyDeleteBucketFilter(detections);

    {
        std::lock_guard<std::mutex> lock(detectionBuffer.mutex);
        detectionBuffer.boxes.clear();
        detectionBuffer.precise_boxes.clear();
        detectionBuffer.classes.clear();
        detectionBuffer.confidences.clear();

        for (const auto& det : detections)
        {
            detectionBuffer.boxes.push_back(det.box);
            detectionBuffer.precise_boxes.push_back(det.preciseBox);
            detectionBuffer.classes.push_back(det.classId);
            detectionBuffer.confidences.push_back(det.confidence);
        }

        runtime::latency::markInferenceDone(publishSubmitNs);
        detectionBuffer.bumpVersionLocked(publishContext);
        detectionBuffer.cv.notify_all();
    }

    const auto tickStart = std::chrono::steady_clock::now();
    try
    {
        runtime::aim_loop::tick();
    }
    catch (...)
    {
    }
    frameAimTickMs += std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - tickStart).count();
}
