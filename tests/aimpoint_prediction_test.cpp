#include "control/recovered_aim_controller.h"
#include <cmath>
#include <cstdio>
using namespace control;
namespace {
int failures = 0;
void check(bool pass, const char* name) {
    if (!pass) { ++failures; std::printf("FAIL: %s\n", name); }
}
}
int main() {
    // Retained target name for existing build scripts; the old frame predictor
    // has been replaced with following compensation. Not run in this change.
    FollowCompensator c;
    Vec2 offset;
    for (int n=0; n<200; ++n)
        offset=c.update({5,-5},{80,80},{1,0},1000000+n*10000,.01);
    check(offset.x>1 && offset.x<5 && offset.y==0,
          "persistent lag builds a gradual correction on the enabled axis only");
    double before=offset.x;
    for (int n=0; n<100; ++n)
        offset=c.update({100,0},{80,80},{1,0},2990000,.01);
    check(offset.x<before+.1, "duplicate images cannot accumulate repeated error");
    for (int n=0; n<100; ++n)
        offset=c.update({-5,0},{80,80},{1,0},3000000+n*10000,.01);
    check(offset.x<before && offset.x>0,
          "persistent opposite error gradually retracts correction without resetting it");
    offset=c.update({5,5},{80,80},{0,0},4000000,.01);
    check(offset.norm()==0, "disabling returns the original point immediately");
    c.reset();
    for (int n=0; n<200; ++n) {
        offset=c.update({5,0},{80,80},{1,0},5000000+n*10000,.01);
        c.setSaturation({1,0});
    }
    check(offset.x==0, "output saturation prevents accumulating more same-direction correction");
    offset=c.update({5,0},{80,80},{1,0},8000000,.01);
    check(offset.norm()==0, "long image gap restarts correction");

    c.reset();
    for (int n=0; n<200; ++n)
        offset=c.update({80,-80},{20,20},{10,10},9000000+n*10000,.01);
    check(offset.x>5 && offset.x<=10 && offset.y<-5 && offset.y>=-10,
          "correction respects half the observation field, independent of target size");
    c.reset();
    for (int n=0; n<100; ++n)
        offset=c.update({41,0},{416,416},{10,0},11000000+n*10000,.01);
    check(offset.x>41 && offset.x<208,
          "41px persistent lag can build more than a single-digit correction");
    offset=c.update({-41,0},{416,416},{10,0},12000000,.01);
    check(offset.x>40, "crossing the original aimpoint never hard-clears compensation");
    for (int n=1; n<30; ++n)
        offset=c.update({-41,0},{416,416},{10,0},12000000+n*10000,.01);
    check(offset.x>0,
          "opposite point error adjusts the existing lead instead of treating crossing as motion reversal");
    c.reset();
    for (int n=0; n<80; ++n)
        offset=c.update({41,5},{416,416},{50,10},16000000+n*10000,.01);
    check(offset.x>150, "strength above ten is used without an internal ten cap");
    const double oldY=offset.y;
    offset=c.update({-41,5},{416,416},{50,10},16800000,.01);
    check(offset.x>150 && offset.y>=oldY,
          "even a large error crossing preserves the stored correction on both axes");
    c.reset();
    for (int n=0; n<200; ++n)
        offset=c.update({double(n%2),0},{80,80},{10,0},12000000+n*10000,.01);
    check(offset.x>.1 && offset.y==0,
          "zero/one pixel observations with a positive mean continue accumulating");
    c.reset();
    for (int n=0; n<200; ++n)
        offset=c.update({.1,0},{80,80},{10,0},25000000+n*10000,.01);
    check(offset.x>.1,
          "subpixel persistent error has no hidden direction or learning deadband");
    c.reset();
    for (int n=0; n<100; ++n)
        offset=c.update({5,0},{80,80},{10,0},28000000+n*10000,.01);
    for (int n=0; n<100; ++n)
        offset=c.update({5,0},{80,80},{10,0},28990000,.01);
    const double beforeZero=offset.x;
    offset=c.update({0,0},{80,80},{10,0},29000000,.01);
    check(offset.x>beforeZero+.001,
          "zero raw error still allows residual filtered trend to grow the lead");
    const double beforeCrossing=offset.x;
    offset=c.update({-.5,0},{80,80},{10,0},29010000,.01);
    check(offset.x>beforeCrossing,
          "slightly crossing the original aimpoint does not abruptly stop accumulation");
    for (int n=0; n<300; ++n)
        offset=c.update({0,0},{80,80},{10,0},29020000+n*10000,.01);
    check(offset.x<beforeZero+2,
          "with zero error the filtered trend fades instead of growing indefinitely");
    c.reset();
    for (int n=0; n<200; ++n)
        offset=c.update({2.0+double(n%2),0},{80,80},{10,0},14000000+n*10000,.01);
    check(offset.x>.1 && offset.y==0,
          "small persistent lag above the noise floor still builds compensation");
    const double learned=offset.x;
    offset=c.update({-1,0},{80,80},{10,0},16000000,.01);
    check(offset.x>=learned,
          "one-pixel crossing does not discard an established lead");
    c.reset();
    for (int n=0; n<10; ++n)
        offset=c.update({41,0},{416,416},{10,0},18000000+n*10000,.01);
    check(offset.x>3,
          "large lag starts visible compensation within 100ms at strength ten");
    c.reset();
    for (int n=0; n<200; ++n)
        offset=c.update({n==0 ? 0.0 : (n%2 ? 1.0 : -1.0),0},
                        {80,80},{10,0},15000000+n*10000,.01);
    check(offset.norm()==0, "alternating zero-mean noise does not build correction");

    // Motion and point error deliberately vary independently: crossing the
    // point must never masquerade as a turn, at any tested target speed.
    for (double speed : {2.0, 30.0, 300.0}) {
        FollowCompensator moving;
        double position=0;
        int64_t stamp=40000000;
        auto tick=[&](double e, double v, bool valid=true) {
            position+=v*.01;
            stamp+=10000;
            return moving.update({e,0},{416,416},{10,0},stamp,.01,
                                  FollowMotion{{position,0},{},valid});
        };
        for (int n=0; n<100; ++n) offset=tick(8,speed);
        for (int n=0; n<100; ++n) offset=tick(0,speed);
        check(moving.stateX()==FollowCompensator::Remembered,
              "a settled correction is remembered across slow, medium and fast motion");
        const double stored=offset.x;
        offset=tick(-8,speed);
        check(offset.x>stored*.9 && moving.stateX()!=FollowCompensator::Reversed,
              "error crossing with unchanged target motion retains correction");
        offset=tick(0,0);
        check(offset.x>stored*.9 && moving.stateX()!=FollowCompensator::Stopped,
              "one zero velocity sample cannot declare a stop");
        for (int n=0; n<80; ++n) offset=tick(0,speed);
        bool preset=false;
        for (int n=0; n<40; ++n) {
            offset=tick(0,-speed);
            preset |= moving.stateX()==FollowCompensator::Preset && moving.presetAmount().x<0;
        }
        check(preset && offset.x<0,
              "confirmed reversal reuses a recent settled correction without waiting for error buildup");
        check(std::abs(offset.x)<=stored*1.1,
              "preset magnitude stays bounded by the previous settled correction");
        for (int n=0; n<80; ++n) offset=tick(0,-speed);
        bool stopped=false;
        for (int n=0; n<200; ++n) {
            offset=tick(0,0);
            stopped |= moving.stateX()==FollowCompensator::Stopped;
        }
        check(stopped && std::abs(offset.x)<.01,
              "sustained zero motion eventually confirms stopping at every speed");
    }

    FollowCompensator uncertain;
    for (int n=0; n<200; ++n)
        offset=uncertain.update({8,0},{416,416},{10,0},50000000+n*10000,.01,
            FollowMotion{{double(n),0},{},true});
    const double retained=offset.x;
    offset=uncertain.update({-8,0},{416,416},{10,0},52000000,.01);
    check(offset.x>retained*.9 && uncertain.stateX()==FollowCompensator::Uncertain,
          "unavailable motion evidence never hard-clears compensation");
    offset=uncertain.update({-8,0},{416,416},{10,0},52000000,.01,
                            FollowMotion{{-100,0},{},true});
    check(offset.x>retained*.9 && uncertain.stateX()==FollowCompensator::Uncertain,
          "duplicate image cannot confirm a turn or apply a preset");

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
