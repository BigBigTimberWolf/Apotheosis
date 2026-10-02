#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace runtime
{
namespace latency
{

using Clock = std::chrono::steady_clock;

inline int64_t nowNs()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               Clock::now().time_since_epoch())
        .count();
}

inline double nsToMs(int64_t ns)
{
    return static_cast<double>(ns) / 1.0e6;
}

struct CaptureStamp
{
    std::atomic<int64_t>  ns{0};
    std::atomic<uint64_t> seq{0};
};

inline CaptureStamp& captureStamp()
{
    static CaptureStamp s;
    return s;
}

inline int64_t markCapture(int64_t source_ns = 0)
{
    const int64_t ns = source_ns > 0 ? source_ns : nowNs();
    auto& s = captureStamp();
    s.ns.store(ns, std::memory_order_release);
    s.seq.fetch_add(1, std::memory_order_relaxed);
    return ns;
}

inline int64_t loadCaptureNs()
{
    return captureStamp().ns.load(std::memory_order_acquire);
}

inline uint64_t loadCaptureSeq()
{
    return captureStamp().seq.load(std::memory_order_relaxed);
}

inline std::atomic<int>& deviceFrameAgeUs()
{
    static std::atomic<int> v{ -1 };
    return v;
}

inline std::atomic<int64_t>& deviceAgeUpdateNs()
{
    static std::atomic<int64_t> v{0};
    return v;
}

inline void noteDeviceFrameAgeUs(int us)
{
    deviceFrameAgeUs().store(us, std::memory_order_relaxed);
    deviceAgeUpdateNs().store(nowNs(), std::memory_order_release);
}

