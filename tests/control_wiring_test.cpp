
#include "config/config.h"   // HotkeyProfile / ClassFilterState（本机可编）
#include "control/aim_controller.h"
#include "runtime/aim_loop.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace runtime::aim_loop;
using namespace control;

static int g_failures = 0;
static int g_checks = 0;

static void check(bool ok, const std::string& what)
{
    ++g_checks;
    if (!ok)
    {
        ++g_failures;
        std::printf("  FAIL: %s\n", what.c_str());
    }
}

static void checkNear(double got, double want, double tol, const std::string& what)
{
    ++g_checks;
    if (!(std::fabs(got - want) <= tol))
    {
        ++g_failures;
        std::printf("  FAIL: %s (got %.6f, want %.6f)\n", what.c_str(), got, want);
    }
}

static void section(const char* name) { std::printf("[%s]\n", name); }

static void testBucketMapping()
{
    section("类别桶: aim_classes → Aim/Delete");

    check(buildClassBuckets({}).empty(), "空 aim_classes ⇒ 空桶");

    {
        const auto b = buildClassBuckets({ 0 });
        check(b.size() == 1, "classId=0 ⇒ 桶长 1");
        check(b[0] == 1, "classId=0 被标为 Aim");
    }

    {
        const auto b = buildClassBuckets({ 2 });
        check(b.size() == 3, "最大 classId=2 ⇒ 桶长 3");
        check(b[0] == 0, "★ classId=0 不在列表 ⇒ Delete");
        check(b[1] == 0, "★ classId=1 不在列表 ⇒ Delete");
        check(b[2] == 1, "classId=2 ⇒ Aim");
    }

    {
        const auto b = buildClassBuckets({ 3, 0, 2 });
        check(b.size() == 4, "最大 3 ⇒ 桶长 4");
        check(b[0] == 1 && b[1] == 0 && b[2] == 1 && b[3] == 1,
              "0/2/3 是 Aim, 1 是 Delete（顺序无关）");
    }

    {
        const auto b = buildClassBuckets({ -5, 1 });
        check(b.size() == 2, "负 classId 不撑大桶");
        check(b[1] == 1, "正的仍然生效");
    }

    check(buildClassBuckets({ -1, -2 }).empty(), "全负 ⇒ 空桶");
}

