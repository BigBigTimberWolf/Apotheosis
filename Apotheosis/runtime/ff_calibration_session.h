#pragma once

#include "control/ff_calibration.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <mutex>
#include <vector>

namespace runtime {

inline int64_t ffCalibrationNowUs() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

// The optional calibration start key. One tap is equivalent to pressing the
// dialog's "开始标定" button. The keyboard thread samples it (reading a key costs
// the same device round-trip as the aim hotkeys) and counts presses, so the
// dialog's slow polling can never miss a short tap. The value only ever grows.
inline std::atomic<unsigned long long> g_ffCalibrationKeyTaps{0};

// UI arms a session; the ordinary aim worker supplies existing detections and
// successful driver events. The inactive fast path is one atomic load.
class FfCalibrationSession {
public:
    enum class State { Idle, Armed, Sampling, Ready, Failed, Cancelled };
    struct Snapshot {
        State state = State::Idle;
        int hotkey = -1, bank = 0, completed = 0, total = 0;
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
        data_.total = kPulseCount;
        data_.message = "保持视角和静止目标，按住所选热键开始；松开即停止";
        armedUs_ = ffCalibrationNowUs(); startedUs_ = lastFrameUs_ = nextPulseUs_ = 0;
        queuedUs_ = 0; pulse_ = 0; failureBaseline_ = 0;
        resetMeasurement();
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
        if (active()) {
            const auto now = ffCalibrationNowUs();
            // Waiting and sampling are timed separately: the arm window is a
            // convenience timer, the sampling window measures the recording.
            if (data_.state == State::Armed && now - armedUs_ > kArmTimeoutUs)
                failLocked("标定等待超时，请重新开始");
            else if (data_.state == State::Sampling && now - startedUs_ > kSamplingTimeoutUs)
                failLocked("标定超时");
        }
        Snapshot s;
        s.state = data_.state; s.hotkey = data_.hotkey; s.bank = data_.bank;
        s.completed = data_.completed; s.total = data_.total; s.message = data_.message;
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
        if (data_.state == State::Armed && nowUs - armedUs_ > kArmTimeoutUs) {
            failLocked("标定等待超时"); return {};
        }
        if (data_.state == State::Sampling && nowUs - startedUs_ > kSamplingTimeoutUs) {
            failLocked("标定超时"); return {};
        }
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
            data_.observations.reserve(4096); data_.moves.reserve(kPulseCount);
            resetMeasurement();
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
                move.counts.x != dispatched_.x || move.counts.y != dispatched_.y) {
                failLocked("出现非标定位移，标定中止"); return {};
            }
            data_.moves.push_back(move); queuedUs_ = 0; ++pulse_;
            data_.completed = int(pulse_); nextPulseUs_ = move.timeUs + kPulsePeriodUs;
            // Only frames captured after the send has taken effect belong to the
            // settled plateau that measures this pulse.
            settleFromUs_ = move.timeUs + kSettleUs;
            if (pulse_ % 2 == 0) adaptAmplitude(pairAxis(int(pulse_) - 2));
        }
        if (data_.observations.size() >= 4096) { failLocked("采样数量超限，请降低采集帧率后重试"); return {}; }
        data_.observations.push_back({frameUs, candidate.box.center()}); lastFrameUs_ = frameUs;
        if (frameUs >= settleFromUs_) { plateauSum_ += candidate.box.center(); ++plateauFrames_; }
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
        if (pulse_ == kPulseCount) {
            if (frameUs >= data_.moves.back().timeUs + kPulsePeriodUs) {
                data_.state = State::Ready; data_.message = "采样完成，请松开热键后查看并应用结果";
            }
            return {};
        }
        if (nowUs < nextPulseUs_) return {};
        // Dispatching closes the plateau that holds the previous pulse response,
        // which is the only measurement of how many pixels one count moves.
        closePlateau();
        pendingResponse_ = true;
        pendingIndex_ = int(pulse_);
        pendingAxis_ = pairAxis(int(pulse_));
        pendingAmplitude_ = amplitude_[pendingAxis_];
        queuedUs_ = nowUs;
        dispatched_ = pulseAt(pulse_);
        return dispatched_;
    }