inline int loadDeviceFrameAgeUs()
{
    if (nowNs() - deviceAgeUpdateNs().load(std::memory_order_acquire) > 2'000'000'000)
        return -1;
    return deviceFrameAgeUs().load(std::memory_order_relaxed);
}

inline std::atomic<int64_t>& submitNs()
{
    static std::atomic<int64_t> v{0};
    return v;
}

inline std::atomic<int64_t>& submittedCaptureNs()
{
    static std::atomic<int64_t> v{0};
    return v;
}

inline int64_t takeSubmittedCaptureNs()
{
    return submittedCaptureNs().load(std::memory_order_acquire);
}

struct SubmitStamp
{
    int64_t capture_ns = 0;
    int64_t submit_ns  = 0;
    uint64_t sequence = 0;
};

struct Stage
{
    double   last_ms = 0.0;
    double   ema_ms  = 0.0;
    double   max_ms  = 0.0;
    uint64_t n       = 0;

    void push(double ms)
    {
        last_ms = ms;
        ema_ms = (n < 10) ? (ema_ms * n + ms) / (n + 1)
                          : (ema_ms * 0.9 + ms * 0.1);
        if (ms > max_ms || n == 0) max_ms = ms;
        ++n;
    }

    void reset()
    {
        last_ms = ema_ms = max_ms = 0.0;
        n = 0;
    }
};

enum StageId
{
    kCaptureWait = 0,
    kInference,
    kPublishToAim,
    kAimToMove,
    kTotal,
    kEndToEnd,
    kStageCount
};

struct Counters
{
    uint64_t frames_consumed   = 0;
    uint64_t detections_seen    = 0;
    uint64_t capture_frames    = 0;
    uint64_t dropped_capture   = 0;
    uint64_t stale_consumes    = 0;
};

struct Shared
{
    std::mutex mu;
    Stage      stages[kStageCount];
    Stage      gpu_preprocess;
    Stage      gpu_engine;
    Stage      gpu_copy;
    Stage      gpu_pipeline;
    Stage      cpu_postprocess;
    Stage      aim_tick;
    bool       last_timing_was_graph = false;
    Counters   counters;
    uint64_t   last_capture_seq_seen = 0;
    int64_t    last_capture_ns_seen  = 0;

    double     capture_interval_ms   = 0.0;
    int64_t    prev_capture_ns       = 0;

    double     engine_inference_ms   = -1.0;
    int64_t    engine_update_ns      = 0;
    int64_t    reset_ns              = 0;
    bool       enabled               = true;

    double     sync_wait_ms          = -1.0;
    bool       sync_spun             = false;
    uint64_t   sync_fallbacks        = 0;
};

inline Shared& shared()
{
    static Shared s;
    return s;
}

inline void noteCaptureForStats(int64_t ns)
{
    auto& sh = shared();
    std::lock_guard<std::mutex> lk(sh.mu);
    if (sh.prev_capture_ns != 0)
    {
        const double dt = nsToMs(ns - sh.prev_capture_ns);
        if (dt > 0.0 && dt < 2000.0)
            sh.capture_interval_ms = (sh.capture_interval_ms <= 0.0)
                                         ? dt
                                         : sh.capture_interval_ms * 0.9 + dt * 0.1;
    }
    sh.prev_capture_ns = ns;
    sh.counters.capture_frames++;
}

inline SubmitStamp markSubmitStamp()
{
    return {loadCaptureNs(), 0, loadCaptureSeq()};
}

inline void markDetectorConsume(SubmitStamp& stamp)
{
    stamp.submit_ns = nowNs();
    submitNs().store(stamp.submit_ns, std::memory_order_release);
    submittedCaptureNs().store(stamp.capture_ns, std::memory_order_release);
    auto& sh = shared();
    std::lock_guard<std::mutex> lk(sh.mu);
    if (stamp.capture_ns > 0)
        sh.stages[kCaptureWait].push(nsToMs(stamp.submit_ns - stamp.capture_ns));
    if (stamp.sequence > sh.last_capture_seq_seen + 1)
        sh.counters.dropped_capture += stamp.sequence - sh.last_capture_seq_seen - 1;
    sh.last_capture_seq_seen = stamp.sequence;
    sh.last_capture_ns_seen = stamp.capture_ns;
}

inline int64_t markSubmit()
{
    auto stamp = markSubmitStamp();
    markDetectorConsume(stamp);
    return stamp.capture_ns;
}

inline void markInferenceDone(int64_t submit_ns = -1)
{
    const int64_t sub = (submit_ns >= 0)
        ? submit_ns
        : submitNs().load(std::memory_order_acquire);
    if (sub == 0) return;
    auto& sh = shared();
    std::lock_guard<std::mutex> lk(sh.mu);
    sh.stages[kInference].push(nsToMs(nowNs() - sub));
}

inline void noteDetectionSeen()
{
    auto& sh = shared();
    std::lock_guard<std::mutex> lk(sh.mu);
    sh.counters.detections_seen++;
}

inline int64_t markAimConsume(int64_t frame_capture_ns, int64_t publish_ns)
{
    auto& sh = shared();
    const int64_t now = nowNs();
    std::lock_guard<std::mutex> lk(sh.mu);
    sh.stages[kPublishToAim].push(publish_ns != 0 ? nsToMs(now - publish_ns) : 0.0);
    if (frame_capture_ns == 0)
    {
        sh.counters.stale_consumes++;
        return now;
    }
    sh.stages[kTotal].push(nsToMs(now - frame_capture_ns));
    sh.counters.frames_consumed++;
    return now;
}

inline void markMoveSent(int64_t frame_capture_ns, int64_t aim_ns, int64_t sent_ns = 0)
{
    if (!sent_ns) sent_ns = nowNs();
    if (frame_capture_ns <= 0 || aim_ns < frame_capture_ns || sent_ns < aim_ns) return;
    auto& sh = shared();
    std::lock_guard<std::mutex> lk(sh.mu);
    if (frame_capture_ns < sh.reset_ns) return;
    sh.stages[kAimToMove].push(nsToMs(sent_ns - aim_ns));
    sh.stages[kEndToEnd].push(nsToMs(sent_ns - frame_capture_ns));
}

struct Snapshot
{
    bool     enabled             = true;
    uint64_t frames_consumed     = 0;
    uint64_t detections_seen     = 0;
    uint64_t capture_frames      = 0;
    uint64_t dropped_capture     = 0;
    uint64_t stale_consumes      = 0;
    int      device_frame_age_us = -1;
    double   capture_fps         = 0.0;
    double   capture_interval_ms = 0.0;
    Stage    stages[kStageCount];
    Stage    gpu_preprocess;
    Stage    gpu_engine;
    Stage    gpu_copy;
    Stage    gpu_pipeline;
    Stage    cpu_postprocess;
    Stage    aim_tick;
    bool     last_timing_was_graph = false;
    double   engine_inference_ms = -1.0;

    double   sync_wait_ms        = -1.0;
    bool     sync_spun           = false;
    uint64_t sync_fallbacks      = 0;
};

inline void noteEngineInferenceMs(double ms)
{
    auto& sh = shared();
    std::lock_guard<std::mutex> lk(sh.mu);
    sh.engine_inference_ms = ms;
    sh.engine_update_ns = nowNs();
}

inline void notePipelineTimes(bool graph, double preprocessMs, double inferenceMs, double copyMs,
                              double postprocessMs, double aimTickMs)
{
    auto& sh = shared();
    std::lock_guard<std::mutex> lk(sh.mu);
    const double totalGpuMs = preprocessMs + inferenceMs + copyMs;
    if (!graph)
    {
        sh.gpu_preprocess.push(preprocessMs);
        sh.gpu_engine.push(inferenceMs);
        sh.gpu_copy.push(copyMs);
    }
    sh.gpu_pipeline.push(totalGpuMs);
    sh.cpu_postprocess.push(postprocessMs);
    if (aimTickMs >= 0.0) sh.aim_tick.push(aimTickMs);
    sh.last_timing_was_graph = graph;
    sh.engine_inference_ms = totalGpuMs;
    sh.engine_update_ns = nowNs();
}

inline void noteAimTick(double ms)
{
    auto& sh = shared();
    std::lock_guard<std::mutex> lk(sh.mu);
    sh.aim_tick.push(ms);
}

inline void noteSyncWait(double ms, bool spun, uint64_t fallbacks)
{
    auto& sh = shared();
    std::lock_guard<std::mutex> lk(sh.mu);
    sh.sync_wait_ms   = ms;
    sh.sync_spun      = spun;
    sh.sync_fallbacks = fallbacks;
}

inline Snapshot snapshot()
{
    auto& sh = shared();
    Snapshot out;
    std::lock_guard<std::mutex> lk(sh.mu);
    out.enabled             = sh.enabled;
    out.engine_inference_ms = nowNs() - sh.engine_update_ns <= 2'000'000'000
        ? sh.engine_inference_ms : -1.0;
    out.sync_wait_ms        = sh.sync_wait_ms;
    out.sync_spun           = sh.sync_spun;
    out.sync_fallbacks      = sh.sync_fallbacks;
    out.frames_consumed     = sh.counters.frames_consumed;
    out.detections_seen     = sh.counters.detections_seen;
    out.capture_frames      = sh.counters.capture_frames;
    out.dropped_capture     = sh.counters.dropped_capture;
    out.stale_consumes      = sh.counters.stale_consumes;
    out.device_frame_age_us = loadDeviceFrameAgeUs();
    out.capture_interval_ms = sh.capture_interval_ms;
    out.capture_fps = (sh.capture_interval_ms > 0.0)
                          ? 1000.0 / sh.capture_interval_ms
                          : 0.0;
    for (int i = 0; i < kStageCount; ++i)
        out.stages[i] = sh.stages[i];
    out.gpu_preprocess = sh.gpu_preprocess;
    out.gpu_engine = sh.gpu_engine;
    out.gpu_copy = sh.gpu_copy;
    out.gpu_pipeline = sh.gpu_pipeline;
    out.cpu_postprocess = sh.cpu_postprocess;
    out.aim_tick = sh.aim_tick;
    out.last_timing_was_graph = sh.last_timing_was_graph;
    return out;
}

// 在途补偿用的"自动累加延迟"（毫秒）：
//   采集等待(kCaptureWait) + 推理(kInference) + 发布到瞄准(kPublishToAim)
//   + 瞄准到下发(kAimToMove)
//
// 全部取【平滑后的平均值】(EMA)，不取瞬时值 —— 瞬时值会让提前量每帧乱跳。
// 只累加已经产生过样本的段；一段样本都没有时返回 -1，表示"本拍没测到"，
// 由预测器决定是沿用上一帧还是本拍不预测（不会拿 0 或负数去顶）。
inline double autoLeadLatencyMs()
{
    auto& sh = shared();
    std::lock_guard<std::mutex> lk(sh.mu);
    if (!sh.enabled)
        return -1.0;

    double sum = 0.0;
    bool any = false;
    for (int id : { kCaptureWait, kInference, kPublishToAim, kAimToMove })
    {
        if (sh.stages[id].n > 0)
        {
            sum += sh.stages[id].ema_ms;
            any = true;
        }
    }
    return any ? sum : -1.0;
}

inline void reset()
{    auto& sh = shared();
    std::lock_guard<std::mutex> lk(sh.mu);
    for (int i = 0; i < kStageCount; ++i)
        sh.stages[i].reset();
    sh.gpu_preprocess.reset();
    sh.gpu_engine.reset();
    sh.gpu_copy.reset();
    sh.gpu_pipeline.reset();
    sh.cpu_postprocess.reset();
    sh.aim_tick.reset();
    sh.last_timing_was_graph = false;
    sh.counters = Counters{};
    sh.last_capture_seq_seen = loadCaptureSeq();
    sh.last_capture_ns_seen  = 0;
    sh.capture_interval_ms   = 0.0;
    sh.prev_capture_ns       = 0;
    sh.engine_inference_ms   = -1.0;
    sh.engine_update_ns      = 0;
    sh.reset_ns              = nowNs();
    noteDeviceFrameAgeUs(-1);
}

inline void setEnabled(bool on)
{
    auto& sh = shared();
    std::lock_guard<std::mutex> lk(sh.mu);
    sh.enabled = on;
}

inline bool enabled()
{
    auto& sh = shared();
    std::lock_guard<std::mutex> lk(sh.mu);
    return sh.enabled;
}

inline std::string formatLines(bool detail)
{
    const Snapshot s = snapshot();
    char buf[192];
    std::string out;

    auto add = [&](const char* fmt, auto... args) {
        std::snprintf(buf, sizeof(buf), fmt, args...);
        out += buf;
        out += '\n';
    };
    auto addLine = [&](const char* text) {
        out += text;
        out += '\n';
    };

    if (!s.enabled)
        return u8"延迟探针: 已关闭\n";

    if (s.frames_consumed == 0)
        return u8"延迟探针: 等待数据...\n";

    add(u8"采集 %.1f fps (%.2f ms/帧)", s.capture_fps, s.capture_interval_ms);
    add(u8"总延迟 %.1f ms  (均 %.1f / 峰 %.1f)",
        s.stages[kTotal].last_ms, s.stages[kTotal].ema_ms,
        s.stages[kTotal].max_ms);

    if (detail)
    {
        add(u8"  采集->取帧  %5.1f ms", s.stages[kCaptureWait].last_ms);
        add(u8"  推理        %5.1f ms", s.stages[kInference].last_ms);
        add(u8"  发布->消费  %5.1f ms", s.stages[kPublishToAim].last_ms);
        add(u8"  消费->写出  %5.1f ms", s.stages[kAimToMove].last_ms);
        if (s.stages[kEndToEnd].n > 0)
            add(u8"  全链路      %5.1f ms", s.stages[kEndToEnd].last_ms);
        else
            addLine(u8"  全链路      -- (暂无成功发送样本)");
        if (s.device_frame_age_us >= 0)
            add(u8"  设备侧帧龄  %5.1f ms  (驱动/MF 内部)", s.device_frame_age_us / 1000.0);
        else
            addLine(u8"  设备侧帧龄  --      (时间戳缺失、无效或过期)");
        if (s.dropped_capture > 0)
            add(u8"  !! detector 跟不上, 已丢 %llu 帧",
                static_cast<unsigned long long>(s.dropped_capture));
        addLine(u8"  (以上均不含采集卡芯片内部 HDMI->USB, 当前探针未覆盖)");
    }
    else
    {
        add(u8"  推理 %.1f | 消费 %.1f | 写出 %.1f",
            s.stages[kInference].last_ms, s.stages[kPublishToAim].last_ms,
            s.stages[kAimToMove].last_ms);
    }

    return out;
}

inline std::vector<std::string> formatLinesAscii(bool detail)
{
    const Snapshot s = snapshot();
    std::vector<std::string> out;
    char buf[192];

    if (!s.enabled) { out.push_back("latency probe: OFF"); return out; }
    if (s.frames_consumed == 0) { out.push_back("latency probe: waiting..."); return out; }

    std::snprintf(buf, sizeof(buf), "E2E %.1f ms  (avg %.1f / pk %.1f)",
                  s.stages[kEndToEnd].n > 0 ? s.stages[kEndToEnd].last_ms : -1.0, s.stages[kEndToEnd].ema_ms,
                  s.stages[kEndToEnd].max_ms);
    out.push_back(s.stages[kEndToEnd].n > 0 ? buf : "E2E n/a (no successful send)");

    std::snprintf(buf, sizeof(buf), "  cap->det %5.1f  infer %5.1f",
                  s.stages[kCaptureWait].last_ms, s.stages[kInference].last_ms);
    out.push_back(buf);

    std::snprintf(buf, sizeof(buf), "  pub->aim %5.1f  aim->mv %5.1f",
                  s.stages[kPublishToAim].last_ms, s.stages[kAimToMove].last_ms);
    out.push_back(buf);

    std::snprintf(buf, sizeof(buf), "  T0->T3   %5.1f   [src %.0f fps]",
                  s.stages[kTotal].last_ms, s.capture_fps);
    out.push_back(buf);

    if (detail)
    {
        std::snprintf(buf, sizeof(buf), "  frames %llu  dropped %llu",
                      static_cast<unsigned long long>(s.frames_consumed),
                      static_cast<unsigned long long>(s.dropped_capture));
        out.push_back(buf);
        if (s.device_frame_age_us >= 0)
        {
            std::snprintf(buf, sizeof(buf), "  devq %.1f ms (driver/MF, pre-hand-off)",
                          s.device_frame_age_us / 1000.0);
            out.push_back(buf);
        }
        else
        {
            out.push_back("  devq n/a (timestamp unavailable/stale)");
        }
        out.push_back("  (excl. card-internal HDMI->USB)");
    }
    return out;
}

struct FileLogConfig
{
    std::string directory   = "logs";
    std::string basename    = "latency";
    int         interval_ms = 1000;
    double      spike_ms    = 25.0;
    bool        flush_each  = true;
};

namespace detail
{

struct FileLogState
{
    std::mutex        mu;
    std::thread       worker;
    std::atomic<bool> running{false};
    std::string       path;
    std::string       error;
    FileLogConfig     cfg;
};

inline FileLogState& fileLogState()
{
    static FileLogState* s = new FileLogState();
    return *s;
}

inline std::string pathToUtf8(const std::filesystem::path& p)
{
    const auto u8 = p.u8string();
    return std::string(u8.begin(), u8.end());
}

inline std::string timestampNow()
{
    const auto now = std::chrono::system_clock::now();
    const std::time_t t = std::chrono::system_clock::to_time_t(now);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        now.time_since_epoch()).count() % 1000;
    std::tm tm{};
#ifdef _MSC_VER
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d.%03d",
                  tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                  tm.tm_hour, tm.tm_min, tm.tm_sec, static_cast<int>(ms));
    return buf;
}

