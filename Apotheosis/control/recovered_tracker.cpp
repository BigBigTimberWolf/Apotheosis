#include "recovered_tracker.h"

#include <algorithm>
#include <cmath>

namespace control {
namespace {

bool usable(const Candidate& candidate)
{
    const Box& b = candidate.box;
    return b.w > 0.0 && b.h > 0.0 && std::isfinite(b.x) && std::isfinite(b.y) &&
           std::isfinite(b.w) && std::isfinite(b.h) &&
           std::isfinite(candidate.confidence);
}

double iou(const Box& a, const Box& b)
{
    const double left = std::max(a.x, b.x);
    const double top = std::max(a.y, b.y);
    const double right = std::min(a.x + a.w, b.x + b.w);
    const double bottom = std::min(a.y + a.h, b.y + b.h);
    const double intersection = std::max(0.0, right - left) *
                                std::max(0.0, bottom - top);
    const double unionArea = a.area() + b.area() - intersection;
    return unionArea > 0.0 ? intersection / unionArea : 0.0;
}

double matchScore(const RecoveredTrack& track, const Candidate& candidate,
                  const RecoveredTrackerConfig& config)
{
    if (track.classId != candidate.classId) return 0.0;
    const double radius = std::max(12.0, track.box.diagonal() * config.matchRadiusRatio);
    const Vec2 delta = candidate.box.center() - track.box.center();
    const double distance = delta.norm();
    if (std::abs(delta.x) > radius || std::abs(delta.y) > radius || distance > radius)
        return 0.0;
    const double overlap = iou(track.box, candidate.box);
    const double q = std::clamp(distance / radius, 0.0, 1.0);
    if (overlap < config.minIou && q > 0.45) return 0.0;
    return 0.47 * overlap + 0.35 * (1.0 - q) +
           0.10 * std::clamp(candidate.confidence, 0.0, 1.0) +
           0.08 * track.confidence;
}

double gateAxis(double old, double next, double size, bool xAxis, int& reverseCount)
{
    const double g = xAxis ? std::max(std::max(size, 8.0) * 0.0067, 2.0)
                           : std::max(std::max(size, 10.0) * 0.0107, 2.5);
    const double h = xAxis ? std::max(std::max(size, 8.0) * 0.013, 1.15 * g)
                           : std::max(std::max(size, 10.0) * 0.0175, 1.15 * g);
    const double threshold = std::max(0.004 * std::max(size, 8.0), g);
    const double strong = std::max(1.15 * threshold, h);
    // Normal finite-value defaults of 0x218180. The caller stores each
    // return value as the old value for the next frame.
    constexpr double k = 0.82; // C40=cfg[0x10]=0.3, clamped at 0.82
    if (std::abs(next) <= threshold)
    {
        reverseCount = 0;
        if (std::abs(old) <= 1.18 * threshold) return 0.0;
        const double decayed = k * old;
        return std::abs(decayed) > 0.62 * threshold ? decayed : 0.0;
    }
    if (std::abs(old) <= threshold)
    {
        reverseCount = 0;
        return next;
    }
    if (old * next > 0.0)
    {
        reverseCount = 0;
        const double alpha = std::abs(next) >= std::abs(old) ? 0.5192 : 0.68;
        return old + alpha * (next - old);
    }
    if (std::abs(next) >= strong)
    {
        const int required = xAxis ? 2 : 3;
        reverseCount = std::min(required, reverseCount + 1);
        return reverseCount < required ? 0.0 : next;
    }
    reverseCount = 0;
    if (std::abs(old) <= 1.18 * threshold) return 0.0;
    const double decayed = 0.74 * old;
    return std::abs(decayed) > 0.72 * threshold ? decayed : 0.0;
}

void updateMatched(RecoveredTracker::State& state, const Candidate& candidate,
                   double dtSec, const RecoveredTrackerConfig& config)
{
    const Box old = state.track.box;
    const Box& incoming = candidate.box;
    const double sizeRatio = std::max({ incoming.w / old.w, old.w / incoming.w,
                                        incoming.h / old.h, old.h / incoming.h });
    const double sizeDelta = std::clamp((sizeRatio - 1.0) / 0.65, 0.0, 1.0);
    const double distance = (incoming.center() - old.center()).norm();
    const double distanceDelta = std::clamp(
        distance / std::max(0.85 * std::max(old.diagonal(), 12.0), 18.0), 0.0, 1.0);
    const double base = std::clamp(1.0 - config.qualityAttenuation *
        (0.38 * sizeDelta + 0.62 * distanceDelta), 0.22, 1.0);
    const double positionAlpha = std::max(0.1, base * config.positionAlpha);
    const double sizeAlpha = std::max(0.04, base * config.sizeAlpha);
    const Vec2 center = old.center() + (incoming.center() - old.center()) * positionAlpha;
    const double width = old.w + (incoming.w - old.w) * sizeAlpha;
    const double height = old.h + (incoming.h - old.h) * sizeAlpha;
    state.track.box = { center.x - width * 0.5, center.y - height * 0.5, width, height };

    // Image-space target velocity supplies controller FF. Per user choice,
    // mouse-count conversion and the source's fixed 0.91 are not reintroduced.
    const double velocityDt = std::clamp(dtSec, 0.0020833334, 0.12);
    const Vec2 rawVelocity = (incoming.center() - state.lastObservation) /
                             velocityDt;
    const double firstAlpha = base * 0.3;
    state.firstVelocity += (rawVelocity - state.firstVelocity) * firstAlpha;
    const double outputAlpha = std::clamp(std::max(base, 0.55) * 0.24, 0.12, 0.65);
    const Vec2 next = state.track.velocity +
        (rawVelocity - state.track.velocity) * outputAlpha;
    state.track.velocity = {
        gateAxis(state.track.velocity.x, next.x, incoming.w, true, state.reverseX),
        gateAxis(state.track.velocity.y, next.y, incoming.h, false, state.reverseY)
    };
    if (state.track.velocity.norm() <
        std::max(config.frameMinDimension * 0.0096, 2.5))
        state.track.velocity = {};
    state.lastObservation = incoming.center();
    state.track.observedCenter = state.lastObservation;
    state.track.confidence = candidate.confidence;
    state.track.missedFrames = 0;
}

} // namespace

std::vector<RecoveredTrack> RecoveredTracker::update(
    const std::vector<Candidate>& candidates, double dtSec)
{
    struct Pair { double score; size_t track; size_t candidate; };
    std::vector<Pair> pairs;
    for (size_t ti = 0; ti < tracks_.size(); ++ti)
        for (size_t ci = 0; ci < candidates.size(); ++ci)
            if (usable(candidates[ci]))
            {
                const double score = matchScore(tracks_[ti].track, candidates[ci], config_);
                if (score > 0.0) pairs.push_back({ score, ti, ci });
            }
    std::sort(pairs.begin(), pairs.end(), [](const Pair& a, const Pair& b) {
        if (a.score != b.score) return a.score > b.score;
        if (a.track != b.track) return a.track < b.track;
        return a.candidate < b.candidate;
    });

    std::vector<int> assignment(candidates.size(), -1);
    std::vector<bool> occupied(tracks_.size(), false);
    for (const Pair& pair : pairs)
        if (!occupied[pair.track] && assignment[pair.candidate] < 0)
        {
            occupied[pair.track] = true;
            assignment[pair.candidate] = static_cast<int>(pair.track);
        }

    for (size_t ci = 0; ci < candidates.size(); ++ci)
    {
        if (!usable(candidates[ci])) continue;
        if (assignment[ci] >= 0)
            updateMatched(tracks_[assignment[ci]], candidates[ci], dtSec,
                          config_);
        else
        {
            State state;
            state.track.id = nextId_++;
            state.track.box = candidates[ci].box;
            state.track.classId = candidates[ci].classId;
            state.track.confidence = candidates[ci].confidence;
            state.lastObservation = candidates[ci].box.center();
            state.track.observedCenter = state.lastObservation;
            assignment[ci] = static_cast<int>(tracks_.size());
            tracks_.push_back(state);
        }
    }

    std::vector<RecoveredTrack> visible;
    visible.reserve(candidates.size());
    for (int index : assignment)
        if (index >= 0)
            visible.push_back(tracks_[static_cast<size_t>(index)].track);
    for (size_t ti = 0; ti < occupied.size(); ++ti)
        if (!occupied[ti])
        {
            ++tracks_[ti].track.missedFrames;
            tracks_[ti].track.velocity.x *= 0.74;
            tracks_[ti].track.velocity.y *= 0.68;
            tracks_[ti].firstVelocity = tracks_[ti].firstVelocity * 0.72;
        }
    std::erase_if(tracks_, [this](const State& state) {
        return state.track.missedFrames > config_.maxMissedFrames;
    });
    return visible;
}

void RecoveredTracker::reset()
{
    tracks_.clear();
    nextId_ = 1;
}

std::vector<RecoveredTrack> RecoveredDualTracker::update(
    const std::vector<Candidate>& candidates, double dtSec)
{
    // Preserve the common candidate order supplied by the detector. Its
    // historical re-ranking is an upstream stage, not part of either tracker.
    std::vector<Candidate> ordered;
    for (const Candidate& candidate : candidates)
        if (usable(candidate)) ordered.push_back(candidate);
    const auto motion = motion_.update(ordered, dtSec);
    if (ordered.empty()) return {};

    // The 0x168 input is pre-merged within the frame. The E8 motion tracker
    // above still sees every common candidate, including the merged rows.
    std::vector<Candidate> shaped;
    for (const Candidate& candidate : ordered)
    {
        bool merged = false;
        for (Candidate& kept : shaped)
            if (kept.classId == candidate.classId &&
                iou(kept.box, candidate.box) >= 0.55)
            {
                if (candidate.confidence > kept.confidence + 0.01)
                    kept = candidate;
                merged = true;
                break;
            }
        if (!merged) shaped.push_back(candidate);
    }

    struct Pair { double score; size_t state; size_t candidate; };
    std::vector<Pair> pairs;
    for (size_t si = 0; si < boxes_.size(); ++si)
        for (size_t ci = 0; ci < shaped.size(); ++ci)
        {
            const auto& old = boxes_[si].track;
            const auto& incoming = shaped[ci];
            if (old.classId != incoming.classId) continue;
            const double radius = std::max(30.0, old.box.diagonal() * 0.8);
            const double distance = (incoming.box.center() - old.box.center()).norm();
            if (distance > radius) continue;
            const double overlap = iou(old.box, incoming.box);
            if (overlap < 0.1 && distance > radius * 0.45) continue;
            pairs.push_back({ 0.6 * overlap + 0.4 * (1.0 - distance / radius), si, ci });
        }
    std::sort(pairs.begin(), pairs.end(), [](const Pair& a, const Pair& b) {
        if (a.score != b.score) return a.score > b.score;
        if (a.state != b.state) return a.state < b.state;
        return a.candidate < b.candidate;
    });
    std::vector<int> assignment(shaped.size(), -1);
    std::vector<bool> occupied(boxes_.size(), false);
    for (const Pair& pair : pairs)
        if (!occupied[pair.state] && assignment[pair.candidate] < 0)
        {
            occupied[pair.state] = true;
            assignment[pair.candidate] = static_cast<int>(pair.state);
        }

    for (size_t ci = 0; ci < shaped.size(); ++ci)
    {
        const Candidate& incoming = shaped[ci];
        if (assignment[ci] < 0)
        {
            BoxState state;
            state.track.id = nextBoxId_++;
            state.track.box = incoming.box;
            state.track.observedCenter = incoming.box.center();
            state.track.classId = incoming.classId;
            state.track.confidence = incoming.confidence;
            assignment[ci] = static_cast<int>(boxes_.size());
            boxes_.push_back(state);
        }
        else
        {
            BoxState& state = boxes_[static_cast<size_t>(assignment[ci])];
            // Normal finite-value 0x168 position correction. Its clock is
            // independent of E8; the caller's dt is the available proxy.
            const double elapsed = std::clamp(dtSec, 0.001, 0.14);
            const double predicted = state.positionVariance +
                100.0 * elapsed * elapsed + 24.5;
            const double measurement = (2.0 - 0.88 * 1.5) * 0.35;
            const double gain = predicted / (predicted + measurement);
            const Box& old = state.track.box;
            state.track.box = {
                old.x + gain * (incoming.box.x - old.x),
                old.y + gain * (incoming.box.y - old.y),
                old.w + gain * (incoming.box.w - old.w),
                old.h + gain * (incoming.box.h - old.h)
            };
            state.positionVariance = (1.0 - gain) * predicted;
            state.track.observedCenter = incoming.box.center();
            state.track.confidence = incoming.confidence;
            state.age++;
            state.missed = 0;
        }
    }
    for (size_t si = 0; si < occupied.size(); ++si)
        if (!occupied[si]) ++boxes_[si].missed;

    std::vector<RecoveredTrack> visible;
    std::vector<bool> used(motion.size(), false);
    for (int index : assignment)
    {
        if (index < 0) continue;
        RecoveredTrack published = boxes_[static_cast<size_t>(index)].track;
        const Vec2 center = published.box.center();
        bool sameIdAvailable = false;
        for (size_t mi = 0; mi < motion.size(); ++mi)
            if (!used[mi] && motion[mi].id == published.id &&
                (center - motion[mi].observedCenter).norm() < 1.0)
                sameIdAvailable = true;
        size_t best = motion.size();
        double bestDistance = 1.0; // strict one-pixel join gate
        for (size_t mi = 0; mi < motion.size(); ++mi)
        {
            if (used[mi] || (sameIdAvailable && motion[mi].id != published.id))
                continue;
            const double distance = (center - motion[mi].observedCenter).norm();
            if (distance < bestDistance)
            {
                bestDistance = distance;
                best = mi;
            }
        }
        if (best < motion.size())
        {
            published.velocity = motion[best].velocity;
            used[best] = true;
        }
        visible.push_back(published);
    }
    std::erase_if(boxes_, [](const BoxState& state) { return state.missed > 3; });
    return visible;
}

void RecoveredDualTracker::reset()
{
    motion_.reset();
    boxes_.clear();
    nextBoxId_ = 1;
}

void RecoveredDualTracker::setFrameSize(int width, int height)
{
    RecoveredTrackerConfig config;
    config.frameMinDimension = std::max(1, std::min(width, height));
    motion_.setConfig(config);
}

} // namespace control
