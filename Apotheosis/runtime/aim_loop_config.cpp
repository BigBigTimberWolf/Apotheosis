
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
                case 2: cfg.buckets.byClassId[static_cast<size_t>(cf.first)] = control::Bucket::Aim;    break;
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

    cfg.stabilizer.matchCenterRatio = flat.matchCenterRatio;
    cfg.stabilizer.areaRatioTol     = flat.areaRatioTol;
    cfg.stabilizer.kSnapMult        = flat.kSnapMult;
    cfg.stabilizer.minAspect        = flat.minAspect;
    cfg.stabilizer.maxAspect        = flat.maxAspect;

    cfg.aimPoint.yOffset = flat.yOffset;
    cfg.aimPoint.yOffsetMax = flat.yOffsetMax;
    cfg.aimPoint.randomSeed = flat.randomSeed;

    cfg.classAimPoints.clear();
    cfg.classAimPoints.reserve(flat.classAimPoints.size());
    for (const auto& cap : flat.classAimPoints)
    {
        control::ClassAimPoint p;
        p.classId = static_cast<int>(cap[0]);
        p.yOffset = cap[1];
        p.yOffsetMax = cap[2];
        p.yOffset = std::clamp(p.yOffset, 0.0, 1.0);
        p.yOffsetMax = std::clamp(p.yOffsetMax, 0.0, 1.0);
        if (p.yOffsetMax < p.yOffset)
            std::swap(p.yOffset, p.yOffsetMax);
        if (p.classId >= 0)
            cfg.classAimPoints.push_back(p);
    }

    cfg.pid.kpX = flat.kpX;
    cfg.pid.kpY = flat.kpY;
    cfg.pid.kiX = flat.kiX;
    cfg.pid.kiY = flat.kiY;
    cfg.pid.kdX = flat.kdX;
    cfg.pid.kdY = flat.kdY;
    cfg.pid.tauUnwindSec = flat.tauUnwindSec;
    cfg.pid.tauDerivSec = flat.tauDerivSec;
    cfg.pid.iMax = flat.iMax;
    cfg.pid.maxOutputCounts = flat.maxOutputCounts;
    cfg.pid.pFullScalePx = flat.pFullScalePx;
    cfg.pid.kPxPerCount = flat.kPxPerCount;
    cfg.pid.inflightBeta = flat.inflightBeta;
    cfg.pid.deadTimeMs = (flat.inflightDeadTimeMs > 0.0) ? flat.inflightDeadTimeMs : control::kLoopDeadTimeMs;

    cfg.predictor.leadMs = flat.predictLeadMs;
    cfg.predictor.maxVelocityPxPerSec = flat.predictMaxVelocityPxPerSec;
    cfg.predictor.maxLeadRatio = flat.predictMaxLeadRatio;

    cfg.requireFreshDetection = true;
    cfg.requireFreshCrosshair = true;

    return cfg;
}

FlatConfig flattenProfile(const HotkeyProfile& hk, int detectionResolution,
                          const std::vector<ClassFilterState>& classFilters,
                          const Config& globalConfig,
                          bool scopeEngaged)
{
    FlatConfig flat;

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
    flat.kPxPerCount = cp.k_px_per_count;
    flat.inflightBeta = cp.inflight_beta;
    flat.inflightDeadTimeMs = cp.inflight_dead_time_ms;
    flat.predictLeadMs = cp.predict_lead_ms;
    flat.predictMaxVelocityPxPerSec = cp.predict_max_velocity_px_s;
    flat.predictMaxLeadRatio = cp.predict_max_lead_ratio;
    flat.randomSeed = cp.random_seed;

    flat.yOffset = hk.ctl_y_offset;
    flat.yOffsetMax = hk.ctl_y_offset_max;

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
                                        static_cast<double>(ac.y_offset_max) });
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
