#pragma once

#include "control/types.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <deque>

namespace runtime {

// Match successful mouse sends to the frame whose observed target motion is
// being differentiated. The source controller keeps a 128-event ring and
// weights small source-0 moves before feeding them back into target velocity.
class MotionFeedbackWindow
{
public:
    struct Event
    {
        int dx = 0;
        int dy = 0;
        int64_t timestampUs = 0;
        int source = 0;
    };

    void add(Event event)
    {
        if (event.timestampUs <= 0 || (event.dx == 0 && event.dy == 0) ||
            event.source < 0 || event.source > 1)
            return;
        events_.push_back(event);
        if (events_.size() > 128) events_.pop_front();
    }

    control::Vec2 sample(int64_t frameUs, bool weighted = true)
    {
        if (frameUs <= 0) return {};
        // A duplicated or older result must not replay an event into the
        // velocity estimate or move the interval clock backwards.
        if (previousFrameUs_ > 0 && frameUs <= previousFrameUs_) return {};
        if (previousFrameUs_ > 0)
        {
            const double interval = static_cast<double>(
                std::clamp(frameUs - previousFrameUs_, int64_t{1000}, int64_t{50000}));
            smoothedIntervalUs_ = smoothedIntervalUs_ > 0.0
                ? smoothedIntervalUs_ + 0.25 * (interval - smoothedIntervalUs_)
                : interval;
        }
        previousFrameUs_ = frameUs;

        const double baseline = std::clamp(
            smoothedIntervalUs_ > 0.0 ? smoothedIntervalUs_ + 1500.0 : 8000.0,
            2000.0, 24000.0);
        const double halfWidth = smoothedIntervalUs_ > 0.0
            ? std::clamp(smoothedIntervalUs_ * 0.5 + 1000.0, 1500.0, 6000.0)
            : 3000.0;
        const double center = std::max(0.0, static_cast<double>(frameUs) - baseline);
        const double lower = std::max(0.0, center - halfWidth);
        const double upper = center + halfWidth;

        control::Vec2 sum;
        for (auto it = events_.rbegin(); it != events_.rend(); ++it)
        {
            if (static_cast<double>(it->timestampUs) > upper) continue;
            if (static_cast<double>(it->timestampUs) < lower) break;
            const double magnitude = std::hypot(static_cast<double>(it->dx),
                                                static_cast<double>(it->dy));
            const double weight = !weighted || it->source == 1 ? 1.0
                : magnitude <= 2.25 ? 0.35 : magnitude <= 4.0 ? 0.65 : 1.0;
            sum.x += weight * it->dx;
            sum.y += weight * it->dy;
        }
        return sum;
    }

    void reset()
    {
        events_.clear();
        previousFrameUs_ = 0;
        smoothedIntervalUs_ = 0.0;
    }

private:
    std::deque<Event> events_;
    int64_t previousFrameUs_ = 0;
    double smoothedIntervalUs_ = 0.0;
};

} // namespace runtime
