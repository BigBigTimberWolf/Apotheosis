#pragma once

#include "config/config.h"

#include <string>
#include <vector>
#include <algorithm>

struct SecondaryAimLatch
{
    bool latched = false;
    bool primaryDown = false;
    bool secondaryDown = false;

    void sample(bool primaryPressed, bool secondaryPressed, bool enabled)
    {
        const bool primaryEdge = primaryPressed && !primaryDown;
        const bool secondaryEdge = secondaryPressed && !secondaryDown;
        if (enabled) {
            // Main wins if both are pressed. Repeated activation never toggles.
            if (primaryPressed && (primaryEdge || secondaryEdge)) latched = false;
            else if (secondaryEdge) latched = true;
        }
        primaryDown = primaryPressed;
        secondaryDown = secondaryPressed;
    }
};

// Only identical valid bindings compete for manual activation. An overlapping
// chord still takes precedence over a single key, as it did before.
inline std::vector<std::string> hotkeyBinding(const HotkeyProfile& profile)
{
    auto keys = profile.keys;
    if (keys.empty() || std::any_of(keys.begin(), keys.end(), [](const auto& key) {
        return key.empty() || key == "None";
    })) return {};
    std::sort(keys.begin(), keys.end());
    keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
    if (profile.keys_chord && (profile.keys.size() != 2 || keys.size() != 2)) return {};
    return keys;
}

inline bool sameHotkeyBinding(const HotkeyProfile& a, const HotkeyProfile& b)
{
    if (!a.enabled || !b.enabled) return false;
    if (a.group != b.group || a.keys_chord != b.keys_chord) return false;
    const auto keys = hotkeyBinding(a);
    return !keys.empty() && keys == hotkeyBinding(b);
}

inline bool hasHotkeyConflict(const std::vector<HotkeyProfile>& profiles, int index)
{
    if (index < 0 || index >= static_cast<int>(profiles.size())) return false;
    for (int i = 0; i < static_cast<int>(profiles.size()); ++i)
        if (i != index && sameHotkeyBinding(profiles[index], profiles[i])) return true;
    return false;
}

inline int preferredHotkey(const std::vector<HotkeyProfile>& profiles, int index)
{
    if (index < 0 || index >= static_cast<int>(profiles.size()) || !profiles[index].enabled) return -1;
    int fallback = index;
    for (int i = static_cast<int>(profiles.size()) - 1; i >= 0; --i) {
        if (!profiles[i].enabled) continue;
        if (i != index && !sameHotkeyBinding(profiles[index], profiles[i])) continue;
        if (profiles[i].activation_selected) return i;
        fallback = std::max(fallback, i);
    }
    return fallback;
}

inline bool activateHotkey(std::vector<HotkeyProfile>& profiles, int index)
{
    if (index < 0 || index >= static_cast<int>(profiles.size()) || !profiles[index].enabled ||
        !hasHotkeyConflict(profiles, index)) return false;
    bool changed = false;
    for (int i = 0; i < static_cast<int>(profiles.size()); ++i) {
        if (!profiles[i].enabled || (i != index && !sameHotkeyBinding(profiles[index], profiles[i]))) continue;
        const bool selected = i == index;
        changed |= profiles[i].activation_selected != selected;
        profiles[i].activation_selected = selected;
    }
    return changed;
}

inline bool validActivationKey(const std::vector<HotkeyProfile>& profiles, int index)
{
    if (!hasHotkeyConflict(profiles, index)) return false;
    const auto& profile = profiles[index];
    if (!profile.enabled) return false;
    if (profile.activation_key.empty() || profile.activation_key == "None") return false;
    for (const auto& other : profiles) {
        if (!other.enabled) continue;
        if (other.group != profile.group) continue;
        if (std::find(other.keys.begin(), other.keys.end(), profile.activation_key) != other.keys.end())
            return false;
        if (&other != &profile && sameHotkeyBinding(profile, other) &&
            other.activation_key == profile.activation_key) return false;
    }
    return true;
}

class HotkeyActivationSampler
{
public:
    template <typename IsPressed>
    bool sample(std::vector<HotkeyProfile>& profiles, const std::string& group, IsPressed&& pressed)
    {
        states_.resize(profiles.size());
        std::vector<int> requested;
        for (int i = 0; i < static_cast<int>(profiles.size()); ++i) {
            const auto& profile = profiles[i];
            auto& state = states_[i];
            const auto binding = hotkeyBinding(profile);
            const bool enabled = profile.enabled && profile.group == group && validActivationKey(profiles, i);
            const bool changed = state.key != profile.activation_key || state.name != profile.name ||
                state.group != profile.group || state.binding != binding || state.chord != profile.keys_chord;
            const bool down = enabled && pressed(profile.activation_key);
            if (enabled && state.enabled && !changed && group == previousGroup_ && down && !state.down)
                requested.push_back(i);
            state = {profile.activation_key, profile.name, profile.group, binding, profile.keys_chord, enabled, down};
        }
        previousGroup_ = group;
        bool activated = false;
        // Separate binding families can share an activation key. Within one
        // family, simultaneous different selectors retain list priority.
        for (int index : requested) activated = activateHotkey(profiles, index) || activated;
        return activated;
    }
private:
    struct State {
        std::string key, name, group;
        std::vector<std::string> binding;
        bool chord = false, enabled = false, down = false;
    };
    std::vector<State> states_;
    std::string previousGroup_;
};

// Profiles later in the group override earlier ones while both are held.
// This lets a secondary hotkey (for example X2) override a held primary
// hotkey (for example right mouse) without changing either profile's targets.
template <typename IsPressed>
int selectActiveHotkeyIndex(const std::vector<HotkeyProfile>& hotkeys,
                            const std::string& group, IsPressed&& isPressed)
{
    // A two-key chord is more specific than an overlapping single-key hotkey.
    // Within each kind, the later profile in the group still has priority.
    for (bool chordPass : {true, false})
        for (size_t i = hotkeys.size(); i > 0; --i)
        {
            const size_t index = i - 1;
            const auto& hk = hotkeys[index];
            if (!hk.enabled || hk.group != group || hk.keys_chord != chordPass) continue;
            if (preferredHotkey(hotkeys, static_cast<int>(index)) != static_cast<int>(index)) continue;
            if (chordPass) {
                if (hk.keys.size() != 2 || hk.keys[0].empty() || hk.keys[1].empty() ||
                    hk.keys[0] == "None" || hk.keys[1] == "None" ||
                    hk.keys[0] == hk.keys[1]) continue;
                if (isPressed(std::vector<std::string>{hk.keys[0]}) &&
                    isPressed(std::vector<std::string>{hk.keys[1]})) return static_cast<int>(index);
            } else if (isPressed(hk.keys)) return static_cast<int>(index);
        }
    return -1;
}
