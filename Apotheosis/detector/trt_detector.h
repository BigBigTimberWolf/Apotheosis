#ifndef TRT_DETECTOR_H
#define TRT_DETECTOR_H

#include <opencv2/opencv.hpp>
#include <NvInfer.h>
#include <array>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <vector>
#include <unordered_map>
#include <cuda_fp16.h>
#include <memory>
#include <thread>
#include <chrono>
#include <functional>
#include <cuda_runtime_api.h>

#include "i_detector.h"
#include "runtime/frame_crosshair.h"
#include "postProcess.h"
#include "raw_yolo_postprocess.h"
#include "../mem/gpu_image.h"

class TrtDetector : public IDetector
{
public:
    TrtDetector();
    ~TrtDetector() override;

    DetectorBackend backend() const noexcept override { return DetectorBackend::TensorRT; }
    const char* backendName() const noexcept override { return "TRT"; }

    bool initialize(const std::string& model_path) override;
    void processFrame(const cv::Mat& frame, runtime::FrameContext context = runtime::FrameContext{}) override;
    void processFrameGpu(GpuImage frame, runtime::FrameContext context = runtime::FrameContext{}) override;
    void inferenceThread() override;

    int numberOfClasses() const override { return numClasses; }
    std::vector<std::string> classNames() const override { return class_names_; }

    std::chrono::duration<double, std::milli> lastPreprocessTime() const override { return lastPreprocessTimeValue; }
    std::chrono::duration<double, std::milli> lastInferenceTime() const override { return lastInferenceTimeValue; }
    std::chrono::duration<double, std::milli> lastCopyTime() const override { return lastCopyTimeValue; }
    std::chrono::duration<double, std::milli> lastPostprocessTime() const override { return lastPostprocessTimeValue; }
    std::chrono::duration<double, std::milli> lastNmsTime() const override { return lastNmsTimeValue; }

    void requestExit() override;
    void shutdown();
    const std::string& lastError() const noexcept { return last_error_; }

    float img_scale;

    std::vector<std::string> inputNames;
    std::vector<std::string> outputNames;
    std::unordered_map<std::string, size_t> outputSizes;

    std::chrono::duration<double, std::milli> lastPreprocessTimeValue{};
    std::chrono::duration<double, std::milli> lastInferenceTimeValue{};
    std::chrono::duration<double, std::milli> lastCopyTimeValue{};
    std::chrono::duration<double, std::milli> lastPostprocessTimeValue{};
    std::chrono::duration<double, std::milli> lastNmsTimeValue{};
    double frameAimTickMs = 0.0;

private:
    std::unique_ptr<nvinfer1::IRuntime> runtime;
    std::unique_ptr<nvinfer1::ICudaEngine> engine;
    std::unique_ptr<nvinfer1::IExecutionContext> context;

    cudaStream_t stream;

    bool useCudaGraph;
    bool cudaGraphCaptured;
    std::array<cudaGraph_t, 2> cudaGraphs{ nullptr, nullptr };
    std::array<cudaGraphExec_t, 2> cudaGraphExecs{ nullptr, nullptr };
    std::array<GpuImage, 2> graphInputBuffers;
    int graphInputRows = 0;
    int graphInputCols = 0;
    int graphInputChannels = 0;
    size_t graphInputStep = 0;
    bool captureCudaGraph(int slot);
    void launchCudaGraph(int slot);
    void destroyCudaGraph();
    enum class GraphStagingResult { Failed, Reuse, Capture };
    GraphStagingResult ensureGraphStaging(int rows, int cols, int channels);

    std::unordered_map<std::string, void*> pinnedOutputBuffers;
    std::unordered_map<std::string, void*> pinnedOutputBuffersB;
    std::array<cudaEvent_t, 2> slotDoneEvent{ nullptr, nullptr };
    std::unordered_map<std::string, void*>& pinnedSlot(int s) {
        return s == 0 ? pinnedOutputBuffers : pinnedOutputBuffersB;
    }
    void allocatePinnedOutputs();
    void freePinnedOutputs();
    void releaseModelResources();

    void waitForEvent(cudaEvent_t ev);
    double lastSyncSpinMs = 0.0;
    bool   lastSyncUsedSpin = false;
    long   syncFallbackCount = 0;

    std::mutex inferenceMutex;
    std::condition_variable inferenceCV;
    std::atomic<bool> shouldExit;
    cv::Mat currentFrame;
    GpuImage currentFrameGpu;
    bool frameReady;

    enum class PendingFrameType
    {
        None = 0,
        Cpu = 1,
        Gpu = 2
    };
    PendingFrameType pendingFrameType = PendingFrameType::None;

    runtime::FrameContext pendingContext, publishContext;
    runtime::FrameCrosshair publishCrosshair;
    int64_t publishCaptureNs = 0;
    int64_t publishSubmitNs  = 0;

    void loadEngine(const std::string& engineFile);

    void preProcess(const cv::Mat& frame);
    void preProcess(const GpuImage& frame);

    GpuImage gpuFrameBuffer;

    void postProcess(
        const void* output,
        const std::string& outputName,
        nvinfer1::DataType dtype,
        std::chrono::duration<double, std::milli>* nmsTime
    );

    void getInputNames();
    void getOutputNames();
    void getBindings();

    std::unordered_map<std::string, size_t> inputSizes;
    std::unordered_map<std::string, void*> inputBindings;
    std::unordered_map<std::string, void*> outputBindings;
    std::unordered_map<std::string, std::vector<int64_t>> outputShapes;
    int numClasses;
    std::vector<std::string> class_names_;
    std::string last_error_;
    std::vector<detector::RawYoloCandidate> raw_candidates_;
    std::vector<detector::RawYoloCandidate> raw_selected_;
    std::vector<Detection> detection_scratch_;
    int model_input_width_ = 0;
    int model_input_height_ = 0;

    size_t getSizeByDim(const nvinfer1::Dims& dims);
    size_t getElementSize(nvinfer1::DataType dtype);

    std::string inputName;
    void* inputBufferDevice;

    std::unordered_map<std::string, nvinfer1::DataType> outputTypes;

    void freeTransposedBuffers();

    std::array<cudaEvent_t, 2> preprocessStartEvent{ nullptr, nullptr };
    std::array<cudaEvent_t, 2> inferenceStartEvent{ nullptr, nullptr };
    std::array<cudaEvent_t, 2> inferenceCompleteEvent{ nullptr, nullptr };
    std::array<cudaEvent_t, 2> copyCompleteEvent{ nullptr, nullptr };
    bool asyncInferenceInProgress = false;
};

#endif // TRT_DETECTOR_H