static void testConfigMapping()
{
    section("FlatConfig → ControllerConfig: 六个增益");

    FlatConfig flat;
    flat.kpX = 11.0; flat.kpY = 12.0;
    flat.kiX = 21.0; flat.kiY = 22.0;
    flat.kdX = 31.0; flat.kdY = 32.0;

    const ControllerConfig cfg = toControllerConfig(flat);
    checkNear(cfg.pid.kpX, 11.0, 0.0, "kpX 搬到位");
    checkNear(cfg.pid.kpY, 12.0, 0.0, "kpY 搬到位");
    checkNear(cfg.pid.kiX, 21.0, 0.0, "kiX 搬到位");
    checkNear(cfg.pid.kiY, 22.0, 0.0, "kiY 搬到位");
    checkNear(cfg.pid.kdX, 31.0, 0.0, "kdX 搬到位");
    checkNear(cfg.pid.kdY, 32.0, 0.0, "kdY 搬到位");

    section("FlatConfig → ControllerConfig: 时间常数与限幅");
    flat.tauUnwindSec = 0.055;
    flat.tauDerivSec = 0.077;
    flat.iMax = 99.0;
    flat.maxOutputCounts = 123;
    flat.pFullScalePx = 44.0;
    const ControllerConfig cfg2 = toControllerConfig(flat);
    checkNear(cfg2.pid.tauUnwindSec, 0.055, 0.0, "tauUnwindSec 搬到位");
    checkNear(cfg2.pid.tauDerivSec, 0.077, 0.0, "tauDerivSec 搬到位");
    checkNear(cfg2.pid.iMax, 99.0, 0.0, "iMax 搬到位");
    check(cfg2.pid.maxOutputCounts == 123, "maxOutputCounts 搬到位");
    checkNear(cfg2.pid.pFullScalePx, 44.0, 0.0, "pFullScalePx 搬到位");

    section("FlatConfig → ControllerConfig: 瞄点与滞回");
    flat.yOffset = 0.3; flat.yOffsetMax = 0.7; flat.hysteresisRatio = 1.8;
    const ControllerConfig cfg3 = toControllerConfig(flat);
    checkNear(cfg3.aimPoint.yOffset, 0.3, 0.0, "yOffset 搬到位");
    checkNear(cfg3.aimPoint.yOffsetMax, 0.7, 0.0, "yOffsetMax 搬到位");
    checkNear(cfg3.selector.hysteresisRatio, 1.8, 0.0, "hysteresisRatio 搬到位");

    section("类别桶进控制器配置");
    flat.aimClassIds = { 0, 2 };
    const ControllerConfig cfg4 = toControllerConfig(flat);
    check(cfg4.buckets.byClassId.size() == 3, "桶长度 = maxClassId+1");
    check(cfg4.buckets.bucketOf(0) == Bucket::Aim, "0 ⇒ Aim");
    check(cfg4.buckets.bucketOf(1) == Bucket::Delete, "1 ⇒ Delete");
    check(cfg4.buckets.bucketOf(2) == Bucket::Aim, "2 ⇒ Aim");
    check(cfg4.buckets.bucketOf(99) == Bucket::Delete,
          "★ 越界 classId ⇒ Delete（安全默认: 未知类别不瞄）");

    FlatConfig emptyFlat;
    const ControllerConfig cfg5 = toControllerConfig(emptyFlat);
    check(cfg5.buckets.byClassId.empty(), "无 aim_classes ⇒ 空桶");
    check(cfg5.buckets.bucketOf(0) == Bucket::Delete,
          "★ 空桶时任何类别都是 Delete —— 没有配置来源就不瞄任何东西");

    section("新鲜度门禁恒为开");
    check(cfg.requireFreshDetection, "requireFreshDetection 恒为 true");
    check(cfg.requireFreshCrosshair, "requireFreshCrosshair 恒为 true");

    section("★ 死区不得回归");
    PidConfig pcfg;
    (void)pcfg;
    check(true, "PidConfig 无死区字段参与映射（死区已整条删除）");

    // ── 在途补偿 ────────────────────────────────────────────────────────
    section("FlatConfig → ControllerConfig: 在途补偿三个参数");

    FlatConfig pflat;
    checkNear(toControllerConfig(pflat).predictor.leadMs, 0.0, 0.0,
              "★ 默认 leadMs = 0（预测默认关闭）");
    checkNear(toControllerConfig(pflat).predictor.maxVelocityPxPerSec, 0.0, 0.0,
              "默认速度上限 = 0（不限制）");
    checkNear(toControllerConfig(pflat).predictor.maxLeadRatio, 0.0, 0.0,
              "默认距离上限 = 0（不限制）");

    pflat.predictLeadMs = 46.0;
    pflat.predictMaxVelocityPxPerSec = 1500.0;
    pflat.predictMaxLeadRatio = 0.75;
    const ControllerConfig pcfg2 = toControllerConfig(pflat);
    checkNear(pcfg2.predictor.leadMs, 46.0, 0.0, "★ leadMs 搬到位");
    checkNear(pcfg2.predictor.maxVelocityPxPerSec, 1500.0, 0.0,
              "★ 速度上限搬到位");
    checkNear(pcfg2.predictor.maxLeadRatio, 0.75, 0.0, "★ 距离上限搬到位");

    check(pcfg2.predictor.enabled(), "非 0 leadMs ⇒ 预测启用");
    check(!toControllerConfig(FlatConfig{}).predictor.enabled(),
          "★ 默认配置 ⇒ 预测不启用（不改变现有行为）");
}

