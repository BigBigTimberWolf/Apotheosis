#pragma once

#include <algorithm>
#include <cstdint>

namespace boss {

// Tracks uninterrupted time near one target. This never authorizes a shot;
// TriggerFsm still requires the ordinary hit zone and scope readiness.
class TriggerPrearm {
public:
    void update(bool enabled, bool near_zone, int track_id, int64_t now_ms) {
        if (!enabled || !near_zone || track_id < 0) {
            reset();
            return;
        }
        if (track_id_ != track_id || started_ms_ < 0) {
            track_id_ = track_id;
            started_ms_ = now_ms;
        }
    }

    int elapsedMs(int track_id, int64_t now_ms) const {
        if (track_id < 0 || track_id != track_id_ || started_ms_ < 0) return 0;
        return static_cast<int>(std::clamp<int64_t>(now_ms - started_ms_, 0, 2000));
    }

    void reset() {
        track_id_ = -1;
        started_ms_ = -1;
    }

private:
    int track_id_ = -1;
    int64_t started_ms_ = -1;
};

} // namespace boss
