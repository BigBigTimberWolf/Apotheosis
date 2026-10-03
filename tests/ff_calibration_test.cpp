#include "ff_calibration_simulation.h"
#include <cstdio>

namespace {
int failures=0;
void check(bool okay,const char* message) {
    if(!okay) { std::printf("FAIL: %s\n",message);++failures; }
}
}
int main() {
    using namespace control;
    using State=runtime::FfCalibrationSession::State;
    auto& session=runtime::FfCalibrationSession::instance();
    for(const int interval:{6944,16667}) {
        session.cancel(); check(session.arm(3,2),"session arms the requested profile and bank");
        auto data=simulateFfCalibration(3,interval);
        check(data.state==State::Ready && data.moves.size()==16,"all pulses require successful feedback");
        const auto result=fitFfCalibration(data.observations,data.moves);
        std::printf("calibration %d us: valid=%d X=%.4f Y=%.4f lag=%.1f ms RMS=(%.3f,%.3f)\n",
            interval,result.valid,result.pixelsPerCount.x,result.pixelsPerCount.y,result.delayMs,result.rmse.x,result.rmse.y);
        check(result.valid && std::abs(result.pixelsPerCount.x-1.35)<.02 &&
              std::abs(result.pixelsPerCount.y-.72)<.02,"independent XY sensitivities recovered from noisy detections");
        check(std::abs(result.delayMs-37)<double(interval)*.001+1,"response delay recovered within capture resolution");
        check(session.active(),"ready session still blocks normal aim until the hotkey is released");
        session.released();check(!session.active() && session.snapshot().state==State::Ready,"release preserves completed results");
        auto drift=data.observations;
        for(auto& o:drift) o.center.x+=(o.timeUs-drift.front().timeUs)*.00001;
        check(!fitFfCalibration(drift,data.moves).valid,"moving target or camera drift is rejected");
        auto wrongDirection=data.observations;
        for(auto& o:wrongDirection) o.center.x=640-o.center.x;
        check(!fitFfCalibration(wrongDirection,data.moves).valid,"wrong response sign is rejected");
        auto missing=data.moves; missing.pop_back();
        check(!fitFfCalibration(data.observations,missing).valid,"missing successful sends cannot produce a valid fit");
        check(!fitFfCalibration({},data.moves).valid,"insufficient samples are rejected");
    }
    auto begin=[&] {
        session.cancel();session.arm(3,0);
        const auto now=runtime::ffCalibrationNowUs()+1000;
        session.update(3,now,now-500,{Candidate{{300,300,40,40},0,.9}},{},0);
        return now;
    };
    session.cancel();session.arm(3,0);
    const auto noisy=simulateFfCalibration(3,8000,37,1.0);
    const auto noiseFit=fitFfCalibration(noisy.observations,noisy.moves);
    check(noisy.state==State::Ready && noiseFit.valid &&
          std::abs(noiseFit.pixelsPerCount.x-1.35)<.03,
          "ordinary one-pixel detector jitter does not abort calibration or bias the gain");
    session.cancel();
    auto now=begin();session.released();check(session.snapshot().state==State::Failed,"release during sampling aborts");
    now=begin();session.update(4,now+10000,now+9000,{Candidate{{300,300,40,40},0,.9}},{},0);
    check(session.snapshot().state==State::Failed,"profile switching aborts");
    now=begin();session.update(3,now+10000,now+9000,{}, {},0);
    check(session.snapshot().state==State::Failed,"target loss aborts");
    now=begin();session.update(3,now+10000,now+9000,{Candidate{{300,300,40,40},0,.9}},{},1);
    check(session.snapshot().state==State::Failed,"driver failure aborts");
    now=begin();session.cancel();
    bool sentAfterCancel=false;
    check(!session.dispatch({8,0},[&](Counts){sentAfterCancel=true;}) && !sentAfterCancel,
          "cancel between computation and dispatch cannot enqueue a late probe");
    check(session.update(3,now+400000,now+399000,{Candidate{{300,300,40,40},0,.9}},{},0).x==0,
          "cancelled session emits no further probe movement");
    return failures?1:0;
}