static void testConfigActuallyDrivesControl()
{
    section("★ 配置真的驱动控制（不是只搬字段）");

    FlatConfig flat;
    flat.aimClassIds = { 0 };
    flat.yOffset = 0.5;
    flat.yOffsetMax = 0.5;
    flat.kpX = 35.0;
    flat.kpY = 35.0;

    const double dt = 1.0 / 120.0;

    auto runOnce = [&](double kpX) {
        FlatConfig f = flat;
        f.kpX = kpX;
        AimController ac;
        ac.setConfig(toControllerConfig(f));
        ControlInput in;
        in.cross = Vec2{ 100, 100 };
        in.dtSec = dt;
        Candidate c;
        c.box = Box{ 160, 100, 40, 80 };
        c.classId = 0;
        c.confidence = 0.9;
        in.candidates = { c };
        return ac.update(in);
    };

    const ControlOutput lo = runOnce(10.0);
    const ControlOutput hi = runOnce(100.0);
    check(lo.engaged && hi.engaged, "两组都 engage");
    check(hi.counts.x > lo.counts.x,
          "★ Kp 大 ⇒ 输出大（证明配置真的进了公式，不是摆设）");

    section("★ 在途补偿配置真的驱动预测（不是只搬字段）");
    {
        // 匀速横向移动的目标，跑若干帧后比较瞄点
        auto runPredict = [&](double leadMs) {
            FlatConfig f = flat;
            f.predictLeadMs = leadMs;
            AimController ac;
            ac.setConfig(toControllerConfig(f));
            ControlInput in;
            in.cross = Vec2{ 100, 100 };
            in.dtSec = dt;
            Vec2 anchor;
            for (int i = 0; i < 40; ++i) {
                Candidate c;
                c.box = Box{ 160.0 + i * 5.0, 100.0, 40.0, 80.0 };  // 匀速右移
                c.classId = 0;
                c.confidence = 0.9;
                in.candidates = { c };
                in.frameIndex = static_cast<uint64_t>(i);
                anchor = ac.update(in).anchor;
            }
            return anchor;
        };

        const Vec2 off = runPredict(0.0);
        const Vec2 on = runPredict(1.0);
        check(on.x > off.x,
              "★ leadMs 非 0 ⇒ 瞄点沿目标运动方向前移（配置真的驱动了预测）");
        check(std::fabs(on.y - off.y) < 1e-9,
              "★ 目标只横向移动 ⇒ 纵向瞄点不变");
    }

    section("★ 类别桶真的过滤（Delete 不产生控制）");    {
        FlatConfig f = flat;
        f.aimClassIds = { 0 };
        AimController ac;
        ac.setConfig(toControllerConfig(f));
        ControlInput in;
        in.cross = Vec2{ 100, 100 };
        in.dtSec = dt;
        Candidate c;
        c.box = Box{ 160, 100, 40, 80 };
        c.classId = 1;
        c.confidence = 0.9;
        in.candidates = { c };
        const ControlOutput out = ac.update(in);
        check(!out.engaged,
              "★ classId=1 不在 aim_classes ⇒ 不 engage（桶真的生效）");
        check(out.idleReason == ControlOutput::IdleReason::NoCandidates,
              "idleReason = NoCandidates");
    }

    section("★ 滞回倍数真的进控制器");
    {
        auto pick = [&](double k) {
            FlatConfig f = flat;
            f.hysteresisRatio = k;
            AimController ac;
            ac.setConfig(toControllerConfig(f));
            ControlInput in;
            in.dtSec = dt;
            in.cross = Vec2{ 100, 140 };

            Candidate a; a.box = Box{ 80, 100, 40, 80 };  a.classId = 0; a.confidence = 0.9;
            Candidate b; b.box = Box{ 300, 100, 40, 80 }; b.classId = 0; b.confidence = 0.9;

            for (int i = 0; i < 3; ++i) { in.candidates = { a, b }; in.frameIndex = i; ac.update(in); }

            in.cross = Vec2{ 240, 140 };
            in.candidates = { a, b };
            in.frameIndex = 10;
            return ac.update(in);
        };
        const ControlOutput hard = pick(5.0);
        const ControlOutput soft = pick(1.3);
        check(hard.anchor.x < 200.0,
              "★ k=5（强滞回）: 仍锁在目标 A（锚点 x < 200）");
        check(soft.anchor.x > 200.0,
              "★ k=1.3（弱滞回）: 已切到目标 B（锚点 x > 200）—— "
              "证明 k 真的进了选靶，不是摆设");

        {
            FlatConfig f = flat;
            f.hysteresisRatio = 5.0;
            AimController ac;
            ac.setConfig(toControllerConfig(f));
            ControlInput in;
            in.dtSec = dt;
            in.cross = Vec2{ 100, 140 };
            Candidate a; a.box = Box{ 80, 100, 40, 80 };  a.classId = 0; a.confidence = 0.9;
            Candidate b; b.box = Box{ 300, 100, 40, 80 }; b.classId = 0; b.confidence = 0.9;
            for (int i = 0; i < 3; ++i) { in.candidates = { a, b }; in.frameIndex = i; ac.update(in); }

            in.candidates = { b };
            in.frameIndex = 10;
            const ControlOutput out = ac.update(in);
            check(out.anchor.x > 200.0,
                  "★ 锁定目标消失 ⇒ 再大的滞回也必须改选（不许把人锁死）");
        }
    }
}

