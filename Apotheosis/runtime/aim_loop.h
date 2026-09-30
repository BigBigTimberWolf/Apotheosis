#ifndef RUNTIME_AIM_LOOP_H
#define RUNTIME_AIM_LOOP_H

#include <array>
#include <utility>
#include <vector>

#include "control/controller_contract.h"

struct HotkeyProfile;
struct ClassFilterState;
class Config;

namespace runtime::aim_loop
{

bool tick();
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
    double kpX = 35.0, kpY = 35.0;
    double kiX = 0.0, kiY = 0.0;
    double kdX = 0.0, kdY = 0.0;
    double tauUnwindSec = 0.030;
    double tauDerivSec = 0.020;
    double iMax = 0.0;
    int    maxOutputCounts = 200;
    double pFullScalePx = 0.0;

    // 在途补偿：提前时间（总开关，0=关闭）+ 速度上限 + 距离上限（对角线倍数）。
    double predictLeadMs = 0.0;
    double predictMaxVelocityPxPerSec = 0.0;
    double predictMaxLeadRatio = 0.0;

    // 灵敏度折算系数 k (像素/计数)：把自身下发速率折算回像素、加回观测速度，
    // 修正"准星追近导致画面观测速度偏小"的系统性偏差。0 = 关闭这项修正。
    double kPxPerCount = 0.0;

    // 在途自身位移补偿 (Smith)：把已下发但画面未显现的自身位移从输出里扣掉。
    // inflightBeta = 0 时关闭，与没有这个功能逐位相同。
    double inflightBeta = 0.0;
    double inflightDeadTimeMs = 46.0;

    double yOffset = 0.5;
    double yOffsetMax = 0.5;
    double xOffset = 0.5;
    double xOffsetMax = 0.5;
    double hysteresisRatio = 1.3;
    double maxDistancePx = 0.0;
    int    randomSeed = 0;

    double matchCenterRatio = 0.5;
    double areaRatioTol = 2.0;
    double kSnapMult = 1.15;
    double minAspect = 0.2;
    double maxAspect = 5.0;
    std::vector<int> aimClassIds;

    std::vector<std::array<double, 5>> classAimPoints;

    std::vector<std::pair<int, double>> classMinConf;

    std::vector<std::pair<int, int>> classFilters;
    int detectionResolution = 320;

    // 这一拍【是否在用开镜档】(自动开镜生效 且 该热键打开了独立开镜参数)。
    // 供日志/回归断言读取; 不参与控制计算。
    bool scopeCtlActive = false;
};

control::ControllerConfig toControllerConfig(const FlatConfig& flat);

// 把热键 profile 摊平成控制器参数。
//
// ★ scopeEngaged = 自动开镜(自动扳机按住的右键)【当前生效】。它为 true 且
//   profile 打开了 scope_ctl_enabled 时, 这一拍用【开镜档】(hk.ctl_scope)
//   整组取代默认档 —— 开镜后游戏内灵敏度被倍率放大, 镜前那套增益会过冲。
//   默认值 false ⇒ 不传该参数的老调用点逐位与从前一致。
FlatConfig flattenProfile(const HotkeyProfile& hk, int detectionResolution,
                          const std::vector<ClassFilterState>& classFilters,
                          const Config& globalConfig,
                          bool scopeEngaged = false);

std::vector<int> buildClassBuckets(const std::vector<int>& aimClassIds);

inline constexpr double kMinDtSec = 0.001;
inline constexpr double kMaxDtSec = 0.250;

inline bool dtIsUsable(double dtSec)
{
    return dtSec >= kMinDtSec && dtSec <= kMaxDtSec;
}

}

#endif // RUNTIME_AIM_LOOP_H
