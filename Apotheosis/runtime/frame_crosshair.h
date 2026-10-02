#pragma once
#include "frame_context.h"
#include <cmath>
#include <optional>

namespace runtime {
// Owned by the inference result, never obtained from the latest preview pivot.
struct FrameCrosshair {
    FrameContext frame{};
    double x = 0, y = 0;
    int hotkey = -1;
    bool valid = false;

    std::optional<FrameCrosshair> forDetection(const FrameContext& detection,
                                              int activeHotkey, int resolution) const
    {
        if (!valid || hotkey != activeHotkey || activeHotkey < 0 || resolution <= 0 ||
            frame.width <= 0 || frame.height <= 0 ||
            frame.sequence != detection.sequence || frame.captured_ns != detection.captured_ns ||
            frame.width != detection.width || frame.height != detection.height ||
            !std::isfinite(x) || !std::isfinite(y) || x < 0 || y < 0 ||
            x >= frame.width || y >= frame.height)
            return std::nullopt;
        // Zero timestamps are allowed: ownership in the detection batch still
        // pairs the result with its image; timestamps are not the pairing mechanism.
        auto result = *this;
        result.x *= double(resolution) / frame.width;
        result.y *= double(resolution) / frame.height;
        return result;
    }
};

// Same-frame observations are authoritative. A miss may borrow the last valid
// observation for at most three capture frames; borrowed points never renew it.
class CrosshairFrameHold {
public:
    enum class Source { Center, CurrentFrame, PreviousFrame };
    struct Result { double x=0,y=0; Source source=Source::Center; };
    void reset() { *this = CrosshairFrameHold{}; }

    Result resolve(const FrameContext& frame, const FrameCrosshair& pivot,
                   int hotkey, int resolution, int algorithm, int batchVersion) {
        const Result center{resolution*0.5,resolution*0.5,Source::Center};
        if(hotkey<0 || resolution<=0 || frame.width<=0 || frame.height<=0) {
            reset(); return center;
        }
        const bool changed = hotkey_!=hotkey || resolution_!=resolution || algorithm_!=algorithm
            || lastFrame_.width!=frame.width || lastFrame_.height!=frame.height;
        const bool backwards = seen_ && ((frame.sequence && lastFrame_.sequence && frame.sequence<lastFrame_.sequence)
            || (frame.captured_ns && lastFrame_.captured_ns && frame.captured_ns<lastFrame_.captured_ns));
        if(changed || backwards) reset();
        const bool identified = frame.sequence!=0 || frame.captured_ns!=0;
        if(seen_ && frame.sequence==lastFrame_.sequence && frame.captured_ns==lastFrame_.captured_ns
            && (identified || batchVersion==lastBatch_)) return result_;

        const auto step = seen_ && frame.sequence>lastFrame_.sequence && lastFrame_.sequence
            ? frame.sequence-lastFrame_.sequence : uint64_t{1};
        hotkey_=hotkey; resolution_=resolution; algorithm_=algorithm;
        lastFrame_=frame; lastBatch_=batchVersion; seen_=true;
        if(const auto hit=pivot.forDetection(frame,hotkey,resolution)) {
            retained_={hit->x,hit->y,Source::PreviousFrame};
            haveRetained_=true; misses_=0;
            result_={hit->x,hit->y,Source::CurrentFrame};
        } else {
            misses_+=static_cast<int>(step>4?4:step);
            if(misses_>3) { misses_=4; haveRetained_=false; }
            result_=haveRetained_?retained_:center;
        }
        return result_;
    }
private:
    FrameContext lastFrame_{};
    int hotkey_=-1,resolution_=0,algorithm_=-1,lastBatch_=0,misses_=0;
    bool seen_=false,haveRetained_=false;
    Result retained_{},result_{};
};
}