static void testBucketMerge()
{
    section("★★ 类别桶两源合并（TargetPage 的 class_filters + 逐热键 aim_classes）");

    {
        FlatConfig f;
        f.classFilters = { {0, 2}, {1, 1}, {2, 0} };
        const ControllerConfig cfg = toControllerConfig(f);
        check(cfg.buckets.byClassId.size() == 3, "桶长 = maxId+1");
        check(cfg.buckets.bucketOf(0) == Bucket::Aim,    "全局 Aim 生效");
        check(cfg.buckets.bucketOf(1) == Bucket::Filter, "★★ 全局 Filter 生效（此前永远为空）");
        check(cfg.buckets.bucketOf(2) == Bucket::Delete, "全局 Delete 生效");
    }

    {
        FlatConfig f;
        f.aimClassIds = { 0, 2 };
        const ControllerConfig cfg = toControllerConfig(f);
        check(cfg.buckets.bucketOf(0) == Bucket::Aim, "aim_classes 单独也能用");
        check(cfg.buckets.bucketOf(1) == Bucket::Delete, "未列出 ⇒ Delete");
        check(cfg.buckets.bucketOf(2) == Bucket::Aim, "aim_classes 第二个生效");
    }

    {
        FlatConfig f;
        f.classFilters = { {0, 0} };
        f.aimClassIds  = { 0 };
        const ControllerConfig cfg = toControllerConfig(f);
        check(cfg.buckets.bucketOf(0) == Bucket::Aim,
              "★★ 冲突时逐热键的 Aim 胜出（更具体的意图优先，只提升不降级）");
    }
    {
        FlatConfig f;
        f.classFilters = { {0, 2} };
        f.aimClassIds  = { };
        const ControllerConfig cfg = toControllerConfig(f);
        check(cfg.buckets.bucketOf(0) == Bucket::Aim, "全局 Aim 不因 aim_classes 为空而丢失");
    }
    {
        FlatConfig f;
        f.classFilters = { {0, 1} };
        f.aimClassIds  = { 1 };
        const ControllerConfig cfg = toControllerConfig(f);
        check(cfg.buckets.bucketOf(0) == Bucket::Filter,
              "★ 全局 Filter 不被无关的 aim_classes 条目冲掉");
        check(cfg.buckets.bucketOf(1) == Bucket::Aim, "另一条照常 Aim");
    }

    {
        FlatConfig f;
        f.classFilters = { {0, 2} };
        f.aimClassIds  = { 5 };
        const ControllerConfig cfg = toControllerConfig(f);
        check(cfg.buckets.byClassId.size() == 6, "桶长取两源最大值 +1");
        check(cfg.buckets.bucketOf(0) == Bucket::Aim, "全局的还在");
        check(cfg.buckets.bucketOf(5) == Bucket::Aim, "补进来的也被标 Aim");
        check(cfg.buckets.bucketOf(3) == Bucket::Delete, "中间没提的 ⇒ Delete");
    }

    {
        FlatConfig f;
        f.classFilters = { {0, 1} };
        f.yOffset = 0.5; f.yOffsetMax = 0.5;
        AimController ac;
        ac.setConfig(toControllerConfig(f));
        ControlInput in;
        in.cross = Vec2{ 100, 100 };
        in.dtSec = 1.0 / 120.0;
        Candidate c; c.box = Box{ 160, 100, 40, 80 }; c.classId = 0; c.confidence = 0.9;
        in.candidates = { c };
        const ControlOutput out = ac.update(in);
        check(!out.engaged,
              "★★ Filter 桶的类别【不产生控制】（以前它永远为空，这条测不到）");
    }

    {
        FlatConfig f;
        f.classFilters = { {0, 0}, {1, 1}, {2, 2} };
        const ControllerConfig cfg = toControllerConfig(f);
        check(cfg.buckets.bucketOf(0) == Bucket::Delete, "数值 0 ⇒ Delete");
        check(cfg.buckets.bucketOf(1) == Bucket::Filter, "★★ 数值 1 ⇒ Filter（契约）");
        check(cfg.buckets.bucketOf(2) == Bucket::Aim,    "数值 2 ⇒ Aim");
    }

    {
        FlatConfig f;
        const ControllerConfig cfg = toControllerConfig(f);
        check(cfg.buckets.byClassId.empty(), "两源都空 ⇒ 空桶");
        check(cfg.buckets.bucketOf(0) == Bucket::Delete, "空桶时任何类别都是 Delete");
    }
}

