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
    };

    void setConfig(const RecoveredTrackerConfig& config) { config_ = config; }
    std::vector<RecoveredTrack> update(const std::vector<Candidate>& candidates,
                                       double dtSec);
    void reset();

private:
    RecoveredTrackerConfig config_{};
    std::vector<State> tracks_;
    int nextId_ = 1;
};

// The source keeps a motion track (0xE8) and a separately filtered box
// record (0x168). Joined motion velocity supplies FF without mouse-event scaling.
class RecoveredDualTracker
{
public:
    std::vector<RecoveredTrack> update(const std::vector<Candidate>& candidates,
                                       double dtSec);
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
