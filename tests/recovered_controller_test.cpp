#include "control/recovered_pid.h"
#include "control/recovered_tracker.h"
#include "control/recovered_aim_controller.h"
#include "runtime/aim_loop.h"
#include "runtime/aimpoint_recoil.h"
#include "config/config.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

namespace {
int failures = 0;
void check(bool okay, const char* message)
{
    if (!okay) { std::printf("FAIL: %s\n", message); ++failures; }
}
}

int main()
{
    using namespace control;

    runtime::AimpointRecoilGate recoil;
    check(recoil.update(true, false, 1000, 30.0, 60.0) == 0.0,
          "aim hotkey alone does not start aimpoint recoil");
    check(recoil.update(true, true, 1100, 30.0, 60.0) == 0.0 &&
          std::abs(recoil.update(true, true, 1300, 30.0, 60.0) - 6.0) < 1e-6,
          "recoil starts when fire joins the held aim hotkey");
    check(recoil.update(true, false, 1400, 30.0, 60.0) == 0.0 &&
          recoil.update(true, true, 1500, 30.0, 60.0) == 0.0,
          "fire release resets recoil for the next shot");
    check(recoil.update(true, true, 5000, 30.0, 60.0) == 60.0 &&
          recoil.update(false, true, 5100, 30.0, 60.0) == 0.0,
          "recoil is capped and aim-hotkey release resets it");
    // Automatic trigger presses are supplied as fireHeld only after the
    // driver accepts left-down; their release follows the same reset path.
    check(recoil.update(true, true, 6000, 30.0, 60.0) == 0.0 &&
          std::abs(recoil.update(true, true, 6100, 30.0, 60.0) - 3.0) < 1e-6 &&
          recoil.update(true, false, 6200, 30.0, 60.0) == 0.0,
          "automatic trigger hold and release use the same recoil gate");

    // FOV interpolation and target-switch regressions.
    {
        DynamicFov fov;
        fov.configure({100,100}, true, 10, 200, 120);
        const Box distant{30,-5,10,10};
        for (int n=0; n<100; ++n) fov.follow(distant, {}, .01);
        check(fov.radii().x>45 && fov.contains(distant, {}, true),
              "slow pursuit retains room instead of shrinking through the locked target");
        for (int n=0; n<100; ++n) fov.follow({-5,-5,10,10}, {}, .01);
        check(fov.radii().x<5.1 && fov.radii().y<5.1,
              "FOV smoothly contracts once the crosshair catches up");
        fov.follow(distant, {}, .01);
        check(fov.radii().x>5.1 && fov.radii().x<50 &&
              fov.contains(distant, {}, true) && !fov.contains(distant, {}, false),
              "expansion is smooth and the established lock survives the contracted gate");
        check(!fov.contains({100,100,10,10}, {}, true),
              "lock protection does not bypass the original FOV boundary");
        const double beforeRelease=fov.radii().x;
        fov.release(.01);
        check(fov.radii().x>beforeRelease && fov.radii().x<50,
              "target loss uses configured expansion time instead of snapping to full FOV");
        fov.configure({100,100}, true, 10, 200, 0);
        fov.release(.01);
        check(fov.radii().x==50, "zero expansion time restores FOV immediately");
        fov.reset();
        check(fov.radii().x==50 && fov.radii().y==50, "FOV reset restores original dimensions");

        RecoveredPid masked;
        RecoveredPidConfig p;
        p.kdX=p.kdY=0;
        p.feedforwardX=p.feedforwardY=1;
        p.maskX=true;
        masked.setConfig(p);
        for (int n=0; n<30; ++n) {
            const auto step=masked.update({30,30},{10,10},.01);
            check(step.counts.x==0 && step.integral.x==0 && step.carry.x==0 && step.counts.y>0,
                  "masking X suppresses PID, FF and carry without disabling Y");
        }
        p.maskY=true;
        masked.setConfig(p);
        const auto both=masked.update({30,30},{10,10},.01);
        check(both.counts.x==0 && both.counts.y==0 && both.integral.y==0 && both.carry.y==0,
              "masking both axes suppresses both outputs and clears stored accumulation");
    }

    {
        int previousAcquireMs = 0;
        for (int expandMs : {0, 500, 1000}) {
            ControllerConfig cfg;
            cfg.frameWidth = cfg.frameHeight = 320;
            cfg.fovWidth = cfg.fovHeight = 200;
            cfg.dynamicFovEnabled = true;
            cfg.dynamicFovSize = 20;
            cfg.dynamicFovShrinkMs = 200;
            cfg.dynamicFovExpandMs = expandMs;
            cfg.buckets.byClassId = {Bucket::Aim, Bucket::Aim};
            RecoveredAimController controller;
            controller.setConfig(cfg, {});
            ControlInput in;
            in.cross = {160, 160}; in.dtSec = .01;
            in.candidates = {Candidate{{155,155,10,10},0,.9}};
            ControlOutput out;
            for (int i=0; i<150; ++i) out = controller.update(in);
            check(out.fovRadii.x < 10.01, "initial lock contracts the FOV");
            in.candidates = {Candidate{{200,155,10,10},1,.9}};
            int acquiredMs = 0;
            for (int i=1; i<=200; ++i) {
                const auto before = out.fovRadii;
                out = controller.update(in);
                if (!out.engaged) continue;
                acquiredMs = i * 10;
                check(out.targetClassId == 1, "the newly acquired target is selected");
                if (expandMs > 0) {
                    check(out.fovRadii.x < before.x + 6 && out.fovRadii.y <= before.y,
                          "new target preserves FOV instead of snapping to full size");
                    std::printf("FOV %d ms: acquire %d ms, radius %.2f -> %.2f\n",
                                expandMs, acquiredMs, before.x, out.fovRadii.x);
                }
                break;
            }
            check(acquiredMs > previousAcquireMs, "longer expansion delays entry of an outside target");
            previousAcquireMs = acquiredMs;
            controller.reset();
            in.candidates.clear();
            out = controller.update(in);
            check(out.fovRadii.x == 100, "explicit stop/reset still restores the full FOV");
        }
    }

    RecoveredPid pid;
    // Crosshair enable controls the base Ki reversal gate in every bank.
    for (bool enabled : {false,true}) {
        HotkeyProfile hk;
        hk.crosshair_detect_enabled=enabled;
        hk.recovered_pid.kiX=1;
        hk.recovered_secondary_pid.kiX=2;
        hk.recovered_scope_pid.kiX=3;
        for(int bank=0;bank<3;++bank) {
            const auto mapped=runtime::aim_loop::pidForProfile(hk,bank==2,bank==1);
            check(mapped.preserveIntegralOnReverse==enabled && mapped.kiX==bank+1,
                  "crosshair reversal flag reaches all three selected PID banks");
        }
        RecoveredPidConfig c;
        c.kpX=c.kiX=c.kdX=0;
        c.kpY=1.41f;c.kiY=.001f;c.kdY=.002f;
        c.smoothMaxPixel=80;
        c.preserveIntegralOnReverse=enabled;
        RecoveredPid replay;replay.setConfig(c);
        const auto a=replay.update({0,49.649238586},{},.1);
        const auto b=replay.update({0,-50},{},.001);
        const auto end=replay.update({0,-50},{},.001);
        // New deterministic baseline after the always-on stabilizers (filtered D,
        // decoupled integral). The old firmware-exact ±ki knife-edge that made the
        // reversal branch differ by one count is gone; both banks now resolve the
        // same, which this snapshot guards against future accidental drift.
        check(a.counts.y==24 && b.counts.y==-27 && end.counts.y==-27,
              "stabilized PID three-frame reversal: deterministic new baseline");
    }
    for (float nextKi : {2.0f,.5f}) {
        RecoveredPid p;RecoveredPidConfig c;
        c.kpX=c.kpY=c.kdX=c.kdY=0;c.kiX=c.kiY=1;
        // Deadzone 0 isolates the Ki-edit integral-clear rule from the always-on
        // in-deadzone integral freeze; this rule itself is unchanged.
        c.deadzoneX=c.deadzoneY=0;
        c.preserveIntegralOnReverse=true;p.setConfig(c);
        p.update({2,3},{},.1);
        c.kiX=nextKi;p.setConfig(c);
        const auto step=p.update({0,0},{},.01);
        check(std::abs(step.integral.x)<1e-6 &&
              std::abs(step.integral.y-.3)<1e-6,
              "second-port Ki edit clears only its own axis integral");
    }
    {
        RecoveredPid p;RecoveredPidConfig c;
        c.kpX=c.kpY=1;c.kiX=c.kiY=c.kdX=c.kdY=0;
        c.deadzoneX=c.deadzoneY=0;p.setConfig(c);
        p.update({1,0},{},.01); // Stores one-third count.
        c.feedforwardX=.1f;p.setConfig(c);
        check(p.update({1,0},{},.01).counts.x==1,
              "FF-only edits preserve integer carry");
        c.kpX=2;p.setConfig(c);
        check(p.update({1,0},{},.01).counts.x==0,
              "second-port KpX edit skips one send");
        c.segmentEnabled=true;c.segment=3;p.setConfig(c);
        check(p.update({3,0},{},.01).counts.x==0,
              "second-port segment toggle skips one send");
        c.kiX=40;c.feedforwardX=15;p.setConfig(c);
        check(p.config().kiX==10 && p.config().feedforwardX==10,
              "second-port Ki and FF use their original 0..10 range");
    }
    RecoveredPidConfig config;
    config.kpX = config.kpY = 0.5f;
    config.kiX = config.kiY = 0.5f;
    config.kdX = config.kdY = 0.0f;
    config.deadzoneX = 10.0f;
    config.deadzoneY = 0.0f;
    config.smoothMaxPixel = 3.0f;
    config.segmentEnabled = true;
    config.segment = 2.0f;
    pid.setConfig(config);
    const Counts expected[] = { { 2, -1 }, { 1, -1 }, { 2, -1 } };
    for (int frame = 0; frame < 3; ++frame)
    {
        const auto step = pid.update({ 8, -4 }, {}, 0.0625);
        check(step.counts.x == expected[frame].x &&
              step.counts.y == expected[frame].y,
              "recovered direct PID sequence (77)");
    }

    pid.reset();
    config.smoothMaxPixel = 10.0f;
    config.feedforwardX = config.feedforwardY = 1.0f;
    pid.setConfig(config);
    const auto withFeedforward = pid.update({ 8, -4 }, { 2, -2 }, 0.0625);
    check(withFeedforward.counts.x == 3 && withFeedforward.counts.y == -2,
          "feedforward joins after PID deadzone (77)");
    config.segment = 1.0f;
    pid.setConfig(config);
    const auto changed = pid.update({ 8, -4 }, {}, 0.0625);
    check(changed.counts.x == 0 && changed.counts.y == 0,
          "segment change skips the current send (92)");
    const auto stable = pid.update({ 8, -4 }, {}, 0.0625);
    check(stable.counts.x != 0, "stable segment resumes sending");

    // D is low-pass filtered now (always on): the raw difference is still
    // reported in derivativeRaw, but the term fed to the output is attenuated on
    // a spike and converges toward the raw value under a sustained slope. This is
    // what turns kd into usable phase lead instead of amplified noise.
    RecoveredPid filteredD;
    RecoveredPidConfig filteredDConfig;
    filteredDConfig.kpX = filteredDConfig.kpY = 0.0f;
    filteredDConfig.kiX = filteredDConfig.kiY = 0.0f;
    filteredDConfig.kdX = filteredDConfig.kdY = 1.0f;
    filteredDConfig.deadzoneX = filteredDConfig.deadzoneY = 0.0f;
    filteredD.setConfig(filteredDConfig);
    filteredD.update({ 0, 0 }, {}, 0.01);
    const auto spike = filteredD.update({ 10, 0 }, {}, 0.01);
    check(std::abs(spike.derivativeRaw.x - 1000.0) < 0.1 &&
          spike.pid.x > 100.0 && spike.pid.x < 950.0,
          "D is low-pass filtered: raw difference reported, output attenuated on a spike");
    double lastD = spike.pid.x;
    for (int n = 2; n <= 8; ++n)
        lastD = filteredD.update({ 10.0 * n, 0 }, {}, 0.01).pid.x; // constant slope
    check(lastD > spike.pid.x && lastD < 1000.0,
          "filtered D converges toward the raw derivative under a sustained slope");

    RecoveredTracker tracker;
    const auto first = tracker.update({ Candidate{ { 126, 114, 20, 20 }, 0, 0.9 } }, 0.01);
    const auto second = tracker.update({ Candidate{ { 134, 114, 20, 20 }, 0, 0.9 } }, 0.01);
    check(first.size() == 1 && second.size() == 1 && first[0].id == second[0].id,
          "track keeps identity across a matched frame");
    check(second.size() == 1 && std::abs(second[0].box.centerX() - 138.6668) < 0.01,
          "recovered default position smoothing (90)");
    check(second.size() == 1 && std::abs(second[0].velocity.x - 152.39) < 0.2,
          "no-event two-frame velocity (90)");
    RecoveredTracker withMoveFeedback;
    withMoveFeedback.update({ Candidate{ { 126, 114, 20, 20 }, 0, 0.9 } }, 0.01);
    const auto feedbackFrame = withMoveFeedback.update(
        { Candidate{ { 134, 114, 20, 20 }, 0, 0.9 } }, 0.01, { 4, 0 });
    check(feedbackFrame.size() == 1 &&
          feedbackFrame[0].velocity.x > second[0].velocity.x,
          "second-port FF velocity includes successful mouse movement events");
    tracker.update({}, 0.01);
    const auto reacquired = tracker.update({ Candidate{ { 134, 114, 20, 20 }, 0, 0.9 } }, 0.01);
    check(reacquired.size() == 1 && reacquired[0].id == first[0].id,
          "track survives a one-frame miss (31)");

    const Candidate row0{ { 130.485947, 113.547417, 14.514389, 18.778999 }, 0,
                          0.544530 };
    const Candidate row1{ { 130.616486, 113.466026, 13.957153, 17.991509 }, 0,
                          0.322401 };
    Candidate moved0 = row0, moved1 = row1;
    moved0.box.x += 1.0;
    moved1.box.x += 1.0;
    RecoveredDualTracker dual;
    dual.setFrameSize(256, 256);
    const auto dualFirst = dual.update({ row0, row1 }, 0.01);
    const auto dualSecond = dual.update({ moved0, moved1 }, 0.01);
    check(dualFirst.size() == 1 && dualSecond.size() == 1 &&
          dualSecond[0].id == dualFirst[0].id,
          "0x168 merges same-frame overlap while E8 keeps both tracks (181/183)");
    check(dualSecond.size() == 1 &&
          std::abs(dualSecond[0].velocity.x - 23.2624) < 0.12,
          "dual join exposes E8 velocity from selected state row (183)");
    RecoveredDualTracker reversedOrder;
    reversedOrder.setFrameSize(256, 256);
    const auto reversedFirst = reversedOrder.update({ row1, row0 }, 0.01);
    Candidate rising0 = row0, falling1 = row1;
    rising0.box.y += 0.24;
    falling1.box.y -= 0.24;
    const auto reversedSecond = reversedOrder.update({ falling1, rising0 }, 0.01);
    check(reversedFirst.size() == 1 && reversedSecond.size() == 1 &&
          reversedSecond[0].velocity.y < -2.5,
          "common input order can change joined E8 velocity sign (182)");

    // The motion tracker sees both overlapping rows, while the box tracker
    // merges them. Their next IDs then differ, even for a spatially matched
    // moving target. An unrelated motion row with the same numeric ID must
    // not prevent attaching the moving target's velocity.
    RecoveredDualTracker offsetIds;
    offsetIds.setFrameSize(256, 256);
    const Candidate other{ { 180, 113, 16, 20 }, 0, 0.9 };
    offsetIds.update({ row0, row1, other }, 0.01);
    Candidate otherMoved = other;
    otherMoved.box.x += 2.0;
    const auto offsetIdTracks = offsetIds.update({ row0, row1, otherMoved }, 0.01);
    bool movingVelocityFound = false;
    for (const auto& track : offsetIdTracks)
        if (track.box.centerX() > 160.0)
            movingVelocityFound = track.velocity.x > 1.0;
    check(movingVelocityFound,
          "merged overlap cannot hide a separate moving target velocity");

    RecoveredAimController eventController;
    ControllerConfig eventConfig;
    eventConfig.frameWidth = eventConfig.frameHeight = 256;
    eventConfig.buckets.byClassId = { Bucket::Aim };
    eventController.setConfig(eventConfig, RecoveredPidConfig{});
    ControlInput eventInput;
    eventInput.candidates = { row0, row1 };
    eventInput.cross = { 128, 128 };
    eventInput.dtSec = 0.01;
    eventController.update(eventInput);
    eventInput.candidates = { moved0, moved1 };
    const auto eventSelected = eventController.update(eventInput);
    check(eventSelected.engaged &&
          std::abs(eventSelected.trackedVelocity.x - 23.2624) < 0.2 &&
          eventSelected.trackedVelocity.y == 0,
          "selected diagnostic velocity comes only from observed image motion");
    RecoveredAimController slowerFrameClock;
    slowerFrameClock.setConfig(eventConfig, RecoveredPidConfig{});
    eventInput.candidates = { row0, row1 };
    eventInput.trackingDtSec = 0.02;
    slowerFrameClock.update(eventInput);
    eventInput.candidates = { moved0, moved1 };
    const auto slowerVelocity = slowerFrameClock.update(eventInput);
    check(slowerVelocity.engaged &&
          slowerVelocity.trackedVelocity.x < eventSelected.trackedVelocity.x,
          "tracker uses capture-frame interval independently of PID interval");

    RecoveredAimController switching;
    ControllerConfig switchConfig = eventConfig;
    RecoveredPidConfig switchPid;
    switching.setConfig(switchConfig, switchPid);
    ControlInput switchInput;
    switchInput.dtSec = 0.01;
    switchInput.cross = { 128, 128 };
    switchInput.candidates = { Candidate{ { 70, 110, 20, 20 }, 0, 0.9 } };
    const auto oldTarget = switching.update(switchInput);
    switchInput.candidates = { Candidate{ { 170, 110, 20, 20 }, 0, 0.9 } };
    switching.update(switchInput);
    switching.update(switchInput);
    const auto newTarget = switching.update(switchInput);
    check(oldTarget.engaged && newTarget.engaged &&
          oldTarget.targetId != newTarget.targetId &&
          std::abs(newTarget.derivativeRaw.x) < 0.01,
          "second-port target switch seeds derivative from new target");

    RecoveredAimController controller;
    ControllerConfig controllerConfig;
    controllerConfig.buckets.byClassId = { Bucket::Aim };
    controllerConfig.aimPoint.xOffset = controllerConfig.aimPoint.xOffsetMax = 0.5;
    controllerConfig.aimPoint.yOffset = controllerConfig.aimPoint.yOffsetMax = 0.5;
    controller.setConfig(controllerConfig, config);
    ControlInput input;
    input.dtSec = 0.05;
    input.cross = { 128, 128 };
    input.candidates = { Candidate{ { 126, 114, 20, 20 }, 0, 0.9 } };
    const auto selected = controller.update(input);
    check(selected.engaged && selected.hasTarget, "new runtime controller selects a track");
    input.candidates.clear();
    input.detectionFresh = false;
    const auto missing = controller.update(input);
    check(!missing.engaged, "missing frame produces no PID movement");
    input.candidates = { Candidate{ { 126, 114, 20, 20 }, 0, 0.9 } };
    input.detectionFresh = true;
    const auto selectedAgain = controller.update(input);
    check(selectedAgain.engaged && selectedAgain.targetId == selected.targetId,
          "one-frame loss keeps target identity");

    RecoveredAimController geometry;
    controllerConfig.frameWidth = controllerConfig.frameHeight = 256;
    controllerConfig.aimPoint.xOffset = controllerConfig.aimPoint.xOffsetMax = 0.875;
    controllerConfig.aimPoint.yOffset = controllerConfig.aimPoint.yOffsetMax = 0.475;
    geometry.setConfig(controllerConfig, config);
    const auto point = geometry.update(input);
    check(point.anchor.x == 144.0 && point.anchor.y == 124.0,
          "source X rounding and Y truncation differ at half pixel (21)");

    runtime::aim_loop::FlatConfig flat;
    flat.detectionResolution = 256;
    flat.aimClassIds = { 0 };
    flat.classAimPoints = { { 0, 0.4, 0.4, 0.8, 0.8 } };
    const auto wired = runtime::aim_loop::toControllerConfig(flat);
    check(wired.frameWidth == 256 && wired.buckets.bucketOf(0) == Bucket::Aim &&
          wired.classAimPoints.size() == 1 && wired.classAimPoints[0].xOffset == 0.8,
          "runtime config reaches recovered geometry and class mapping");
    RecoveredAimController crosshairFallback;
    crosshairFallback.setConfig(wired, config);
    ControlInput fallbackInput;
    fallbackInput.dtSec = 0.05;
    fallbackInput.cross = { 128, 128 };
    fallbackInput.crosshairFresh = false;
    fallbackInput.candidates = { Candidate{ { 126, 114, 20, 20 }, 0, 0.9 } };
    const auto fallback = crosshairFallback.update(fallbackInput);
    check(!wired.requireFreshCrosshair && fallback.engaged && fallback.hasTarget &&
          fallback.cross.x == 128 && fallback.cross.y == 128,
          "missing crosshair color keeps screen-center aiming active");

    // The global class list is shared by all hotkeys. Only aimClassIds may
    // select a target for the currently active hotkey.
    runtime::aim_loop::FlatConfig perHotkey;
    perHotkey.detectionResolution = 256;
    perHotkey.classFilters = { { 0, 2 }, { 1, 2 } };
    ControlInput hotkeyInput;
    hotkeyInput.dtSec = 0.05;
    hotkeyInput.cross = { 128, 128 };
    hotkeyInput.candidates = {
        Candidate{ { 126, 114, 20, 20 }, 0, 0.9 },
        Candidate{ { 146, 114, 20, 20 }, 1, 0.9 }
    };

    RecoveredAimController emptyHotkey;
    emptyHotkey.setConfig(runtime::aim_loop::toControllerConfig(perHotkey), config);
    check(!emptyHotkey.update(hotkeyInput).hasTarget,
          "hotkey without aim classes cannot inherit global aim targets");

    perHotkey.aimClassIds = { 0 };
    RecoveredAimController rightHotkey;
    rightHotkey.setConfig(runtime::aim_loop::toControllerConfig(perHotkey), config);
    const auto rightTarget = rightHotkey.update(hotkeyInput);
    check(rightTarget.hasTarget && rightTarget.targetClassId == 0,
          "right hotkey selects only its configured class");

    perHotkey.aimClassIds = { 1 };
    RecoveredAimController x2Hotkey;
    x2Hotkey.setConfig(runtime::aim_loop::toControllerConfig(perHotkey), config);
    const auto x2Target = x2Hotkey.update(hotkeyInput);
    check(x2Target.hasTarget && x2Target.targetClassId == 1,
          "X2 hotkey selects its own class instead of right hotkey targets");

    // Both profiles can aim at the same class while keeping distinct Y points.
    perHotkey.aimClassIds = { 0 };
    perHotkey.classAimPoints = { { 0, 0.25, 0.25, 0.5, 0.5 } };
    RecoveredAimController lowAim;
    lowAim.setConfig(runtime::aim_loop::toControllerConfig(perHotkey), config);
    const auto lowPoint = lowAim.update(hotkeyInput);

    perHotkey.classAimPoints = { { 0, 1.0, 1.0, 0.5, 0.5 } };
    RecoveredAimController highAim;
    highAim.setConfig(runtime::aim_loop::toControllerConfig(perHotkey), config);
    const auto highPoint = highAim.update(hotkeyInput);
    check(lowPoint.hasTarget && highPoint.hasTarget &&
          highPoint.anchor.y < lowPoint.anchor.y,
          "X2 class Y offset changes the calculated aim point");

    // A zero seed uses the built-in deterministic seed. It must not replace a
    // nonzero class Y range with zero while the same target moves.
    perHotkey.classAimPoints = { { 0, 0.8, 0.9, 0.5, 0.5 } };
    RecoveredAimController randomY;
    randomY.setConfig(runtime::aim_loop::toControllerConfig(perHotkey), config);
    bool randomYStayedInRange = true;
    for (int frame = 0; frame < 12; ++frame)
    {
        hotkeyInput.frameIndex = frame + 1;
        hotkeyInput.candidates[0].box.x = 126 + frame;
        const auto moving = randomY.update(hotkeyInput);
        const double top = moving.targetBox.y;
        const double height = moving.targetBox.h;
        const double ratioFromTop = (moving.anchor.y - top) / height;
        randomYStayedInRange &= moving.hasTarget &&
            ratioFromTop >= 0.08 && ratioFromTop <= 0.22;
    }
    check(randomYStayedInRange,
          "zero random seed preserves a nonzero class Y range while moving");

    // The hotkey-held recoil mode moves the aim point on the same locked
    // track; it does not need to rebuild or reset the controller each frame.
    hotkeyInput.aimpointRecoilYpx = 8.0;
    const auto shiftedPoint = lowAim.update(hotkeyInput);
    hotkeyInput.aimpointRecoilYpx = 16.0;
    const auto shiftedAgain = lowAim.update(hotkeyInput);
    hotkeyInput.aimpointRecoilYpx = 0.0;
    const auto restoredPoint = lowAim.update(hotkeyInput);
    check(shiftedPoint.targetId == lowPoint.targetId &&
          shiftedAgain.targetId == lowPoint.targetId &&
          shiftedPoint.anchor.y == lowPoint.anchor.y + 8.0 &&
          shiftedAgain.anchor.y == lowPoint.anchor.y + 16.0 &&
          restoredPoint.anchor.y == lowPoint.anchor.y,
          "held recoil offset grows and resets without switching target");

    {
        ControllerConfig cc;cc.frameWidth=cc.frameHeight=320;cc.fovWidth=cc.fovHeight=320;cc.buckets.byClassId={Bucket::Aim,Bucket::Delete};
        RecoveredPidConfig pid;pid.kpX=pid.kpY=1;
        RecoveredAimController controller;controller.setConfig(cc,pid);
        ControlInput in;in.dtSec=.01;in.cross={160,160};in.candidates={{{180,140,20,40},0,.9},{{230,140,20,40},1,.8}};
        in.macro.classId=1;auto selected=controller.update(in);check(selected.hasTarget&&selected.targetClassId==1,"macro class selects requested class without mutating profile");
        in.macro.partX=0;in.macro.partY=0;selected=controller.update(in);check(selected.anchor.x==selected.targetBox.x&&selected.anchor.y==selected.targetBox.y,"macro aim point applies within selected box");
        in.macro.classId=-1;in.macro.command=1;in.macro.commandSerial=1;in.macro.x=190;in.macro.y=160;selected=controller.update(in);const int locked=selected.targetId;
        in.candidates.push_back({{159,140,20,40},0,1});selected=controller.update(in);check(selected.targetId==locked,"macro target lock survives nearer candidate");
        in.macro.speed=100;in.macro.smoothing=.5;selected=controller.update(in);check(std::hypot(selected.counts.x,selected.counts.y)<=2,"macro speed limit bounds output with integer carry");
        in.macro.command=4;in.macro.commandSerial=2;selected=controller.update(in);check(!selected.hasTarget&&!selected.engaged,"macro clear target releases current frame");
    }
    {
        ControllerConfig cc;cc.frameWidth=cc.frameHeight=320;cc.fovWidth=cc.fovHeight=320;
        cc.buckets.byClassId={Bucket::Aim,Bucket::Aim};cc.classPriorityById={1,0};
        RecoveredAimController controller;controller.setConfig(cc,RecoveredPidConfig{});
        ControlInput in;in.dtSec=.01;in.cross={160,160};in.candidates={{{155,140,20,40},0,.9},{{220,140,20,40},1,.8}};
        in.macro.command=2;in.macro.commandSerial=1;const auto out=controller.update(in);
        check(out.targetClassId==1,"clearing macro overrides preserves configured class priority on first frame");
    }
    // Aim-point extrapolation velocity: snaps within one observation frame on
    // reversal and sudden stop, while the frozen FF velocity stays gated. dt=4ms.
    {
        RecoveredTracker tr; RecoveredTrackerConfig cfg; tr.setConfig(cfg);
        auto step=[&](RecoveredTracker& t,double cx){
            std::vector<Candidate> c{Candidate{{cx-20.0,100,40,40},0,0.9}};
            auto v=t.update(c,0.004); return v.empty()?RecoveredTrack{}:v.front();
        };
        RecoveredTrack t;
        for(int n=0;n<20;++n) t=step(tr,200.0+n*10.0); // steady +2500 px/s
        check(t.predictVelocity.x>2000 && t.predictVelocity.x<3000,
              "predict velocity converges to the steady target velocity");
        const auto rev=step(tr,380.0); // one leftward step reverses direction
        check(rev.predictVelocity.x<-1000 && rev.velocity.x>-500,
              "predict velocity snaps to the reversal in one frame while FF stays gated");
        RecoveredTracker tr2; tr2.setConfig(cfg); RecoveredTrack t2;
        for(int n=0;n<20;++n) t2=step(tr2,200.0+n*10.0);
        const auto stop=step(tr2,390.0); // center unchanged => sudden stop
        check(std::abs(stop.predictVelocity.x)<300,
              "predict velocity collapses to zero on a sudden stop in one frame");
        // Units regression: a near-stationary target with small detection jitter
        // must stay a small predict velocity. The noise gate is px/second (matches
        // raw/pv); without the /dt it was ~250x too small, so every frame snapped
        // to the noisy raw velocity and the lead hunted around the aim point.
        RecoveredTracker tr3; tr3.setConfig(cfg); RecoveredTrack t3;
        for(int n=0;n<40;++n){
            const double cx=300.0+(n%2 ? 0.5 : -0.5); // +/-0.5 px detection jitter
            std::vector<Candidate> c{Candidate{{cx-20.0,100,40,40},0,0.9}};
            auto v=tr3.update(c,0.004); if(!v.empty()) t3=v.front();
        }
        check(std::abs(t3.predictVelocity.x)<800.0,
              "stationary target with detection jitter keeps a small predict velocity");
    }
    // Regression (crosshair-detect stall): a pure-P tune (ki=0) must never build
    // an integral, even when the output saturates and reversal-reset is disabled
    // (preserveIntegralOnReverse == crosshair color-detect mode). The removed
    // back-calculation anti-windup used to inject an opposing integral here that
    // stalled the crosshair at the detection-box edge instead of the aim point.
    {
        RecoveredPid p; RecoveredPidConfig c;
        c.kpX=c.kpY=2; c.kiX=c.kiY=0; c.kdX=c.kdY=0; c.smoothMaxPixel=50;
        c.deadzoneX=c.deadzoneY=0; c.preserveIntegralOnReverse=true;
        p.setConfig(c);
        for(int n=0;n<40;++n) p.update({100,0},{},0.004); // large, saturating error
        const auto sat=p.update({100,0},{},0.004);
        check(std::abs(sat.integral.x)<1e-9 && std::abs(sat.pid.x-200.0)<1e-6,
              "pure-P tune builds no integral under saturation (no anti-windup injection)");
    }
    return failures ? 1 : 0;
}