private:
    // Pulses travel in opposite pairs per axis, alternating axes. The amplitude
    // of the next pair on an axis is derived from the response measured for the
    // pair before it, so low-sensitivity and high-zoom scenes still move enough
    // pixels for the fit while a high-gain scene is not pushed out of the FOV.
    static constexpr size_t kPairs = 8;              // 4 pairs per axis
    static constexpr size_t kPulseCount = kPairs * 2;
    static constexpr int kInitialAmplitude = 8;
    static constexpr int kMinimumAmplitude = 3;
    static constexpr int kMaximumAmplitude = 384;
    static constexpr double kTargetTravelPx = 20.0;  // aim for this much travel
    static constexpr double kAcceptTravelPx = 10.0;  // already enough for the fit
    static constexpr double kRejectTravelPx = 40.0;  // too far, could lose the target
    static constexpr int64_t kPulsePeriodUs = 350000;
    static constexpr int64_t kSettleUs = 180000;     // capture pipeline + response lag
    static constexpr int64_t kArmTimeoutUs = 60000000;
    static constexpr int64_t kSamplingTimeoutUs = 30000000;
    static int pairAxis(int index) { return (index / 2) % 2; }
    control::Counts pulseAt(size_t index) const {
        const int axis = pairAxis(int(index));
        const int sign = (index % 2 == 0) ? 1 : -1;
        const int amplitude = amplitude_[axis];
        return axis == 0 ? control::Counts{sign * amplitude, 0}
                         : control::Counts{0, sign * amplitude};
    }
    void resetMeasurement() {
        amplitude_[0] = amplitude_[1] = kInitialAmplitude;
        travelPx_[0] = travelPx_[1] = 0;
        travelAmplitude_[0] = travelAmplitude_[1] = kInitialAmplitude;
        measuredPulse_[0] = measuredPulse_[1] = -1;
        plateauSum_ = {}; plateauFrames_ = 0; previousMean_ = {};
        havePrevious_ = false; pendingResponse_ = false;
        pendingIndex_ = -1; pendingAxis_ = 0; pendingAmplitude_ = 0;
        settleFromUs_ = 0; dispatched_ = {};
    }
    // Closes the settled window and turns it into a response measurement for the
    // pulse that was in flight when the window opened.
    void closePlateau() {
        const bool hadFrames = plateauFrames_ > 0;
        const control::Vec2 mean = hadFrames
            ? plateauSum_ / double(plateauFrames_) : control::Vec2{};
        plateauSum_ = {}; plateauFrames_ = 0;
        if (hadFrames && pendingResponse_ && havePrevious_) {
            const double delta = pendingAxis_ == 0 ? (mean.x - previousMean_.x)
                                                   : (mean.y - previousMean_.y);
            travelPx_[pendingAxis_] = std::abs(delta);
            travelAmplitude_[pendingAxis_] = pendingAmplitude_;
            measuredPulse_[pendingAxis_] = pendingIndex_;
        }
        havePrevious_ = hadFrames; previousMean_ = mean;
        if (!hadFrames) pendingResponse_ = false;
    }
    void adaptAmplitude(int axis) {
        const int amplitude = amplitude_[axis];
        const bool measured = measuredPulse_[axis] == int(pulse_) - 2;
        const double travel = travelPx_[axis];
        int next = amplitude;
        if (measured && travel > 0.5) {
            const int measuredWith = std::max(1, travelAmplitude_[axis]);
            const double gain = travel / double(measuredWith); // px per count
            if (travel < kAcceptTravelPx || travel > kRejectTravelPx) {
                const int wanted = gain > 1e-3 ? int(std::lround(kTargetTravelPx / gain))
                                               : amplitude * 4;
                // One step may not change the amplitude by more than 4x, so a
                // single noisy measurement cannot throw the target out of view.
                next = std::clamp(wanted, std::max(1, amplitude / 4), amplitude * 4);
            }
        } else {
            next = amplitude * 4; // nothing measurable yet: move further
        }
        amplitude_[axis] = std::clamp(next, kMinimumAmplitude, kMaximumAmplitude);
    }
    void failLocked(const char* message) {
        data_.state = State::Failed; data_.message = message;
        active_.store(false, std::memory_order_release);
    }
    std::mutex mutex_;
    std::atomic<bool> active_{false};
    Snapshot data_;
    int64_t armedUs_ = 0, startedUs_ = 0, lastFrameUs_ = 0, nextPulseUs_ = 0, queuedUs_ = 0;
    size_t pulse_ = 0;
    unsigned long long failureBaseline_ = 0;
    control::Box initialBox_;
    int classId_ = -1;
    // Adaptive pulse plan and its response measurement.
    int amplitude_[2] = {kInitialAmplitude, kInitialAmplitude};
    double travelPx_[2] = {0, 0};
    int travelAmplitude_[2] = {kInitialAmplitude, kInitialAmplitude};
    int measuredPulse_[2] = {-1, -1};
    control::Counts dispatched_{};
    control::Vec2 plateauSum_{}, previousMean_{};
    int plateauFrames_ = 0;
    bool havePrevious_ = false, pendingResponse_ = false;
    int pendingIndex_ = -1, pendingAxis_ = 0, pendingAmplitude_ = 0;
    int64_t settleFromUs_ = 0;
};

} // namespace runtime
