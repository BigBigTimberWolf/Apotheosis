#include "mouse/trigger_flash_post.h"

#include <stdexcept>

#define CHECK(x) do { if (!(x)) throw std::runtime_error(#x); } while (false)

static control::Box box(double x) { return {x, 90.0, 20.0, 20.0}; }

static boss::TriggerFlashPostController::Input frame(int64_t time, double x) {
    boss::TriggerFlashPostController::Input in;
    in.nowMs = time;
    in.fresh = true;
    in.selected = true;
    in.selectedClassId = 1;
    in.selectedBox = box(x);
    in.candidates.push_back({box(x), 1, 0.9});
    return in;
}

int main() {
    using Controller = boss::TriggerFlashPostController;
    Controller controller;
    Controller::Settings settings;
    settings.mode = 1;
    settings.pixelsPerCount = 1.0;
    controller.configure(settings);

    auto in = frame(1000, 180);
    auto action = controller.tick(in);
    CHECK(!action.blockNormal && controller.phase() == Controller::Phase::Tracking);
    in = frame(1020, 150);
    in.confirmedMove = {30, 0}; // movement sent by the ordinary aim path
    action = controller.tick(in);
    CHECK(!action.blockNormal && action.move.x == 0);
    controller.shotSent(1, box(150)); // accepted ordinary trigger shot arms flash
    in = frame(1080, 150);
    action = controller.tick(in);
    CHECK(!action.blockNormal && action.move.x == 0); // still shooting this target
    in = frame(1100, 400);
    in.candidates.push_back({box(150), 1, 0.9});
    action = controller.tick(in);
    CHECK(action.blockNormal && controller.phase() == Controller::Phase::Tracking);
    CHECK(controller.shotTargetLocked() &&
          controller.shotTargetDistance({box(150), 1, 0.9}, {}) == 0.0);
    in = {};
    in.nowMs = 1150;
    in.fresh = true;
    action = controller.tick(in);
    CHECK(action.blockNormal && action.move.x == 0); // one missing frame is insufficient
    in.nowMs = 1700;
    in.fresh = false;
    action = controller.tick(in);
    CHECK(action.blockNormal && action.move.x == 0); // frozen capture is not a kill
    in.nowMs = 1720;
    in.fresh = true;
    action = controller.tick(in);
    CHECK(action.blockNormal && action.move.x == -30 &&
          action.cancelPendingMove && controller.phase() == Controller::Phase::Return);
    in.nowMs = 1740;
    in.confirmedMove = {-30, 0};
    action = controller.tick(in);
    CHECK(action.blockNormal && action.move.x == 0 &&
          controller.phase() == Controller::Phase::ReturnSettle);
    in = frame(1760, 400); // another target is visible before return settles
    in.fresh = false;
    in.confirmedMove = {10, 0}; // late confirmation of a normal aim move
    action = controller.tick(in);
    CHECK(action.blockNormal && controller.phase() == Controller::Phase::Return);
    in.nowMs = 1780;
    in.confirmedMove = {};
    action = controller.tick(in);
    CHECK(action.blockNormal && action.move.x == -10);
    in.nowMs = 1800;
    in.confirmedMove = {-10, 0};
    action = controller.tick(in);
    CHECK(action.blockNormal && controller.phase() == Controller::Phase::ReturnSettle);
    in.nowMs = 1820;
    in.confirmedMove = {};
    in.fresh = true;
    action = controller.tick(in);
    CHECK(action.blockNormal && controller.phase() == Controller::Phase::ReturnSettle);
    in.nowMs = 1860;
    action = controller.tick(in);
    CHECK(action.blockNormal && controller.phase() == Controller::Phase::Idle);
    in.nowMs = 1880;
    action = controller.tick(in);
    CHECK(controller.phase() == Controller::Phase::Tracking);
    controller.reset();

    // Retargeting before the first shot keeps the original view as the
    // return origin; prior confirmed aim moves must not be forgotten.
    settings.disappearMs = 0;
    controller.configure(settings);
    in = frame(1800, 180);
    controller.tick(in);
    in = frame(1820, 30);
    in.confirmedMove = {30, 0};
    controller.tick(in);
    controller.shotSent(1, box(30));
    in = {};
    in.nowMs = 1830;
    in.fresh = true;
    controller.tick(in);
    in.nowMs = 1840;
    action = controller.tick(in);
    CHECK(action.move.x == -30);
    controller.reset();

    settings.returnYPercent = 75;
    controller.configure(settings);
    in = frame(1900, 180);
    controller.tick(in);
    in = frame(1920, 160);
    in.confirmedMove = {20, 100};
    controller.tick(in);
    controller.shotSent(1, box(160));
    in = {};
    in.nowMs = 1930;
    in.fresh = true;
    controller.tick(in);
    in.nowMs = 1940;
    action = controller.tick(in);
    CHECK(action.move.x == -20 && action.move.y == -75);
    in.nowMs = 1960;
    in.confirmedMove = {-20, -75};
    action = controller.tick(in);
    CHECK(action.move.x == 0 && action.move.y == 0 &&
          controller.phase() == Controller::Phase::ReturnSettle);
    controller.reset();

    settings.mode = 2;
    settings.spinCountsPerTurn = 14000;
    settings.turnStepDegrees = 90;
    settings.turnDurationMs = 40;
    settings.turnHoldMs = 300;
    controller.configure(settings);
    in = {};
    in.nowMs = 2000;
    in.fresh = true;
    action = controller.tick(in);
    CHECK(action.blockNormal && action.move.x == 0 &&
          controller.phase() == Controller::Phase::Turn);
    in = frame(2020, 80);
    action = controller.tick(in);
    CHECK(action.blockNormal && action.move.x > 0 &&
          controller.phase() == Controller::Phase::Turn); // ignore target during turn
    const int firstMove = action.move.x;
    in = {};
    in.nowMs = 2040;
    in.confirmedMove = {firstMove, 0};
    action = controller.tick(in);
    CHECK(action.move.x > 0);
    const int secondMove = action.move.x;
    in.nowMs = 2060;
    in.confirmedMove = {secondMove, 0};
    action = controller.tick(in);
    CHECK(action.move.x == 0 && controller.phase() == Controller::Phase::Settle);
    in = frame(2080, 80);
    action = controller.tick(in);
    CHECK(action.move.x == 0 && controller.phase() == Controller::Phase::Tracking);
    in = frame(2090, 80);
    action = controller.tick(in);
    CHECK(!action.blockNormal); // original aim/trigger path resumes
    controller.shotSent(1, box(80));
    in = {};
    in.nowMs = 2100;
    in.fresh = true;
    controller.tick(in);
    in.nowMs = 2200;
    action = controller.tick(in);
    CHECK(action.blockNormal && controller.phase() == Controller::Phase::Settle &&
          action.cancelPendingMove); // keep clearing this direction first
    in = frame(2230, 400); // a second enemy in the same direction
    action = controller.tick(in);
    CHECK(action.blockNormal && controller.phase() == Controller::Phase::Tracking);
    action = controller.tick(frame(2250, 400));
    CHECK(!action.blockNormal);
    controller.shotSent(1, box(400));
    in = {};
    in.nowMs = 2260;
    in.fresh = true;
    controller.tick(in);
    in.nowMs = 2360;
    action = controller.tick(in);
    CHECK(action.blockNormal && controller.phase() == Controller::Phase::Settle);
    in.nowMs = 2500;
    action = controller.tick(in);
    CHECK(controller.phase() == Controller::Phase::Settle); // hold starts after the kill
    in.nowMs = 2660;
    action = controller.tick(in);
    CHECK(controller.phase() == Controller::Phase::Turn); // next 90° only after dwell
    controller.reset();

    settings.spinCountsPerTurn = 120000;
    settings.turnDurationMs = 10;
    controller.configure(settings);
    in = {};
    in.nowMs = 2700;
    in.fresh = true;
    controller.tick(in);
    in.nowMs = 2720;
    action = controller.tick(in);
    CHECK(action.move.x == 10000); // transport limit, no frame-by-frame scan
    controller.reset();

    settings.spinCountsPerTurn = 100;
    settings.turnStepDegrees = 100; // final segment is only 60°
    controller.configure(settings);
    in = {};
    in.nowMs = 2900;
    in.fresh = true;
    action = controller.tick(in);
    int totalSent = 0;
    for (int i = 0; i < 100 && controller.phase() != Controller::Phase::Stopped; ++i) {
        totalSent += action.move.x;
        in.nowMs += 20;
        in.confirmedMove = {action.move.x, 0};
        action = controller.tick(in);
    }
    CHECK(controller.phase() == Controller::Phase::Stopped && totalSent == 100);
    in = frame(in.nowMs + 20, 100);
    action = controller.tick(in);
    CHECK(controller.phase() == Controller::Phase::Tracking && action.move.x == 0);
    controller.reset();

    settings.mode = 1;
    settings.disappearMs = 250;
    controller.configure(settings);
    in = frame(3000, 100);
    controller.tick(in);
    controller.shotSent(1, box(100));
    in = {};
    in.nowMs = 3010;
    in.fresh = true;
    controller.tick(in);
    in.nowMs = 3020;
    action = controller.tick(in);
    CHECK(controller.phase() == Controller::Phase::Tracking && action.blockNormal);
    in.nowMs = 3270;
    action = controller.tick(in);
    CHECK(controller.phase() == Controller::Phase::ReturnSettle &&
          action.cancelPendingMove && action.blockNormal);
    controller.reset();
    CHECK(controller.phase() == Controller::Phase::Idle);
}
