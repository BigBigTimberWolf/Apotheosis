#pragma once

#include <NvInfer.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iterator>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

enum class TrtBuildStage {
    Idle,
    Reading,
    Parsing,
    Configuring,
    Optimizing,
    Loading,
    Saving,
    Complete,
    Failed
};

struct TrtBuildSnapshot {
    uint64_t generation = 0;
    TrtBuildStage stage = TrtBuildStage::Idle;
    std::string model;
    std::string detail;
    std::string trtPhase;
    int phaseStep = 0;
    int phaseMax = 0;
    long long startedMs = 0;
    bool active = false;
};

inline std::mutex gTrtBuildMutex;
inline TrtBuildSnapshot gTrtBuildStatus;
struct TrtBuildPhase {
    std::string name;
    int step = 0;
    int max = 0;
};
inline std::vector<TrtBuildPhase> gTrtBuildPhases;

inline void TrtBuildSyncPhaseLocked()
{
    if (gTrtBuildPhases.empty())
    {
        gTrtBuildStatus.trtPhase.clear();
        gTrtBuildStatus.phaseStep = 0;
        gTrtBuildStatus.phaseMax = 0;
        return;
    }
    const auto& phase = gTrtBuildPhases.back();
    gTrtBuildStatus.trtPhase = phase.name;
    gTrtBuildStatus.phaseStep = phase.step;
    gTrtBuildStatus.phaseMax = phase.max;
}

inline long long TrtNowMs() noexcept
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

inline TrtBuildSnapshot TrtBuildRead()
{
    std::lock_guard<std::mutex> lock(gTrtBuildMutex);
    return gTrtBuildStatus;
}

inline void TrtBuildBegin(std::string model)
{
    std::lock_guard<std::mutex> lock(gTrtBuildMutex);
    const uint64_t nextGeneration = gTrtBuildStatus.generation + 1;
    gTrtBuildStatus = {};
    gTrtBuildPhases.clear();
    gTrtBuildStatus.generation = nextGeneration;
    gTrtBuildStatus.stage = TrtBuildStage::Reading;
    gTrtBuildStatus.model = std::move(model);
    gTrtBuildStatus.startedMs = TrtNowMs();
    gTrtBuildStatus.active = true;
}

inline void TrtBuildSetStage(TrtBuildStage stage, std::string detail = {})
{
    std::lock_guard<std::mutex> lock(gTrtBuildMutex);
    if (!gTrtBuildStatus.active) return;
    gTrtBuildStatus.stage = stage;
    gTrtBuildStatus.detail = std::move(detail);
    gTrtBuildPhases.clear();
    TrtBuildSyncPhaseLocked();
}

inline void TrtBuildSetError(std::string detail)
{
    std::lock_guard<std::mutex> lock(gTrtBuildMutex);
    if (gTrtBuildStatus.active && gTrtBuildStatus.detail.empty())
        gTrtBuildStatus.detail = std::move(detail);
}

inline void TrtBuildFinish(bool success, std::string detail = {})
{
    std::lock_guard<std::mutex> lock(gTrtBuildMutex);
    if (!gTrtBuildStatus.active) return;
    gTrtBuildStatus.stage = success ? TrtBuildStage::Complete : TrtBuildStage::Failed;
    if (success)
        gTrtBuildStatus.detail = std::move(detail);
    else if (!detail.empty())
        gTrtBuildStatus.detail = std::move(detail);
    else if (gTrtBuildStatus.detail.empty())
        gTrtBuildStatus.detail = "构建失败，请查看日志";
    gTrtBuildPhases.clear();
    TrtBuildSyncPhaseLocked();
    gTrtBuildStatus.active = false;
}

class TrtBuildScope {
public:
    explicit TrtBuildScope(std::string model) { TrtBuildBegin(std::move(model)); }
    ~TrtBuildScope() { TrtBuildFinish(false, ""); }
    void succeed(std::string detail = {}) { TrtBuildFinish(true, std::move(detail)); }
};

class TrtProgressMonitor : public nvinfer1::IProgressMonitor {
public:
    void phaseStart(const char* phaseName, const char*, int32_t nbSteps) noexcept override {
        std::lock_guard<std::mutex> lock(gTrtBuildMutex);
        if (!gTrtBuildStatus.active) return;
        gTrtBuildPhases.push_back({phaseName ? phaseName : "", 0,
                                   std::max(0, static_cast<int>(nbSteps))});
        TrtBuildSyncPhaseLocked();
    }

    bool stepComplete(const char* phaseName, int32_t step) noexcept override {
        std::lock_guard<std::mutex> lock(gTrtBuildMutex);
        if (gTrtBuildStatus.active && phaseName)
            for (auto it = gTrtBuildPhases.rbegin(); it != gTrtBuildPhases.rend(); ++it)
                if (it->name == phaseName)
                {
                    it->step = std::clamp(static_cast<int>(step), 0, it->max);
                    TrtBuildSyncPhaseLocked();
                    break;
                }
        return true;
    }

    void phaseFinish(const char* phaseName) noexcept override {
        std::lock_guard<std::mutex> lock(gTrtBuildMutex);
        if (!gTrtBuildStatus.active || !phaseName) return;
        for (auto it = gTrtBuildPhases.rbegin(); it != gTrtBuildPhases.rend(); ++it)
            if (it->name == phaseName)
            {
                gTrtBuildPhases.erase(std::next(it).base());
                TrtBuildSyncPhaseLocked();
                break;
            }
    }
};
