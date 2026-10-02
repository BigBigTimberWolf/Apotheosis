#ifndef RUNTIME_AIM_LOOP_H
#define RUNTIME_AIM_LOOP_H

#include <array>
#include <utility>
#include <vector>

#include "control/controller_contract.h"

struct HotkeyProfile;
struct ClassFilterState;
class Config;
namespace control { struct RecoveredPidConfig; }

namespace runtime::aim_loop
{

bool tick(int* consumedVersion = nullptr);
void reset();
void resetMouse();

bool active();
// Active hotkey index only while a fresh, selected target is actually being aimed at.
int activeTargetHotkey();
bool prepareForMacro();
void finishMacroControl(bool moved);
void resetPidAxes(bool x, bool y);

struct FlatConfig
{
    int fovX = 106, fovY = 74;
    bool dynamicFovEnabled = false;
    int dynamicFovSize = 40;
    int dynamicFovShrinkMs = 200;
    int dynamicFovExpandMs = 120;
    double yOffset = 0.5;
    double yOffsetMax = 0.5;
    double xOffset = 0.5;
    double xOffsetMax = 0.5;
    double hysteresisRatio = 1.3;
    double maxDistancePx = 0.0;
    int    randomSeed = 0;

    std::vector<int> aimClassIds;

    std::vector<std::array<double, 5>> classAimPoints;

    std::vector<std::pair<int, double>> classMinConf;

    std::vector<std::pair<int, int>> classFilters;
    int detectionResolution = 320;

};

control::ControllerConfig toControllerConfig(const FlatConfig& flat);
control::RecoveredPidConfig pidForProfile(const HotkeyProfile& hk, bool scope, bool secondary);

// 把热键 profile 摊平成控制器参数。
//
FlatConfig flattenProfile(const HotkeyProfile& hk, int detectionResolution,
                          const std::vector<ClassFilterState>& classFilters,
                          const Config& globalConfig);

std::vector<int> buildClassBuckets(const std::vector<int>& aimClassIds);

inline constexpr double kMinDtSec = 0.001;
inline constexpr double kMaxDtSec = 0.250;

inline bool dtIsUsable(double dtSec)
{
    return dtSec >= kMinDtSec && dtSec <= kMaxDtSec;
}

}

#endif // RUNTIME_AIM_LOOP_H