inline std::string summaryLine()
{
    const Snapshot s = snapshot();
    char buf[520];
    std::snprintf(
        buf, sizeof(buf),
        "E2E=%.2f avg=%.2f pk=%.2f | cap2det=%.2f infer=%.2f pub2aim=%.2f "
        "aim2mv=%.2f total=%.2f | devq=%.2f | src=%.1ffps frames=%llu seen=%llu "
        "dropped=%llu stale=%llu | gpu=%.3f%s post=%.3f tick=%.3f max=%.3f "
        "| sync=%.2f%s fb=%llu",
        s.stages[kEndToEnd].n > 0 ? s.stages[kEndToEnd].last_ms : -1.0, s.stages[kEndToEnd].ema_ms,
        s.stages[kEndToEnd].max_ms,
        s.stages[kCaptureWait].ema_ms, s.stages[kInference].ema_ms,
        s.stages[kPublishToAim].ema_ms, s.stages[kAimToMove].ema_ms,
        s.stages[kTotal].ema_ms,
        s.device_frame_age_us >= 0 ? s.device_frame_age_us / 1000.0 : -1.0,
        s.capture_fps, static_cast<unsigned long long>(s.frames_consumed),
        static_cast<unsigned long long>(s.detections_seen),
        static_cast<unsigned long long>(s.dropped_capture),
        static_cast<unsigned long long>(s.stale_consumes),
        s.gpu_pipeline.ema_ms, s.last_timing_was_graph ? "(graph)" : "(direct)",
        s.cpu_postprocess.ema_ms, s.aim_tick.ema_ms, s.aim_tick.max_ms,
        s.sync_wait_ms,
        s.sync_spun ? "spin" : "block",
        static_cast<unsigned long long>(s.sync_fallbacks));
    return buf;
}