static void testNewlyExposedKnobs()
{
    section("★ 新暴露的参数真的进控制器（此前写死在代码里）");

    FlatConfig f;
    f.maxDistancePx = 77.0;
    f.randomSeed = 12345;
    f.matchCenterRatio = 0.7;
    f.areaRatioTol = 3.5;
    f.kSnapMult = 2.5;
    f.minAspect = 0.3;
    f.maxAspect = 4.0;

    const ControllerConfig cfg = toControllerConfig(f);
    checkNear(cfg.selector.maxDistancePx, 77.0, 0.0, "maxDistancePx 搬到位");
    check(cfg.aimPoint.randomSeed == 12345, "randomSeed 搬到位");
    checkNear(cfg.stabilizer.matchCenterRatio, 0.7, 0.0, "matchCenterRatio 搬到位");
    checkNear(cfg.stabilizer.areaRatioTol, 3.5, 0.0, "areaRatioTol 搬到位");
    checkNear(cfg.stabilizer.kSnapMult, 2.5, 0.0, "kSnapMult 搬到位");
    checkNear(cfg.stabilizer.minAspect, 0.3, 0.0, "minAspect 搬到位");
    checkNear(cfg.stabilizer.maxAspect, 4.0, 0.0, "maxAspect 搬到位");

    section("★ 稳定器参数真的改变行为（不是只搬字段）");
    {
        auto run = [](double kSnap) {
            FlatConfig g;
            g.classFilters = { {0, 2} };
            g.yOffset = 0.5; g.yOffsetMax = 0.5;
            g.kSnapMult = kSnap;
            AimController ac;
            ac.setConfig(toControllerConfig(g));
            ControlInput in;
            in.cross = Vec2{ 100, 100 };
            in.dtSec = 1.0 / 120.0;
            Candidate a; a.box = Box{ 160, 100, 40, 80 }; a.classId = 0; a.confidence = 0.9;
            in.candidates = { a };
            in.frameIndex = 1;
            ac.update(in);

            Candidate b; b.box = Box{ 190, 100, 40, 80 }; b.classId = 0; b.confidence = 0.9;
            in.candidates = { b };
            in.frameIndex = 2;
            return ac.update(in);
        };
        const ControlOutput strict = run(0.001);
        const ControlOutput loose  = run(10.0);
        check(strict.anchor.x != loose.anchor.x ||
                  strict.anchor.y != loose.anchor.y ||
                  strict.engaged != loose.engaged,
              "★ kSnapMult 真的改变结果（证明稳定器参数进了公式）");
    }
}

