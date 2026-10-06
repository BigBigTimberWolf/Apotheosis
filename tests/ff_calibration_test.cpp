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
        // 诊断字段：没有噪声时静止抖动接近 0，延迟不该贴在扫描上限上。
        check(result.jitterPixels.x < 1.0 && result.jitterPixels.y < 1.0 &&
              result.score > 0.0 && result.inlierRatio > 0.9 && !result.delayAtScanLimit,
              "fit reports the measured jitter, residual score and usable-sample ratio");
    }
    // 现场最常见的“拟合误差过大”其实来自少数被污染的画面：手抖一下、开一枪的
    // 后坐、一帧运动模糊。旧实现是整段最小二乘 + 绝对门槛，几帧就能把一轮判死。
    {
        session.cancel();session.arm(3,0);
        const auto clean=simulateFfCalibration(3,8000,37,.08);
        auto spiked=clean.observations;
        for(size_t i=0;i<spiked.size();i+=12) {
            spiked[i].center.x+=9.0;
            spiked[i].center.y-=7.0;
        }
        const auto spikedFit=fitFfCalibration(spiked,clean.moves);
        std::printf("spiked %.1f%%: valid=%d X=%.4f inliers=%.2f\n",
            100.0/12.0,spikedFit.valid,spikedFit.pixelsPerCount.x,spikedFit.inlierRatio);
        check(spikedFit.valid && std::abs(spikedFit.pixelsPerCount.x-1.35)<.05,
              "a few contaminated frames no longer fail the whole calibration");
    }
    // 抖动大的场景：静止抖动本身就有 3 px 时，旧门槛（rmse ≤ max(0.8, 6%位移)）
    // 必失败；现在门槛跟着实测抖动放大，仍然要求残差相对位移足够小。
    {
        session.cancel();session.arm(3,0);
        const auto jittery=simulateFfCalibration(3,8000,37,3.0);
        const auto jitteryFit=fitFfCalibration(jittery.observations,jittery.moves);
        std::printf("jitter 3px: valid=%d X=%.4f jitter=(%.2f,%.2f) rms=(%.2f,%.2f)\n",
            jitteryFit.valid,jitteryFit.pixelsPerCount.x,jitteryFit.jitterPixels.x,
            jitteryFit.jitterPixels.y,jitteryFit.rmse.x,jitteryFit.rmse.y);
        check(jittery.state==State::Ready && jitteryFit.valid &&
              std::abs(jitteryFit.pixelsPerCount.x-1.35)<.12 &&
              jitteryFit.rmse.x > 1.2,
              "a noisy-but-clean scene fits once the tolerance follows the measured jitter");
    }
    // 但噪声大到跟位移一个量级时必须照样拒绝（不能因为“门槛会放大”就什么都收）。
    {
        session.cancel();session.arm(3,0);
        const auto hopeless=simulateFfCalibration(3,8000,37,12.0);
        const auto hopelessFit=fitFfCalibration(hopeless.observations,hopeless.moves);
        std::printf("jitter 12px: valid=%d score=%.2f rms=(%.2f,%.2f)\n",
            hopelessFit.valid,hopelessFit.score,hopelessFit.rmse.x,hopelessFit.rmse.y);
        check(!hopelessFit.valid && hopelessFit.reason.find("数据不稳定")!=std::string::npos,
              "noise comparable to the probe is still rejected with the measured numbers");
    }
    auto begin=[&] {
        session.cancel();session.arm(3,0);
        const auto now=runtime::ffCalibrationNowUs()+1000;
        session.update(3,now,now-500,{Candidate{{300,300,40,40},0,.9}},{},0);
        return now;
    };
    // 脉冲幅度自适应：低灵敏度（像素/计数 小）和高倍率过去整段失败，报“位移不足”。
    // 现在按配对实测响应决定下一对的幅度：不够就放大，过大就收小，始终让检测框
    // 移动约 20 px，从而与检测抖动区分开。
    struct SensitivityCase { const char* name; double gainX, gainY; int peakAtLeast, peakAtMost; };
    const SensitivityCase sensitivityCases[] = {
        { "普通灵敏度", 1.35, .72, 0, 0 },
        { "低灵敏度",   0.25, .12, 60, 0 },
        { "极低灵敏度", 0.10, .05, 60, 0 },
        { "高灵敏度",   4.00, 2.50, 0, 8 },
    };
    for(const auto& c : sensitivityCases) {
        session.cancel(); check(session.arm(3,0),"adaptive case arms the session");
        const auto data=simulateFfCalibration(3,8000,37,.3,c.gainX,c.gainY);
        const auto fit=fitFfCalibration(data.observations,data.moves);
        const int peak=simulateFfCalibrationPeakCounts(data);
        std::printf("sensitivity %s (%.2f/%.2f): valid=%d X=%.4f Y=%.4f travel=(%.1f,%.1f) peakCounts=%d reason=%s\n",
            c.name,c.gainX,c.gainY,fit.valid,fit.pixelsPerCount.x,fit.pixelsPerCount.y,
            fit.travelPixels.x,fit.travelPixels.y,peak,fit.reason.c_str());
        check(data.state==State::Ready && data.moves.size()==16,"adaptive plan still records every pulse");
        check(fit.valid,"every sensitivity produces a valid fit");
        check(std::abs(fit.pixelsPerCount.x-c.gainX)<c.gainX*.05 &&
              std::abs(fit.pixelsPerCount.y-c.gainY)<c.gainY*.05,
              "adaptive pulse amplitude recovers the gain at any sensitivity");
        check(fit.travelPixels.x>=8.0 && fit.travelPixels.y>=8.0,
              "adaptive pulse amplitude moves the box far enough to beat detector noise");
        if(c.peakAtLeast) check(peak>=c.peakAtLeast,"low sensitivity escalates the probe counts");
        if(c.peakAtMost) check(peak<=c.peakAtMost,"high gain never escalates the probe counts");
    }
    // 位移真的过低时（例如设备没有真正移动视角），失败原因要给出实测位移，
    // 而不是笼统的“数据不稳定”，用户才知道该调灵敏度还是该重试。
    {
        std::vector<FfCalibrationObservation> obs;
        std::vector<FfCalibrationMove> mv;
        const int64_t t0=1000000;
        for(size_t i=0;i<16;++i) mv.push_back({t0+1000000+int64_t(i)*350000,{i%2?-2:2,0}});
        for(int64_t t=0;t<8000000;t+=25000) {
            int64_t cumulative=0;
            for(const auto& m:mv) if(m.timeUs<=t) cumulative+=m.counts.x;
            obs.push_back({t0+t,{320.0-0.02*double(cumulative)+(t%50000?0.2:-0.2),320.0}});
        }
        const auto tiny=fitFfCalibration(obs,mv);
        std::printf("tiny travel: valid=%d travel=(%.2f,%.2f) reason=%s\n",
            tiny.valid,tiny.travelPixels.x,tiny.travelPixels.y,tiny.reason.c_str());
        check(!tiny.valid && tiny.travelPixels.x<8.0,"a scene that barely moves measures a tiny travel");
        check(tiny.reason.find("移动像素过低")!=std::string::npos,
              "a too-small travel is reported with the measured pixels, not as generic instability");
    }
    // 低帧率下每个平台只剩少量帧，幅度测量仍要收敛。
    session.cancel();session.arm(3,0);
    const auto lowFps=simulateFfCalibration(3,66000,37,.3,1.35,.72);
    const auto lowFpsFit=fitFfCalibration(lowFps.observations,lowFps.moves);
    check(lowFps.state==State::Ready && lowFpsFit.valid && lowFpsFit.travelPixels.y>=8.0,
          "15 fps detection still reaches a measurable travel");
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
    now=begin();session.update(3,now+10000,now+9000,{Candidate{{300,300,40,40},0,.9}},{},1);
    check(session.snapshot().state==State::Failed,"driver failure aborts");
    now=begin();session.cancel();
    bool sentAfterCancel=false;
    check(!session.dispatch({8,0},[&](Counts){sentAfterCancel=true;}) && !sentAfterCancel,
          "cancel between computation and dispatch cannot enqueue a late probe");
    check(session.update(3,now+400000,now+399000,{Candidate{{300,300,40,40},0,.9}},{},0).x==0,
          "cancelled session emits no further probe movement");

    // 瞬时故障不再判死整轮标定（登记表 BUG-0001/0004/0005）：单帧丢框、一次采集
    // 卡顿、运动模糊让框抖一下，都只跳过该帧；连续或累计超限才失败。
    {
        now=begin();
        session.update(3,now+10000,now+9000,{}, {},0);
        check(session.snapshot().state==State::Sampling && session.snapshot().skipped==1,
              "a single missed frame is skipped and counted, not fatal");
        for(int i=0;i<4;++i)
            session.update(3,now+20000+i*9000,now+19000+i*9000,{}, {},0);
        check(session.snapshot().state==State::Failed,
              "a persistent target loss still aborts");
    }
    {
        now=begin();
        session.update(3,now+300000,now+299000,{Candidate{{300,300,40,40},0,.9}},{},0);
        check(session.snapshot().state==State::Sampling && session.snapshot().skipped==1,
              "a single capture hiccup is skipped, not fatal");
        now=begin();
        session.update(3,now+900000,now+899000,{Candidate{{300,300,40,40},0,.9}},{},0);
        check(session.snapshot().state==State::Failed,"a long capture stall still aborts");
    }
    {
        now=begin();
        session.update(3,now+10000,now+9000,{Candidate{{300,300,80,80},0,.9}},{},0);
        check(session.snapshot().state==State::Sampling && session.snapshot().skipped==1,
              "one motion-blurred box is skipped, not fatal");
        for(int i=0;i<8;++i)
            session.update(3,now+20000+i*9000,now+19000+i*9000,{Candidate{{300,300,80,80},0,.9}},{},0);
        check(session.snapshot().state==State::Failed,
              "a persistent magnification change still aborts");
    }
    // 手还在微调时开始标定：旧实现立刻"开始前目标不稳定"判死；现在先延长基线重测。
    {
        session.cancel();session.arm(3,0);
        int64_t t=runtime::ffCalibrationNowUs()+1000;
        for(int i=0;i<10;++i,t+=8000) {
            const double x=320.0+i*1.2;
            session.update(3,t,t-16000,{Candidate{{x-20,270,40,60},0,.9}},{},0);
        }
        for(int i=0;i<60;++i,t+=8000)
            session.update(3,t,t-16000,{Candidate{{300,290,40,60},0,.9}},{},0);
        check(session.snapshot().state==State::Sampling,
              "an unsettled baseline waits for the scene to settle instead of aborting");
    }
    // 被跳过的帧在录到的观测里就是"洞"：拟合必须照样成立。
    {
        std::vector<FfCalibrationObservation> thinned;
        for(size_t i=0;i<noisy.observations.size();++i)
            if(i%17!=0) thinned.push_back(noisy.observations[i]);
        const auto holedFit=fitFfCalibration(thinned,noisy.moves);
        check(holedFit.valid && std::abs(holedFit.pixelsPerCount.x-1.35)<.03,
              "skipped frames leave holes that the fit still handles");
    }
    return failures?1:0;
}