inline void logLine(const std::string& text)
{
    auto& st = fileLogState();
    if (st.path.empty()) return;
    std::ofstream f(st.path, std::ios::out | std::ios::app);
    if (!f) return;
    f << timestampNow() << " | " << text << '\n';
    if (st.cfg.flush_each) f.flush();
}

inline void logWorker(FileLogConfig cfg)
{
    auto& st = fileLogState();

    double   last_max[kStageCount] = {0};
    uint64_t last_dropped = 0;
    uint64_t last_stale   = 0;
    uint64_t last_frames  = 0;
    uint64_t last_seen    = 0;

    const int step_ms = 50;
    int elapsed = 0;

    while (st.running.load())
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(step_ms));
        elapsed += step_ms;

        const Snapshot s = snapshot();

        if (s.dropped_capture > last_dropped)
        {
            char buf[200];
            std::snprintf(buf, sizeof(buf),
                          "EVENT dropped_detector +%llu (total %llu, capture %llu) "
                          u8"-> detector 跟不上, 中间帧被覆盖丢弃",
                          static_cast<unsigned long long>(s.dropped_capture - last_dropped),
                          static_cast<unsigned long long>(s.dropped_capture),
                          static_cast<unsigned long long>(s.capture_frames));
            logLine(buf);
        }
        last_dropped = s.dropped_capture;
        if (s.stale_consumes > last_stale)
        {
            char buf[200];
            std::snprintf(buf, sizeof(buf),
                          "EVENT stale_consume +%llu (total %llu) "
                          u8"-> 控制环读到了无采集戳的数据",
                          static_cast<unsigned long long>(s.stale_consumes - last_stale),
                          static_cast<unsigned long long>(s.stale_consumes));
            logLine(buf);
        }
        last_stale = s.stale_consumes;

        if (cfg.spike_ms > 0.0 && s.stages[kTotal].max_ms > last_max[kTotal] &&
            s.stages[kTotal].max_ms >= cfg.spike_ms)
        {
            char buf[256];
            std::snprintf(buf, sizeof(buf),
                          "SPIKE total=%.2f (avg %.2f) cap2det=%.2f infer=%.2f "
                          "pub2aim=%.2f aim2mv=%.2f",
                          s.stages[kTotal].max_ms, s.stages[kTotal].ema_ms,
                          s.stages[kCaptureWait].ema_ms, s.stages[kInference].ema_ms,
                          s.stages[kPublishToAim].ema_ms, s.stages[kAimToMove].ema_ms);
            logLine(buf);
        }

        if (elapsed >= cfg.interval_ms)
        {
            elapsed = 0;
            if (s.frames_consumed != last_frames)
            {
                logLine(summaryLine());
                last_frames = s.frames_consumed;
            }
            else if (s.frames_consumed > 0)
            {
                char buf[224];
                const uint64_t seen_delta = s.detections_seen - last_seen;
                std::snprintf(
                    buf, sizeof(buf),
                    "IDLE consumed=+0 seen=+%llu -> %s",
                    static_cast<unsigned long long>(seen_delta),
                    seen_delta > 0
                        ? u8"检测仍在产帧, 仅是瞄准键未按下 (采集/推理正常)"
                        : u8"采集或推理确实已停更");
                logLine(buf);
            }
            last_seen = s.detections_seen;
        }

        for (int i = 0; i < kStageCount; ++i)
            if (s.stages[i].max_ms > last_max[i]) last_max[i] = s.stages[i].max_ms;
    }

    logLine("=== latency log stop ===");
}

}

