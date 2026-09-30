#include "mouse/switch31_shot_gate.h"
#include <stdexcept>

#define CHECK(x) do { if (!(x)) throw std::runtime_error(#x); } while (false)

int main() {
    mouse_async::Switch31ShotGate gate;
    gate.onPress(false, true, 50, 20, 100);
    CHECK(gate.onRelease(130) == -1); // left down was never sent
    gate.onPress(true, true, 50, 20, 200);
    CHECK(gate.onRelease(205) == -1); // click shorter than minimum hold
    gate.onPress(true, true, 50, 20, 300);
    CHECK(gate.onRelease(330) == 50); // left-up result cannot erase an accepted shot
    gate.onPress(true, false, 50, 20, 400);
    CHECK(gate.onRelease(430) == -1); // keyboard unavailable
    gate.onPress(true, true, 75, 40, 500);
    CHECK(gate.onRelease(539) == -1);
    gate.onPress(true, true, 75, 40, 600);
    CHECK(gate.onRelease(640) == 75);
    CHECK(gate.onRelease(700) == -1); // only one switch per shot

    gate.onPress(true, true, 50, 20, 800);
    CHECK(gate.remainingHoldMs(805) == 15);
    CHECK(gate.onRelease(805 + gate.remainingHoldMs(805)) == 50);
    CHECK(gate.remainingHoldMs(830) == 0);
}