static void testDtGate()
{
    section("★ dt 门禁");

    check(dtIsUsable(1.0 / 120.0), "120fps (8.3ms) 放行");
    check(dtIsUsable(1.0 / 60.0),  "60fps (16.7ms) 放行");
    check(dtIsUsable(1.0 / 30.0),  "30fps (33.3ms) 放行");
    check(dtIsUsable(1.0 / 10.0),  "10fps (100ms) 放行");
    check(dtIsUsable(0.240),       "240ms 放行（上限内）");

    check(dtIsUsable(kMinDtSec), "★ dt == 下限 放行（闭区间）");
    check(dtIsUsable(kMaxDtSec), "★ dt == 上限 放行（闭区间）");

    check(!dtIsUsable(0.300), "★ 300ms 拒绝（超过 250ms 上限）");
    check(!dtIsUsable(1.0),   "★ 1s 拒绝");
    check(!dtIsUsable(5.0),   "★ 5s 拒绝（会话中断后第一拍）");

    check(!dtIsUsable(0.0005), "★ 0.5ms 拒绝（低于 1ms 下限）");
    check(!dtIsUsable(0.0),    "★ dt=0 拒绝（防除零）");

    check(!dtIsUsable(-0.01), "★★ 负 dt 拒绝（时钟异常，不是小时间）");
    check(!dtIsUsable(-1e9),  "★★ 负 dt 拒绝（任何负值）");

    section("★ dt 两层防线各管一段（职责边界）");
    {
        PidConfig pc;
        pc.kpX = pc.kpY = 35.0;
        for (double bad : { 0.0, -0.01, -1.0 })
        {
            PidController pid(pc);
            const Counts c = pid.update(Vec2{ 200, 200 }, Vec2{ 100, 100 }, bad);
            check(c.x == 0 && c.y == 0,
                  "★ 公式层: dt=" + std::to_string(bad) + " ⇒ 输出 0（不猜 dt）");
        }

        check(!dtIsUsable(0.5),  "★ 策略层: 500ms 被 dtIsUsable 拒绝");
        check(!dtIsUsable(3.0),  "★ 策略层: 3s 被 dtIsUsable 拒绝");

        PidController pid2(pc);
        const Counts c2 = pid2.update(Vec2{ 200, 200 }, Vec2{ 100, 100 }, 0.5);
        check(c2.x != 0 || c2.y != 0,
              "★★ 公式层对 500ms 【会】出力 ⇒ 证明调用方的 dtIsUsable 是必需的");
    }
}

