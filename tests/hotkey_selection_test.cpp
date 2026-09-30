#include "keyboard/hotkey_selection.h"

#include <cstdio>

int main()
{
    std::vector<HotkeyProfile> hotkeys(3);
    hotkeys[0].group = hotkeys[1].group = "default";
    hotkeys[0].keys = { "RightMouseButton" };
    hotkeys[1].keys = { "X2MouseButton" };
    hotkeys[2].group = "other";
    hotkeys[2].keys = { "X2MouseButton" };

    auto choose = [&](bool right, bool x2) {
        return selectActiveHotkeyIndex(hotkeys, "default", [&](const auto& keys) {
            return keys[0] == "RightMouseButton" ? right : x2;
        });
    };

    if (choose(false, false) != -1 || choose(true, false) != 0 ||
        choose(false, true) != 1 || choose(true, true) != 1)
    {
        std::puts("FAIL: X2 must override a held right button in the same group");
        return 1;
    }
    SecondaryAimLatch latch;
    latch.sample(false, true, false); // Binding initialization only synchronizes held keys.
    if (latch.latched) return 2;
    latch.sample(false, true, true);  // Holding secondary must not create a new edge.
    if (latch.latched) return 3;
    latch.sample(false, false, true);
    latch.sample(false, true, true);
    if (!latch.latched) return 4;
    latch.sample(false, false, true);
    latch.sample(false, true, true); // Pressing secondary again keeps secondary.
    if (!latch.latched) return 5;
    latch.sample(false, false, true);
    latch.sample(true, false, true); // Only the main selector returns to main.
    if (latch.latched) return 6;
    latch.sample(false, false, true);
    latch.sample(true, false, true);
    if (latch.latched) return 7;
    latch.sample(false, false, true);
    latch.sample(true, true, true); // Simultaneous selectors choose main.
    if (latch.latched) return 8;
    latch.sample(false, false, true);
    latch.sample(false, true, true);
    latch.sample(false, false, false); // Leaving aim/group does not lose selection.
    if (!latch.latched) return 9;

    // Identical bindings select whole profiles, independently of mouse-down.
    std::vector<HotkeyProfile> duplicates(4);
    for (auto& profile : duplicates) {
        profile.group = "default";
        profile.keys = {"RightMouseButton"};
    }
    duplicates[0].name = "rifle";
    duplicates[1].name = "sniper";
    duplicates[2].group = "other";
    duplicates[3].keys = {"X2MouseButton"};
    duplicates[0].activation_key = "Key1";
    duplicates[1].activation_key = "Key2";
    duplicates[0].aim_classes = {{3}};
    duplicates[1].aim_classes = {{7}};
    auto select = [&](bool right, bool side) {
        return selectActiveHotkeyIndex(duplicates, "default", [&](const auto& keys) {
            return keys.front() == "RightMouseButton" ? right : side;
        });
    };
    if (!hasHotkeyConflict(duplicates, 0) || hasHotkeyConflict(duplicates, 2) ||
        hasHotkeyConflict(duplicates, 3) || select(true, false) != 1) return 10;
    if (!activateHotkey(duplicates, 0) || select(true, false) != 0 ||
        select(false, false) != -1 || select(true, true) != 3) return 11;
    if (duplicates[select(true, false)].aim_classes[0].class_id != 3) return 12;
    HotkeyActivationSampler sampler;
    std::string down;
    auto sample = [&] { return sampler.sample(duplicates, "default", [&](const auto& key) { return key == down; }); };
    sample();
    down = "Key2";
    if (!sample() || select(true, false) != 1 ||
        duplicates[select(true, false)].aim_classes[0].class_id != 7) return 13;
    if (sample()) return 14; // Held activation key cannot fight a UI click.
    activateHotkey(duplicates, 0);
    if (sample() || select(true, false) != 0) return 15;
    down.clear(); sample(); down = "Key2";
    if (!sample() || select(true, false) != 1) return 16;
    down.clear(); sample(); down = "Key1";
    if (!sample() || select(true, false) != 0) return 17;
    std::swap(duplicates[0], duplicates[1]);
    if (sample() || select(true, false) != 1) return 18; // Selection moves with profile.
    duplicates[0].activation_key = "Key1";
    if (validActivationKey(duplicates, 0) || validActivationKey(duplicates, 1)) return 19;
    duplicates[0].activation_key = "RightMouseButton";
    if (validActivationKey(duplicates, 0)) return 20;
    duplicates.erase(duplicates.begin());
    if (hasHotkeyConflict(duplicates, 0) || select(true, false) != 0) return 21;
    HotkeyProfile a, b;
    a.keys = {"RightMouseButton", "X2MouseButton"};
    b.keys = {"X2MouseButton", "RightMouseButton"};
    a.keys_chord = b.keys_chord = true;
    if (!sameHotkeyBinding(a,b)) return 22;
    b.keys_chord = false;
    if (sameHotkeyBinding(a,b)) return 23;
    a.keys.clear(); b.keys.clear(); a.keys_chord = false;
    if (sameHotkeyBinding(a,b)) return 24;
    return 0;
}
