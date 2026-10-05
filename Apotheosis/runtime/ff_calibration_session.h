#pragma once

#include "control/ff_calibration.h"
#include <array>
#include <atomic>
#include <chrono>
#include <mutex>

namespace runtime {

inline int64_t ffCalibrationNowUs() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

// UI arms a session; the ordinary aim worker supplies existing detections and
// successful driver events. The inactive fast path is one atomic load.
class FfCalibrationSession {
public:
    enum class State { Idle, Armed, Sampling, Ready, Failed, Cancelled };
    struct Snapshot {
        State state = State::Idle;
        int hotkey = -1, bank = 0, completed = 0;
        const char* message = "未开始";
        std::vector<control::FfCalibrationObservation> observations;
        std::vector<control::FfCalibrationMove> moves;
    };
    static FfCalibrationSession& instance() { static FfCalibrationSession s; return s; }
    bool active() const { return active_.load(std::memory_order_acquire); }
    bool arm(int hotkey, int bank) {
        std::lock_guard lock(mutex_);
        if (active() || hotkey < 0 || bank < 0 || bank > 2) return false;
        data_ = {}; data_.state = State::Armed; data_.hotkey = hotkey; data_.bank = bank;
        data_.message = "保持视角和静止目标，按住所选热键开始；松开即停止";
        armedUs_ = ffCalibrationNowUs(); startedUs_ = lastFrameUs_ = nextPulseUs_ = 0;
        queuedUs_ = 0; pulse_ = 0; failureBaseline_ = 0;
        active_.store(true, std::memory_order_release);
        return true;
    }
    void cancel(const char* message = "已取消，原标定参数保留") {
        std::lock_guard lock(mutex_);
        if (data_.state == State::Armed || data_.state == State::Sampling || data_.state == State::Ready) {
            data_.state = State::Cancelled; data_.message = message;
        }
        active_.store(false, std::memory_order_release);
    }
    void fail(const char* message) {
        std::lock_guard lock(mutex_); failLocked(message);
    }
    void released() {
        std::lock_guard lock(mutex_);
        if (data_.state == State::Sampling) failLocked("热键已松开，标定中止，原参数保留");
        if (data_.state == State::Ready) active_.store(false, std::memory_order_release);
    }
    Snapshot snapshot(bool includeSamples = false) {
        std::lock_guard lock(mutex_);
        if (active() && ffCalibrationNowUs() - armedUs_ > 15000000)
            failLocked("标定等待超时，请重新开始");
        Snapshot s;
        s.state = data_.state; s.hotkey = data_.hotkey; s.bank = data_.bank;
        s.completed = data_.completed; s.message = data_.message;
        if (includeSamples && s.state == State::Ready) {
            s.observations = data_.observations; s.moves = data_.moves;
        }
        return s;
    }
    // Called once at the first activated frame, after clearing queued normal moves.
    bool needsStart() { std::lock_guard lock(mutex_); return data_.state == State::Armed; }
    template<class Send>
    bool dispatch(control::Counts move, Send&& send) {
        std::lock_guard lock(mutex_);
        // Serialize cancellation with the final queue submission. A UI cancel
        // between update() and dispatch() must not enqueue a late probe.
        if (!active() || data_.state != State::Sampling || queuedUs_ == 0 ||
            (move.x == 0 && move.y == 0)) return false;
        send(move);
        return true;
    }
    control::Counts update(int hotkey, int64_t nowUs, int64_t frameUs,
        const std::vector<control::Candidate>& candidates,
        const std::vector<control::FfCalibrationMove>& successfulMoves,
        unsigned long long failedMoves)
    {
        std::lock_guard lock(mutex_);
        if (!active()) return {};
        if (data_.hotkey != hotkey) { failLocked("切换了热键，标定中止"); return {}; }
        if (data_.state == State::Ready) return {}; // wait for release, never resume aim while held
        if (nowUs - armedUs_ > 15000000) { failLocked("标定超时"); return {}; }
        if (frameUs <= 0) { failLocked("没有有效采集时间戳，不能标定响应延迟"); return {}; }
        if (frameUs <= lastFrameUs_) return {};
        if (lastFrameUs_ > 0 && frameUs - lastFrameUs_ > 200000) {
            failLocked("图像间隔过长，标定中止"); return {};
        }
        if (candidates.empty()) {
            failLocked("标定区域需保留一个静止目标；目标丢失时请重试"); return {};
        }
        const control::Candidate* pick = nullptr;
        double bestArea = -1;
        for (const auto& c : candidates) {
            if (!c.box.valid()) continue;
            if (data_.state == State::Sampling && c.classId != classId_) continue;
            double a = c.box.w * c.box.h;
            if (a > bestArea) { bestArea = a; pick = &c; }
        }
        if (!pick) {
            failLocked("标定区域需保留一个静止目标；目标丢失时请重试"); return {};
        }
        const auto& candidate = *pick;
        if (data_.state == State::Armed) {
            data_.state = State::Sampling; startedUs_ = nowUs;
            nextPulseUs_ = nowUs + 400000; initialBox_ = candidate.box;
            classId_ = candidate.classId; failureBaseline_ = failedMoves;
            data_.observations.reserve(4096); data_.moves.reserve(kPulses.size());
            data_.message = "正在标定：不要移动鼠标、走位、开火或切换倍率";
        }
        if (failedMoves != failureBaseline_) { failLocked("鼠标发送失败，标定中止"); return {}; }
        if (candidate.classId != classId_ ||
            candidate.box.w / initialBox_.w < 0.85 || candidate.box.w / initialBox_.w > 1.15 ||
            candidate.box.h / initialBox_.h < 0.85 || candidate.box.h / initialBox_.h > 1.15) {
            failLocked("目标或倍率发生变化，标定中止"); return {};
        }
        for (const auto& move : successfulMoves) {
            if (move.timeUs < startedUs_) continue; // residual prior sends are settled in the baseline
            if (queuedUs_ == 0 || move.timeUs < queuedUs_ ||
                move.counts.x != kPulses[pulse_].x || move.counts.y != kPulses[pulse_].y) {
                failLocked("出现非标定位移，标定中止"); return {};
            }
            data_.moves.push_back(move); queuedUs_ = 0; ++pulse_;
            data_.completed = int(pulse_); nextPulseUs_ = move.timeUs + 350000;
        }
        if (data_.observations.size() >= 4096) { failLocked("采样数量超限，请降低采集帧率后重试"); return {}; }
        data_.observations.push_back({frameUs, candidate.box.center()}); lastFrameUs_ = frameUs;
        if (data_.moves.empty() && queuedUs_ == 0 && nowUs >= nextPulseUs_) {
            // Compare averaged halves, not each box against the first noisy
            // detection. Ordinary detector jitter must not abort a fixed scene.
            const auto half = data_.observations.size() / 2;
            control::Vec2 first, second;
            for (size_t i = 0; i < data_.observations.size(); ++i)
                (i < half ? first : second) += data_.observations[i].center;
            if (half < 3 || (first / double(half) - second /
                double(data_.observations.size() - half)).normSq() > 2.25) {
                failLocked("开始前目标不稳定，请保持目标和视角静止"); return {};
            }
        }
        if (queuedUs_ != 0) {
            if (nowUs - queuedUs_ > 300000) failLocked("未收到鼠标成功发送反馈，标定中止");
            return {};
        }
        if (pulse_ == kPulses.size()) {
            if (frameUs >= data_.moves.back().timeUs + 350000) {
                data_.state = State::Ready; data_.message = "采样完成，请松开热键后查看并应用结果";
            }
            return {};
        }
        if (nowUs < nextPulseUs_) return {};
        queuedUs_ = nowUs;
        return kPulses[pulse_];
    }
private:
    void failLocked(const char* message) {
        data_.state = State::Failed; data_.message = message;
        active_.store(false, std::memory_order_release);
    }
    // Net-zero, different amplitudes and directions identify gain and lag.
    inline static constexpr std::array<control::Counts, 16> kPulses{{
        {8,0},{-8,0},{-12,0},{12,0},{16,0},{-16,0},{-10,0},{10,0},
        {0,8},{0,-8},{0,-12},{0,12},{0,16},{0,-16},{0,-10},{0,10}}};
    std::mutex mutex_;
    std::atomic<bool> active_{false};
    Snapshot data_;
    int64_t armedUs_ = 0, startedUs_ = 0, lastFrameUs_ = 0, nextPulseUs_ = 0, queuedUs_ = 0;
    size_t pulse_ = 0;
    unsigned long long failureBaseline_ = 0;
    control::Box initialBox_;
    int classId_ = -1;
};

} // namespace runtime
