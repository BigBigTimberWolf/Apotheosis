#include "control/recovered_aim_controller.h"
#include <cmath>
#include <cstdio>
#include <limits>
using namespace control;
namespace {
int failures = 0;
void check(bool pass, const char* name) {
    if (!pass) { ++failures; std::printf("FAIL: %s\n", name); }
}
}
int main() {
    FollowCompensator c;
    Vec2 offset;
    for (int n=0; n<200; ++n)
        offset=c.update({5,5},{416,416},{10,10},1000000+n*10000,.01);
    check(offset.x>20 && offset.y>20, "persistent error builds both axes");
    const double beforeY=offset.y;
    offset=c.update({-.01,5},{416,416},{10,10},3000000,.01);
    check(offset.x>20 && offset.y>=beforeY,
          "crossing the real aimpoint preserves learned and applied lead");
    // The background-motion immediate-reset path has been removed: an opposite
    // error unwinds the learned lead gradually rather than zeroing it in one
    // frame, and only a long sustained reversal rebuilds lead the other way.
    const double beforeReversal=offset.x;
    offset=c.update({-5,5},{416,416},{10,10},3010000,.01);
    check(offset.x>beforeReversal-1.0,
          "a single opposite frame does not hard-reset the learned lead");
    for (int n=2; n<400; ++n)
        offset=c.update({-5,5},{416,416},{10,10},3000000+n*10000,.01);
    check(offset.x<-5, "a long sustained reversal rebuilds lead the other way");
    offset=c.update({0,5},{416,416},{10,10},7000000,.01);
    check(offset.x<0, "zero error neither reverses nor clears the rebuilt lead");

    c.reset();
    for (int n=0; n<200; ++n) {
        offset=c.update({5,0},{416,416},{10,0},5000000+n*10000,.01);
        c.setSaturation({1,0});
    }
    check(offset.x==0 && offset.y==0, "saturation blocks same-direction accumulation");
    c.reset();
    for (int n=0; n<100; ++n)
        offset=c.update({41,5},{416,416},{10,10},8000000+n*10000,.01);
    check(offset.x>41, "large persistent lag still builds without a box-width cap");
    offset=c.update({41,5},{416,416},{0,10},9000000,.01);
    check(offset.x==0 && offset.y>0, "axis disable clears only that axis");
    offset=c.update({41,5},{416,416},{10,10},10000000,.01);
    check(offset.norm()==0 && c.errorRate().norm()==0, "long gap resets lead and diagnostic error history");
    offset=c.update({41,5},{416,416},{10,10},9990000,.01);
    check(offset.norm()==0 && c.errorRate().norm()==0, "backward timestamps reseed history");
    c.update({std::numeric_limits<double>::quiet_NaN(),0},{416,416},{10,10},10010000,.01);
    check(c.errorRate().norm()==0, "invalid error clears history");

    // Error-rate telemetry is independent of velocity FF and must not count
    // duplicate images even when following compensation is disabled.
    c.reset();
    c.update({20,0},{416,416},{0,0},11000000,.01);
    check(c.errorRate().norm()==0, "first error seeds diagnostic history");
    c.update({21,0},{416,416},{0,0},11010000,.01);
    const double rate=c.errorRate().x;
    check(rate>0, "diagnostic error trend survives disabled compensation");
    for (int n=0; n<10; ++n)
        c.update({-200,0},{416,416},{0,0},11010000,.001);
    check(c.errorRate().x==rate, "duplicate image cannot manufacture an error-rate reversal");
    for (int n=2; n<102; ++n)
        c.update({21,0},{416,416},{0,0},11000000+n*10000,.01);
    check(std::abs(c.errorRate().x)<1e-6, "constant lag has zero diagnostic error rate");

    for (double dt : {.005, .01, .02}) {
        c.reset();
        const int count=int(2.0/dt);
        for (int n=0; n<=count; ++n)
            offset=c.update({4,0},{416,416},{10,0},20000000+int64_t(n*dt*1e6),dt);
        check(offset.x>18 && offset.x<21, "error integration scales with time across capture rates");
        c.reset();
        for (int n=0; n<100; ++n)
            offset=c.update({n%2 ? 1.0:-1.0,0},{416,416},{10,0},
                            24000000+int64_t(n*dt*1e6),dt);
        check(offset.x==0, "alternating sign noise cannot build stored compensation");
    }

    // Velocity FF must not manufacture target motion from crosshair movement.
    {
        ControllerConfig cfg;
        cfg.frameWidth=cfg.frameHeight=640;
        cfg.buckets.byClassId={Bucket::Aim};
        RecoveredPidConfig p;
        p.kpX=p.kpY=p.kiX=p.kiY=p.kdX=p.kdY=0;
        p.feedforwardX=.05f; p.smoothMaxPixel=1000;
        RecoveredAimController ctrl;
        ctrl.setConfig(cfg,p);
        ControlInput in;
        in.candidates={Candidate{{300,300,40,40},0,.9}};
        in.cross={310,320}; in.dtSec=.01; in.observationTimeUs=30000000;
        check(ctrl.update(in).counts.x==0, "FF-only acquisition emits no movement");
        in.cross.x=300; in.observationTimeUs+=10000;
        auto out=ctrl.update(in);
        check(out.counts.x==0 && out.trackedVelocity.x==0 && out.followMotion.x>0,
              "moving only the crosshair changes error telemetry but not velocity FF");
        in.detectionFresh=false; ctrl.update(in);
        in.detectionFresh=true; in.cross.x=290; in.observationTimeUs+=10000;
        out=ctrl.update(in);
        check(out.followMotion.norm()==0 && out.counts.x==0,
              "stationary target reacquisition cannot create FF from a crosshair jump");

        RecoveredAimController fixedCross,movingCross;
        fixedCross.setConfig(cfg,p);movingCross.setConfig(cfg,p);
        bool sawVelocityOutput=false, independent=true;
        for(int n=0;n<40;++n) {
            in.candidates={Candidate{{300+n*.5,300,40,40},0,.9}};
            in.observationTimeUs=31000000+n*10000;
            in.cross={310,320};
            const auto a=fixedCross.update(in);
            in.cross.x+=n*.5;
            const auto b=movingCross.update(in);
            independent &= a.counts.x==b.counts.x && a.trackedVelocity.x==b.trackedVelocity.x;
            sawVelocityOutput |= a.counts.x>0 && a.trackedVelocity.x>0;
        }
        check(independent && sawVelocityOutput,
              "moving target feeds FF independently of crosshair-relative error trend");
    }

    // End-to-end wiring: the follow compensator learns lead from sustained
    // aimpoint error and preserves it when the crosshair merely overtakes the
    // target. The background-motion immediate-reset path has been removed, so a
    // genuine target reversal now unwinds lead gradually instead of snapping it
    // to zero.
    {
        ControllerConfig cfg; cfg.frameWidth=cfg.frameHeight=640; cfg.buckets.byClassId={Bucket::Aim};
        RecoveredPidConfig p; p.kpX=p.kpY=.01f; p.kiX=p.kiY=p.kdX=p.kdY=0;
        p.followX=p.followY=10; p.smoothMaxPixel=1000;
        RecoveredAimController ctrl; ctrl.setConfig(cfg,p);
        ControlInput in; in.dtSec=.01;
        ControlOutput out;
        for(int n=0;n<100;++n) {
            in.observationTimeUs=40000000+n*10000;
            const double center=400-n;
            in.candidates={Candidate{{center-20,280,40,80},0,.95}};
            in.cross={center-5,310};
            out=ctrl.update(in);
        }
        check(out.controlAnchor.x-out.anchor.x>10,"controller learns lead while the target tracks across the frame");
        in.observationTimeUs+=10000; in.candidates={Candidate{{280,280,40,80},0,.95}};
        in.cross.x=301;
        out=ctrl.update(in);
        check(out.error.x<0 && out.controlAnchor.x-out.anchor.x>10,
              "controller preserves lead when the crosshair overtakes a still-rightward target");
        for(int n=101;n<=104;++n) {
            in.observationTimeUs=40000000+n*10000;
            in.candidates={Candidate{{280-3.0*(n-100),280,40,80},0,.95}};
            out=ctrl.update(in);
        }
        check(out.controlAnchor.x-out.anchor.x>0,
              "a few reversed frames do not hard-reset the learned lead (no immediate clear)");
    }

    ControllerConfig config;
    config.frameWidth=640; config.frameHeight=640;
    config.buckets.byClassId={Bucket::Aim};
    RecoveredPidConfig pid;
    pid.followX=pid.followY=0;
    RecoveredAimController controller;
    controller.setConfig(config,pid);
    RecoveredPid reference; reference.setConfig(pid);
    bool same=true;
    for (int n=0; n<100; ++n) {
        ControlInput in;
        in.cross={320,320}; in.dtSec=.01;
        in.observationTimeUs=1000000+n*10000;
        in.candidates={Candidate{{285+std::sin(n*.1),280,80,80},0,.9}};
        auto actual=controller.update(in);
        auto expected=reference.update(actual.anchor-in.cross,actual.trackedVelocity,in.dtSec);
        same &= actual.controlAnchor.x==actual.anchor.x && actual.controlAnchor.y==actual.anchor.y &&
            actual.counts.x==expected.counts.x && actual.counts.y==expected.counts.y;
    }
    check(same,"disabled compensation preserves original PID output for the same observations");

    // Identical center errors with different box widths must produce the same
    // lead. This exercises the controller's frame-size wiring, not just the helper.
    RecoveredAimController narrow, wide;
    pid.kpX=pid.kpY=.1f;
    pid.kiX=pid.kiY=pid.kdX=pid.kdY=0;
    pid.feedforwardX=pid.feedforwardY=0;
    pid.smoothMaxPixel=1000;
    pid.followX=10;
    narrow.setConfig(config,pid); wide.setConfig(config,pid);
    Vec2 narrowShift, wideShift;
    for (int n=0; n<150; ++n) {
        ControlInput in;
        in.cross={320,320}; in.dtSec=.01;
        in.observationTimeUs=20000000+n*10000;
        in.candidates={Candidate{{355,300,12,40},0,.9}};
        auto a=narrow.update(in);
        in.candidates={Candidate{{311,300,100,40},0,.9}};
        auto b=wide.update(in);
        narrowShift=a.controlAnchor-a.anchor;
        wideShift=b.controlAnchor-b.anchor;
    }
    check(narrowShift.x>41 && std::abs(narrowShift.x-wideShift.x)<1e-6,
          "narrow and wide boxes allow the same correction at the same lag");
    std::printf("follow compensation: %d failures\n",failures);
    return failures ? 1 : 0;
}
