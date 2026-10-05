#include "runtime/motion_feedback_window.h"

#include <cmath>
#include <cstdio>

namespace {
int failures = 0;

void check(double actual, double expected, const char* message)
{
    if (std::abs(actual - expected) > 1e-9)
    {
        std::printf("FAIL: %s: got %.6f, expected %.6f\n",
                    message, actual, expected);
        ++failures;
    }
}
}

int main()
{
    runtime::MotionFeedbackWindow window;
    const auto first = window.sample(1'000'000);
    check(first.x, 0.0, "first frame has no feedback");

    window.add({ 2, -1, 1'001'000, 0 });
    window.add({ 5, 2, 1'009'000, 1 });
    const auto second = window.sample(1'010'000);
    check(second.x, 0.7, "small source-0 event is weighted");
    check(second.y, -0.35, "small source-0 Y is weighted");

    const auto duplicate = window.sample(1'010'000);
    check(duplicate.x, 0.0, "duplicate frame does not replay feedback");

    window.add({ 3, 0, 1'011'000, 0 });
    const auto third = window.sample(1'020'000);
    check(third.x, 6.95, "only events in observed frame window are summed");
    check(third.y, 2.0, "source-1 event keeps its full Y weight");

    window.reset();
    const auto afterReset = window.sample(1'030'000);
    check(afterReset.x, 0.0, "reset discards old mouse sends");

    // Measured delay shifts complete frame intervals; each successful event is
    // counted exactly once, including small moves and interval-edge events.
    window.reset();
    window.sample(1'000'000, true, 20);
    window.add({2, 1, 980'000, 0}); // lower boundary, belongs to the previous interval
    window.add({1, 2, 985'000, 0});
    window.add({3, 4, 990'000, 0}); // upper boundary, belongs to this interval
    window.add({5, 6, 991'000, 0});
    const auto aligned = window.sample(1'010'000, true, 20);
    check(aligned.x, 4, "calibrated interval includes small sends without legacy weights");
    check(aligned.y, 6, "calibrated XY use the same observed time interval");
    check(window.sample(1'010'000, true, 20).x, 0, "calibrated duplicate does not replay events");
    check(window.sample(1'020'000, true, 20).x, 5, "next frame does not duplicate upper-boundary sends");
    check(window.sample(1'030'000, true, 20).x, 0, "later frame leaves no feedback tail");
    window.add({7, 0, 1'031'000, 0});
    check(window.sample(1'040'000, true, 0).x, 0, "alignment changes discard the transitional frame interval");
    window.add({8, 0, 1'041'000, 0});
    check(window.sample(1'050'000, true, 0).x, 8, "new alignment starts a fresh frame interval");

    // pendingCorrection sums in-flight sends × conversion.
    {
        runtime::MotionFeedbackWindow w;
        check(w.pendingCorrection(1'000'000, -1, 0.91, 0.91).x, 0,
              "legacy delay returns no pending correction");
        check(w.pendingCorrection(1'000'000, 30, 0.91, 0.91).x, 0,
              "no events returns no pending correction");
        w.add({10, -5, 980'000, 0});
        w.add({4, 2, 990'000, 1});
        w.add({6, 3, 960'000, 0});
        auto p = w.pendingCorrection(1'000'000, 30, 0.91, 0.91);
        check(p.x, (10 + 4) * 0.91, "pending X sums only in-flight sends");
        check(p.y, (-5 + 2) * 0.91, "pending Y sums only in-flight sends");
        auto p2 = w.pendingCorrection(1'000'000, 30, 1.5, 0.8);
        check(p2.x, 14 * 1.5, "pending uses per-axis conversion X");
        check(p2.y, -3 * 0.8, "pending uses per-axis conversion Y");
        auto p3 = w.pendingCorrection(1'020'000, 30, 0.91, 0.91);
        check(p3.x, 0, "later frame: all sends now reflected");
        auto p4 = w.pendingCorrection(1'000'000, 30, 0.91, 0.91);
        check(p4.x, 14 * 0.91, "pendingCorrection is const, repeatable");
    }

    std::puts(failures == 0 ? "motion feedback window OK" : "motion feedback window FAILED");
    return failures == 0 ? 0 : 1;
}