inline bool startFileLog(const FileLogConfig& cfg = FileLogConfig{})
{
    auto& st = detail::fileLogState();
    std::lock_guard<std::mutex> lk(st.mu);

    if (st.running.load()) return true;

    std::error_code ec;
    std::filesystem::create_directories(cfg.directory, ec);

    std::string stamp = detail::timestampNow();
    for (char& c : stamp)
        if (c == ' ' || c == ':') c = '-';
    if (stamp.size() > 19) stamp.resize(19);

    std::filesystem::path p = std::filesystem::path(cfg.directory)
                            / (cfg.basename + "_" + stamp + ".log");

    std::ofstream probe(p, std::ios::out | std::ios::trunc);
    if (!probe)
    {
        st.error = "cannot write " + detail::pathToUtf8(p);
        return false;
    }
    probe << "# Apotheosis end-to-end latency log" << '\n'
          << "# T0=MF sample callback entry T1=detector dequeues frame "
             "T2=inference published T3=aim loop consumes T4=move sent" << '\n'
          << "# total = T3-T0 (software measurement age after MF callback)" << '\n'
          << "# e2e   = T4-T0" << '\n'
          << "# stages: cap2det=T1-T0  infer=T2-T1  pub2aim=T3-T2  aim2mv=T4-T3" << '\n'
          << u8"# devq = 设备侧帧龄: 驱动/MF 把帧交给我们之前花掉的时间. -1 = 驱动未提供"
             u8"/有效/新鲜时间戳. 不用于单独判定卡芯片快慢" << '\n'
          << "# unit: ms. EXCLUDES the capture card's internal HDMI->USB pipeline "
             "(not measurable in software), so this is a LOWER BOUND." << '\n'
          << "# line kinds: periodic summary | EVENT drop/stale | SPIKE | IDLE" << '\n'
          << u8"# frames=aim loop 消费过的批数  seen=aim loop 看到的新检测批数(含未消费)" << '\n'
          << u8"# seen 涨而 frames 不涨 = 瞄准键未按下(正常); 两者都不涨 = 采集/推理停更" << '\n'
          << "# started: " << detail::timestampNow() << '\n';
    probe.flush();
    probe.close();

    st.path = detail::pathToUtf8(std::filesystem::absolute(p, ec));
    st.cfg  = cfg;
    st.error.clear();
    st.running.store(true);
    st.worker = std::thread(detail::logWorker, cfg);

    detail::logLine("=== latency log start ===");
    return true;
}

inline void stopFileLog()
{
    auto& st = detail::fileLogState();
    std::thread toJoin;
    {
        std::lock_guard<std::mutex> lk(st.mu);
        if (!st.running.load()) return;
        st.running.store(false);
        toJoin = std::move(st.worker);
    }
    if (toJoin.joinable()) toJoin.join();
}

inline bool fileLogActive() { return detail::fileLogState().running.load(); }

inline std::string fileLogPath()
{
    auto& st = detail::fileLogState();
    std::lock_guard<std::mutex> lk(st.mu);
    return st.path;
}

inline std::string fileLogError()
{
    auto& st = detail::fileLogState();
    std::lock_guard<std::mutex> lk(st.mu);
    return st.error;
}

}
}