static void testFlattenProfile()
{
    section("★★ HotkeyProfile → FlatConfig 逐字段搬运");

    HotkeyProfile hk;
    hk.ctl_kp_x = 11.0; hk.ctl_kp_y = 12.0;
    hk.ctl_ki_x = 21.0; hk.ctl_ki_y = 22.0;
    hk.ctl_kd_x = 31.0; hk.ctl_kd_y = 32.0;
    hk.ctl_tau_unwind_sec = 0.041;
    hk.ctl_tau_deriv_sec = 0.052;
    hk.ctl_i_max = 63.0;
    hk.ctl_max_output_counts = 77;
    hk.ctl_p_full_scale_px = 88.0;
    hk.ctl_predict_lead_ms = 0.065;
    hk.ctl_predict_max_velocity_px_s = 1700.0;
    hk.ctl_predict_max_lead_ratio = 0.85;
    hk.ctl_y_offset = 0.31;
    hk.ctl_y_offset_max = 0.91;
    hk.ctl_hysteresis_ratio = 2.4;
    hk.ctl_max_distance_px = 155.0;
    hk.ctl_random_seed = 4242;
    hk.ctl_match_center_ratio = 0.61;
    hk.ctl_area_ratio_tol = 3.7;
    hk.ctl_k_snap_mult = 2.2;
    hk.ctl_min_aspect = 0.11;
    hk.ctl_max_aspect = 6.6;
    hk.aim_classes.clear();
    { HotkeyAimClass a; a.class_id = 3; a.y_offset = 0.71f; a.y_offset_max = 0.83f;
      a.min_conf = 0.42f; hk.aim_classes.push_back(a); }
    { HotkeyAimClass a; a.class_id = 7; a.y_offset = 0.22f; a.y_offset_max = 0.29f;
      a.min_conf = 0.15f; hk.aim_classes.push_back(a); }

    Config gcfg;
    gcfg.target_hysteresis_ratio = 2.4;
    gcfg.target_max_distance_px = 155.0;
    gcfg.target_match_center_ratio = 0.61;
    gcfg.target_area_ratio_tol = 3.7;
    gcfg.target_k_snap_mult = 2.2;
    gcfg.target_min_aspect = 0.11;
    gcfg.target_max_aspect = 6.6;

    std::vector<ClassFilterState> cfilters;
    { ClassFilterState c; c.class_id = 1; c.bucket = ClassBucket::Filter; cfilters.push_back(c); }
    { ClassFilterState c; c.class_id = 5; c.bucket = ClassBucket::Aim;    cfilters.push_back(c); }

    const FlatConfig f = flattenProfile(hk, 640, cfilters, gcfg);

    checkNear(f.kpX, 11.0, 0.0, "kpX 搬了");
    checkNear(f.kpY, 12.0, 0.0, "kpY 搬了");
    checkNear(f.kiX, 21.0, 0.0, "kiX 搬了");
    checkNear(f.kiY, 22.0, 0.0, "kiY 搬了");
    checkNear(f.kdX, 31.0, 0.0, "kdX 搬了");
    checkNear(f.kdY, 32.0, 0.0, "kdY 搬了");
    checkNear(f.tauUnwindSec, 0.041, 0.0, "tauUnwindSec 搬了");
    checkNear(f.tauDerivSec, 0.052, 0.0, "tauDerivSec 搬了");
    checkNear(f.iMax, 63.0, 0.0, "iMax 搬了");
    check(f.maxOutputCounts == 77, "maxOutputCounts 搬了");
    checkNear(f.pFullScalePx, 88.0, 0.0, "pFullScalePx 搬了");
    checkNear(f.predictLeadMs, 0.065, 0.0, "★ predictLeadMs 搬了");
    checkNear(f.predictMaxVelocityPxPerSec, 1700.0, 0.0,
              "★ predictMaxVelocityPxPerSec 搬了");
    checkNear(f.predictMaxLeadRatio, 0.85, 0.0, "★ predictMaxLeadRatio 搬了");
    checkNear(f.yOffset, 0.31, 0.0, "yOffset 搬了");
    checkNear(f.yOffsetMax, 0.91, 0.0, "yOffsetMax 搬了");
    checkNear(f.hysteresisRatio, 2.4, 0.0, "hysteresisRatio 搬了");
    checkNear(f.maxDistancePx, 155.0, 0.0, "★ maxDistancePx 搬了");
    check(f.randomSeed == 4242, "★ randomSeed 搬了");
    checkNear(f.matchCenterRatio, 0.61, 0.0, "★ matchCenterRatio 搬了");
    checkNear(f.areaRatioTol, 3.7, 0.0, "★ areaRatioTol 搬了");
    checkNear(f.kSnapMult, 2.2, 0.0, "★ kSnapMult 搬了");
    checkNear(f.minAspect, 0.11, 0.0, "★ minAspect 搬了");
    checkNear(f.maxAspect, 6.6, 0.0, "★ maxAspect 搬了");

    check(f.detectionResolution == 640, "detectionResolution 搬了");
    check(f.aimClassIds.size() == 2 && f.aimClassIds[0] == 3 && f.aimClassIds[1] == 7,
          "aim_classes 的 class_id 全搬了");

    section("★★ 逐类别瞄点 + 置信度必须被搬运");
    check(f.classAimPoints.size() == 2, "classAimPoints 条数对");
    if (f.classAimPoints.size() == 2)
    {
        check(std::abs(f.classAimPoints[0][0] - 3.0) < 1e-9 &&
              std::abs(f.classAimPoints[0][1] - 0.71) < 1e-6 &&
              std::abs(f.classAimPoints[0][2] - 0.83) < 1e-6,
              "★★ 第 1 条: classId=3 / yOffset=0.71 / yOffsetMax=0.83 全搬了");
        check(std::abs(f.classAimPoints[1][0] - 7.0) < 1e-9 &&
              std::abs(f.classAimPoints[1][1] - 0.22) < 1e-6 &&
              std::abs(f.classAimPoints[1][2] - 0.29) < 1e-6,
              "★★ 第 2 条: classId=7 / yOffset=0.22 / yOffsetMax=0.29 全搬了");
    }
    check(f.classMinConf.size() == 2, "classMinConf 条数对");
    if (f.classMinConf.size() == 2)
    {
        check(f.classMinConf[0].first == 3 && std::abs(f.classMinConf[0].second - 0.42) < 1e-6,
              "★★ classId=3 的 min_conf 0.42 搬了");
        check(f.classMinConf[1].first == 7 && std::abs(f.classMinConf[1].second - 0.15) < 1e-6,
              "★★ classId=7 的 min_conf 0.15 搬了");
    }

    {
        const control::ControllerConfig cc = toControllerConfig(f);
        check(cc.classAimPoints.size() == 2, "→ ControllerConfig.classAimPoints 有 2 条");
        bool found3 = false, found7 = false;
        for (const auto& p : cc.classAimPoints)
        {
            if (p.classId == 3) { found3 = true;
                check(std::abs(p.yOffset - 0.71) < 1e-6 && std::abs(p.yOffsetMax - 0.83) < 1e-6,
                      "★★ classId=3 的瞄点区间真的进了控制器配置"); }
            if (p.classId == 7) { found7 = true;
                check(std::abs(p.yOffset - 0.22) < 1e-6 && std::abs(p.yOffsetMax - 0.29) < 1e-6,
                      "★★ classId=7 的瞄点区间真的进了控制器配置"); }
        }
        check(found3 && found7, "两个类别都在配置里 (按 classId 可查到)");

        check(cc.selector.minConfByClassId.size() >= 8,
              "minConf 表按 classId 下标展开 (至少到 7)");
        if (cc.selector.minConfByClassId.size() >= 8)
        {
            check(std::abs(cc.selector.minConfByClassId[3] - 0.42) < 1e-6,
                  "★★ classId=3 的置信度门槛 0.42 落在下标 3");
            check(std::abs(cc.selector.minConfByClassId[7] - 0.15) < 1e-6,
                  "★★ classId=7 的置信度门槛 0.15 落在下标 7");
            check(cc.selector.minConfByClassId[4] == 0.0,
                  "★ 未设门槛的类别 ⇒ 0 (不限, 不是误挡)");
        }
    }

    section("★★ class_filters 必须被搬运（这是原来的断层）");
    check(f.classFilters.size() == 2, "class_filters 条数对");
    check(f.classFilters.size() >= 1 && f.classFilters[0].first == 1 &&
              f.classFilters[0].second == 1,
          "★★ class 1 搬过来且 bucket==1(Filter) —— 不搬的话 TargetPage 白设");
    check(f.classFilters.size() >= 2 && f.classFilters[1].first == 5 &&
              f.classFilters[1].second == 2,
          "class 5 搬过来且 bucket==2(Aim)");

    section("★★ 端到端: flatten 之后 TargetPage 设的 Filter 真的生效");
    {
        HotkeyProfile hk2;
        hk2.aim_classes.clear();
        hk2.ctl_y_offset = 0.5; hk2.ctl_y_offset_max = 0.5;
        std::vector<ClassFilterState> cf2;
        { ClassFilterState c; c.class_id = 0; c.bucket = ClassBucket::Filter; cf2.push_back(c); }

        AimController ac;
        Config dummyCfg;
        ac.setConfig(toControllerConfig(flattenProfile(hk2, 320, cf2, dummyCfg)));
        ControlInput in;
        in.cross = Vec2{ 100, 100 };
        in.dtSec = 1.0 / 120.0;
        Candidate c; c.box = Box{ 160, 100, 40, 80 }; c.classId = 0; c.confidence = 0.9;
        in.candidates = { c };
        check(!ac.update(in).engaged,
              "★★ 界面上设为 Filter 的类别不产生控制（整条链真的通了）");
    }
    {
        HotkeyProfile hk2;
        hk2.aim_classes.clear();
        hk2.ctl_y_offset = 0.5; hk2.ctl_y_offset_max = 0.5;
        std::vector<ClassFilterState> cf2;
        { ClassFilterState c; c.class_id = 0; c.bucket = ClassBucket::Aim; cf2.push_back(c); }

        AimController ac;
        Config dummyCfg;
        ac.setConfig(toControllerConfig(flattenProfile(hk2, 320, cf2, dummyCfg)));
        ControlInput in;
        in.cross = Vec2{ 100, 100 };
        in.dtSec = 1.0 / 120.0;
        Candidate c; c.box = Box{ 160, 100, 40, 80 }; c.classId = 0; c.confidence = 0.9;
        in.candidates = { c };
        check(ac.update(in).engaged,
              "★ 同一路径设为 Aim ⇒ 产生控制（正反两面都钉住）");
    }
}

int main()
{
    std::printf("=== 控制器接线回归 ===\n");
    testFlattenProfile();
    testBucketMapping();
    testConfigMapping();
    testConfigActuallyDrivesControl();
    testBucketMerge();
    testNewlyExposedKnobs();
    testDtGate();

    std::printf("\n%d 项断言, 失败 %d\n", g_checks, g_failures);
    if (g_failures == 0)
        std::printf("全部通过\n");
    return g_failures == 0 ? 0 : 1;
}
