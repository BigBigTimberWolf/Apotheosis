#pragma once

#include <vector>
#include <cmath>
#include <algorithm>
#include <cstdint>
#include <mutex>

namespace control {

// 灵敏度折算系数 (k: px/count) 在线自动测算器
// 用户在训练场对准静止假人/靶子左右甩动鼠标或触发控制器位移，
// 测算器记录 (发出的鼠标计数, 画面目标位移)，使用最小二乘拟合出 k = Δpx / Δcounts。
class SensitivityCalibrator
{
public:
    struct Sample {
        double timeSec = 0.0;
        double anchorPx = 0.0;
        double totalCounts = 0.0;
    };

    struct Status {
        bool running = false;
        int sampleCount = 0;
        int validBatches = 0;        // 已完成的有效拟合批次数 (目标 20 次)
        double progress = 0.0;       // 0.0 ~ 1.0
        double estimatedK = 0.0;      // 拟合出的 k (px/count)
        bool ready = false;
        const char* hint = "";
    };

    void start()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        running_ = true;
        ready_ = false;
        samples_.clear();
        samples_.reserve(1000);
        fittedKs_.clear();
        fittedKs_.reserve(30);
        totalCounts_ = 0.0;
        currTime_ = 0.0;
        bestK_ = 0.0;
        lastFitTime_ = -1.0;
    }

    void stop()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        running_ = false;
    }

    bool isRunning() const { std::lock_guard<std::mutex> lock(mutex_); return running_; }
    bool isReady() const { std::lock_guard<std::mutex> lock(mutex_); return ready_; }
    double estimatedK() const { std::lock_guard<std::mutex> lock(mutex_); return bestK_; }

    void feed(double anchorPx, int countsSent, double dtSec)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!running_ || ready_) return;
        if (!std::isfinite(anchorPx) || !std::isfinite(dtSec) || dtSec <= 0.0) return;

        currTime_ += dtSec;
        totalCounts_ += static_cast<double>(countsSent);

        samples_.push_back({ currTime_, anchorPx, totalCounts_ });

        // 保持最近 4 秒的历史窗口
        while (!samples_.empty() && (currTime_ - samples_.front().timeSec) > 4.0)
        {
            samples_.erase(samples_.begin());
        }

        tryCompute();
    }

    Status status() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        Status s;
        s.running = running_;
        s.sampleCount = static_cast<int>(samples_.size());
        s.validBatches = static_cast<int>(fittedKs_.size());
        s.estimatedK = bestK_;
        s.ready = ready_;

        if (ready_)
        {
            s.progress = 1.0;
            s.hint = "测算完成（20 次高精度均值收敛）！请查看测算值并确认回填。";
        }
        else if (!running_)
        {
            s.progress = 0.0;
            s.hint = "测算已就绪，点击「开始采集」";
        }
        else
        {
            s.progress = std::clamp(static_cast<double>(fittedKs_.size()) / 20.0, 0.0, 1.0);
            s.hint = "正在采集... 请对准静止目标左右甩动鼠标 (持续采集 20 次拟合样本)";
        }
        return s;
    }

private:
    // 界面线程读写状态，推理线程喂样本；两边必须保护同一组数据。
    mutable std::mutex mutex_;

    void tryCompute()
    {
        if (samples_.size() < 25) return;

        // 检查最近 0.25 秒滑动子窗口内的运动激励
        const double now = samples_.back().timeSec;
        if (now - lastFitTime_ < 0.10) return; // 拟合间隔至少 100ms 一次，确保独立样本

        const double windowStart = now - 0.35;
        std::vector<const Sample*> sub;
        for (const auto& sp : samples_)
        {
            if (sp.timeSec >= windowStart)
                sub.push_back(&sp);
        }
        if (sub.size() < 15) return;

        double minC = sub.front()->totalCounts, maxC = sub.front()->totalCounts;
        double minP = sub.front()->anchorPx, maxP = sub.front()->anchorPx;
        for (const auto* sp : sub)
        {
            minC = std::min(minC, sp->totalCounts);
            maxC = std::max(maxC, sp->totalCounts);
            minP = std::min(minP, sp->anchorPx);
            maxP = std::max(maxP, sp->anchorPx);
        }

        const double countsSpan = maxC - minC;
        const double pxSpan = maxP - minP;

        // 该子窗口内的运动激励门限：至少产生 25 个计数与 15 像素位移
        if (countsSpan < 25.0 || pxSpan < 15.0)
            return;

        double sumXX = 0.0;
        double sumXY = 0.0;
        int validPairs = 0;

        for (size_t i = 1; i < sub.size(); ++i)
        {
            const double dCounts = sub[i]->totalCounts - sub[i - 1]->totalCounts;
            const double dPx = sub[i]->anchorPx - sub[i - 1]->anchorPx;
            if (std::abs(dCounts) > 0.0)
            {
                sumXX += dCounts * dCounts;
                sumXY += (-dPx) * dCounts;
                validPairs++;
            }
        }

        if (sumXX > 1e-4 && validPairs >= 10)
        {
            const double rawK = sumXY / sumXX;
            // 物理合法范围：0.1 ~ 3.0 px/count
            if (rawK >= 0.1 && rawK <= 3.0)
            {
                fittedKs_.push_back(rawK);
                lastFitTime_ = now;

                // 实时计算当前已拟合批次的中位数/均值
                std::vector<double> sorted = fittedKs_;
                std::sort(sorted.begin(), sorted.end());
                // 去掉首尾 10% 极值后求平均，抵抗抖动异常值
                size_t trim = sorted.size() / 10;
                double sum = 0.0;
                size_t count = 0;
                for (size_t i = trim; i < sorted.size() - trim; ++i)
                {
                    sum += sorted[i];
                    count++;
                }
                bestK_ = (count > 0) ? (sum / count) : sorted[sorted.size() / 2];

                if (fittedKs_.size() >= 20)
                {
                    ready_ = true;
                    running_ = false;
                }
            }
        }
    }

    bool running_ = false;
    bool ready_ = false;
    double bestK_ = 0.0;
    double currTime_ = 0.0;
    double lastFitTime_ = -1.0;
    double totalCounts_ = 0.0;
    std::vector<Sample> samples_;
    std::vector<double> fittedKs_;
};

inline SensitivityCalibrator& globalSensitivityCalibrator()
{
    static SensitivityCalibrator instance;
    return instance;
}

}
