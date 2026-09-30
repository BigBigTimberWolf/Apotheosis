
#include "runtime/aim_loop.h"

#include "config/config.h"   // HotkeyProfile / ClassFilterState

#include <algorithm>

namespace runtime::aim_loop
{

std::vector<int> buildClassBuckets(const std::vector<int>& aimClassIds)
{
    int maxClassId = -1;
    for (int id : aimClassIds)
        maxClassId = std::max(maxClassId, id);
    if (maxClassId < 0)
        return {};

    std::vector<int> buckets(static_cast<size_t>(maxClassId) + 1, 0);
    for (int id : aimClassIds)
    {
        if (id >= 0)
            buckets[static_cast<size_t>(id)] = 1;
    }
    return buckets;
}

control::ControllerConfig toControllerConfig(const FlatConfig& flat)
{
    control::ControllerConfig cfg;
    cfg.frameWidth = flat.detectionResolution;
    cfg.frameHeight = flat.detectionResolution;
    cfg.fovWidth = std::clamp(flat.fovX, 1, 4096);
    cfg.fovHeight = std::clamp(flat.fovY, 1, 4096);
    cfg.dynamicFovEnabled = flat.dynamicFovEnabled;
    cfg.dynamicFovSize = flat.dynamicFovSize;
    cfg.dynamicFovShrinkMs = flat.dynamicFovShrinkMs;
    cfg.dynamicFovExpandMs = flat.dynamicFovExpandMs;

    {
        int maxId = -1;
        for (const auto& cf : flat.classFilters)
            maxId = std::max(maxId, cf.first);
        for (int id : flat.aimClassIds)
            maxId = std::max(maxId, id);

        if (maxId >= 0)
        {
            cfg.buckets.byClassId.assign(static_cast<size_t>(maxId) + 1,
                                         control::Bucket::Delete);

            for (const auto& cf : flat.classFilters)
            {
                if (cf.first < 0)
                    continue;
                switch (cf.second)
                {
                // "Aim" in the global class list makes a class available for
                // aiming, but each hotkey's aim_classes is the actual target
                // selection. Otherwise an empty hotkey inherits every global
                // Aim class (and appears to use another hotkey's targets).
                case 2: cfg.buckets.byClassId[static_cast<size_t>(cf.first)] = control::Bucket::Filter; break;
                case 1: cfg.buckets.byClassId[static_cast<size_t>(cf.first)] = control::Bucket::Filter; break;
                default: cfg.buckets.byClassId[static_cast<size_t>(cf.first)] = control::Bucket::Delete; break;
                }
            }

            for (int id : flat.aimClassIds)
            {
                if (id >= 0)
                    cfg.buckets.byClassId[static_cast<size_t>(id)] = control::Bucket::Aim;
            }
        }
    }

    cfg.selector.hysteresisRatio = flat.hysteresisRatio;
    cfg.selector.maxDistancePx = flat.maxDistancePx;

    cfg.selector.minConfByClassId.clear();
    for (const auto& mc : flat.classMinConf)
    {
        const int id = mc.first;
        if (id < 0) continue;
        if (static_cast<size_t>(id) >= cfg.selector.minConfByClassId.size())
            cfg.selector.minConfByClassId.resize(static_cast<size_t>(id) + 1, 0.0);
        cfg.selector.minConfByClassId[static_cast<size_t>(id)] =
            std::max(cfg.selector.minConfByClassId[static_cast<size_t>(id)], mc.second);
    }

    cfg.aimPoint.yOffset = flat.yOffset;
    cfg.aimPoint.yOffsetMax = flat.yOffsetMax;
    cfg.aimPoint.xOffset = flat.xOffset;
    cfg.aimPoint.xOffsetMax = flat.xOffsetMax;
    cfg.aimPoint.randomSeed = flat.randomSeed;

    cfg.classAimPoints.clear();
    cfg.classPriorityById.clear();
    for (size_t rank = 0; rank < flat.aimClassIds.size(); ++rank) {
        const int id = flat.aimClassIds[rank];
        if (id < 0) continue;
        if (static_cast<size_t>(id) >= cfg.classPriorityById.size())
            cfg.classPriorityById.resize(static_cast<size_t>(id) + 1, -1);
        if (cfg.classPriorityById[static_cast<size_t>(id)] < 0)
            cfg.classPriorityById[static_cast<size_t>(id)] = static_cast<int>(rank);
    }
    cfg.classAimPoints.reserve(flat.classAimPoints.size());
    for (const auto& cap : flat.classAimPoints)
    {
        control::ClassAimPoint p;
        p.classId = static_cast<int>(cap[0]);
        p.yOffset = cap[1];
        p.yOffsetMax = cap[2];
        p.xOffset = cap[3];
        p.xOffsetMax = cap[4];
        p.yOffset = std::clamp(p.yOffset, 0.0, 1.0);
        p.yOffsetMax = std::clamp(p.yOffsetMax, 0.0, 1.0);
        if (p.yOffsetMax < p.yOffset)
            std::swap(p.yOffset, p.yOffsetMax);
        p.xOffset = std::clamp(p.xOffset, 0.0, 1.0);
        p.xOffsetMax = std::clamp(p.xOffsetMax, 0.0, 1.0);
        if (p.xOffsetMax < p.xOffset)
            std::swap(p.xOffset, p.xOffsetMax);
        if (p.classId >= 0)
            cfg.classAimPoints.push_back(p);
    }

    cfg.requireFreshDetection = true;
    // resolveCrosshair already selects the screen center when color detection
    // is missing or stale. That fallback must remain eligible for aiming.
    cfg.requireFreshCrosshair = false;

    return cfg;
}

FlatConfig flattenProfile(const HotkeyProfile& hk, int detectionResolution,
                          const std::vector<ClassFilterState>& classFilters,
                          const Config& globalConfig,
                          bool scopeEngaged)
{
    FlatConfig flat;
    flat.fovX = hk.fovX;
    flat.fovY = hk.fovY;
    flat.dynamicFovEnabled = hk.dynamic_fov_enabled;
    flat.dynamicFovSize = hk.dynamic_fov_size;
    flat.dynamicFovShrinkMs = hk.dynamic_fov_shrink_ms;
    flat.dynamicFovExpandMs = hk.dynamic_fov_expand_ms;

    // ── 瞄准控制器参数组: 默认档 / 开镜档 ────────────────────────────────
    // ★ 自动开镜生效期间, 若该热键开了独立开镜参数, 就用开镜档【整组】取代
    //   默认档 —— 开镜后游戏内灵敏度被倍率放大, 镜前那套增益在镜内会过冲。
    // ★ 没开(默认)时走的就是默认档, 逐位与从前一致。
    const bool useScope = scopeEngaged && hk.scope_ctl_enabled != 0;
    const AimCtlParams cp = useScope ? hk.ctl_scope : ctlParamsOf(hk);
    flat.scopeCtlActive = useScope;

    flat.kpX = cp.kp_x;
    flat.kpY = cp.kp_y;
    flat.kiX = cp.ki_x;
    flat.kiY = cp.ki_y;
    flat.kdX = cp.kd_x;
    flat.kdY = cp.kd_y;
    flat.tauUnwindSec = cp.tau_unwind_sec;
    flat.tauDerivSec = cp.tau_deriv_sec;
    flat.iMax = cp.i_max;
    flat.maxOutputCounts = cp.max_output_counts;
    flat.pFullScalePx = cp.p_full_scale_px;
    flat.predictLeadMs = cp.predict_lead_ms;
    flat.predictMaxVelocityPxPerSec = cp.predict_max_velocity_px_s;
    flat.predictMaxLeadRatio = cp.predict_max_lead_ratio;
    flat.kPxPerCount = cp.k_px_per_count;
    flat.inflightBeta = cp.inflight_beta;
    flat.inflightDeadTimeMs = cp.inflight_dead_time_ms;
    flat.randomSeed = cp.random_seed;

    flat.yOffset = hk.ctl_y_offset;
    flat.yOffsetMax = hk.ctl_y_offset_max;
    flat.xOffset = hk.ctl_x_offset;
    flat.xOffsetMax = hk.ctl_x_offset_max;

    // 全局选靶与稳定器
    flat.hysteresisRatio = globalConfig.target_hysteresis_ratio;
    flat.maxDistancePx = globalConfig.target_max_distance_px;
    flat.matchCenterRatio = globalConfig.target_match_center_ratio;
    flat.areaRatioTol = globalConfig.target_area_ratio_tol;
    flat.kSnapMult = globalConfig.target_k_snap_mult;
    flat.minAspect = globalConfig.target_min_aspect;
    flat.maxAspect = globalConfig.target_max_aspect;
    flat.detectionResolution = detectionResolution;
    flat.aimClassIds.reserve(hk.aim_classes.size());
    for (const auto& ac : hk.aim_classes)
        flat.aimClassIds.push_back(ac.class_id);

    flat.classAimPoints.clear();
    flat.classAimPoints.reserve(hk.aim_classes.size());
    flat.classMinConf.clear();
    flat.classMinConf.reserve(hk.aim_classes.size());
    for (const auto& ac : hk.aim_classes)
    {
        flat.classAimPoints.push_back({ static_cast<double>(ac.class_id),
                                        static_cast<double>(ac.y_offset),
                                        static_cast<double>(ac.y_offset_max),
                                        static_cast<double>(ac.x_offset),
                                        static_cast<double>(ac.x_offset_max) });
        flat.classMinConf.push_back({ ac.class_id, static_cast<double>(ac.min_conf) });
    }

    static_assert(static_cast<int>(ClassBucket::Delete) == 0, "ClassBucket::Delete 必须 = 0");
    static_assert(static_cast<int>(ClassBucket::Filter) == 1, "ClassBucket::Filter 必须 = 1");
    static_assert(static_cast<int>(ClassBucket::Aim)    == 2, "ClassBucket::Aim 必须 = 2");
    flat.classFilters.reserve(classFilters.size());
    for (const auto& cf : classFilters)
        flat.classFilters.emplace_back(cf.class_id, static_cast<int>(cf.bucket));

    return flat;
}

}
