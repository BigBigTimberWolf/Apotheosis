#pragma once

#include "types.h"

#include <vector>

namespace control {

struct RecoveredTrack
{
    int id = -1;
    Box box{};
    Vec2 observedCenter{};
    int classId = -1;
    double confidence = 0.0;
    Vec2 velocity{};
    // Maneuver-aware velocity for aim-point extrapolation only. Converges on the
    // first frame that reveals a reversal, sudden stop, or new target, while a
    // size-scaled noise gate keeps steady tracking smooth. The frozen FF path
    // keeps using `velocity`.
    Vec2 predictVelocity{};
    // Per-axis one-shot flag: nonzero on the frame a reversal/sudden-stop is
    // confirmed. The follow compensator uses it to clear its lead immediately
    // (normal movement does not set it, so crossing the aim point is preserved).
    Vec2 maneuver{};
    int missedFrames = 0;
};

struct RecoveredTrackerConfig
{
    float matchRadiusRatio = 0.55f;
    float minIou = 0.05f;
    float positionAlpha = 0.42f;
    float sizeAlpha = 0.14f;
    float qualityAttenuation = 1.0f;
    int maxMissedFrames = 3;
    int frameMinDimension = 320;
};

class RecoveredTracker
{
public:
    struct State
    {
        RecoveredTrack track;
        Vec2 lastObservation{};
        Vec2 firstVelocity{};
        int reverseX = 0;
        int reverseY = 0;
        // Seed the extrapolation velocity from the first observed displacement
        // instead of easing up from zero.
        bool predictSeeded = false;
    };

    void setConfig(const RecoveredTrackerConfig& config) { config_ = config; }
    std::vector<RecoveredTrack> update(const std::vector<Candidate>& candidates,
                                       double dtSec, Vec2 eventSum = {});
    void reset();

private:
    RecoveredTrackerConfig config_{};
    std::vector<State> tracks_;
    int nextId_ = 1;
};

// The source keeps a motion track (0xE8) and a separately filtered box
// record (0x168). Joined motion velocity supplies the frozen second-port FF.
class RecoveredDualTracker
{
public:
    std::vector<RecoveredTrack> update(const std::vector<Candidate>& candidates,
                                       double dtSec, Vec2 eventSum = {});
    void setFrameSize(int width, int height);
    void reset();

private:
    struct BoxState
    {
        RecoveredTrack track;
        double positionVariance = 10.0;
        int age = 1;
        int missed = 0;
    };

    RecoveredTracker motion_;
    std::vector<BoxState> boxes_;
    int nextBoxId_ = 1;
};

} // namespace control
