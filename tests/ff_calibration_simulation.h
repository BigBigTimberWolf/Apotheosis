#pragma once
#include "runtime/ff_calibration_session.h"

// Existing detections of a fixed target, with independent sensitivity, delayed
// scene response and success acknowledgements later than queue submission.
inline runtime::FfCalibrationSession::Snapshot simulateFfCalibration(
    int hotkey, int64_t frameIntervalUs = 8000, double delayMs = 37.0, double noisePixels = .08)
{
    auto& session = runtime::FfCalibrationSession::instance();
    const auto start = runtime::ffCalibrationNowUs() + 1000;
    std::vector<control::FfCalibrationMove> history, feedback;
    for (int frame=0; frame<1500; ++frame) {
        const auto now = start + frame * frameIntervalUs;
        const auto imageTime = now - 16000;
        control::Vec2 cumulative;
        for (const auto& e : history)
            if (e.timeUs <= imageTime-int64_t(delayMs*1000))
                cumulative += control::Vec2{double(e.counts.x),double(e.counts.y)};
        const double noise=noisePixels*std::sin(frame*.73);
        const control::Vec2 center{320-1.35*cumulative.x+noise,320-.72*cumulative.y-noise};
        const std::vector<control::Candidate> candidates{{{center.x-20,center.y-30,40,60},0,.9}};
        const auto move=session.update(hotkey,now,imageTime,candidates,feedback,0);
        feedback.clear();
        if(move.x||move.y) {
            const control::FfCalibrationMove e{now+500,move};
            history.push_back(e); feedback.push_back(e);
        }
        const auto snapshot=session.snapshot();
        if(snapshot.state==runtime::FfCalibrationSession::State::Ready ||
           snapshot.state==runtime::FfCalibrationSession::State::Failed) return session.snapshot(true);
    }
    return session.snapshot(true);
}
