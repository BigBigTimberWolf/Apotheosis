#include "tensorrt/trt_monitor.h"

#include <stdexcept>

void require(bool condition)
{
    if (!condition) throw std::runtime_error("TensorRT build monitor regression");
}

int main()
{
    const auto oldGeneration = TrtBuildRead().generation;
    TrtBuildScope build("fixed-416.onnx");
    auto state = TrtBuildRead();
    require(state.active && state.generation == oldGeneration + 1);
    require(state.model == "fixed-416.onnx" && state.stage == TrtBuildStage::Reading);

    TrtBuildSetStage(TrtBuildStage::Optimizing);
    TrtProgressMonitor monitor;
    monitor.phaseStart("parent", nullptr, 10);
    monitor.stepComplete("parent", 3);
    monitor.phaseStart("child", "parent", 4);
    monitor.stepComplete("child", 2);
    state = TrtBuildRead();
    require(state.trtPhase == "child" && state.phaseStep == 2 && state.phaseMax == 4);
    monitor.phaseFinish("child");
    state = TrtBuildRead();
    require(state.trtPhase == "parent" && state.phaseStep == 3 && state.phaseMax == 10);
    monitor.phaseFinish("parent");
    require(TrtBuildRead().trtPhase.empty());

    TrtBuildSetStage(TrtBuildStage::Loading);
    require(TrtBuildRead().stage == TrtBuildStage::Loading);
    build.succeed();
    state = TrtBuildRead();
    require(!state.active && state.stage == TrtBuildStage::Complete);

    {
        TrtBuildScope failed("bad.onnx");
    }
    state = TrtBuildRead();
    require(!state.active && state.stage == TrtBuildStage::Failed);
    require(!state.detail.empty());

    {
        TrtBuildScope failed("bad-fp16.onnx");
        TrtBuildSetStage(TrtBuildStage::Optimizing);
        TrtBuildSetError("TensorRT native error");
    }
    state = TrtBuildRead();
    require(state.stage == TrtBuildStage::Failed);
    require(state.detail == "TensorRT native error");
}
