
// 在途补偿（预测提前量）回归测试。
//
// 钉的核心不变量（按重要性排序）：
//   1. lead_ms = 0 ⇒ 与【完全没有这个功能】逐帧完全一致（回归底线，可一键退回）
//   2. 提前时间 = lead_ms，且【不做任何自动累加】（来源单一，不重复计算）
//   3. 速度上限 ⇒ 钳住大小、保留方向
//   4. 距离上限 ⇒ 按【目标框对角线倍数】硬顶
//   5. 目标跳变/Snap ⇒ 预测状态被清空

#include "control/aim_controller.h"
#include "control/predictor.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

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
        std::printf("  FAIL: %s (got %.6f, want %.6f ± %.6f)\n",
                    what.c_str(), got, want, tol);
    }
}

static void section(const char* name)
{
    std::printf("[%s]\n", name);
}

// ── 直接测预测器 ─────────────────────────────────────────────────────────

static void testPredictorDisabled()
{
    section("① lead_ms=0 是总开关（关闭时另两个参数不读取）");

    PredictorState st;
    PredictorConfig cfg;   // leadMs = 0
    const Vec2 center{ 100.0, 200.0 };
    const Vec2 vel{ 500.0, 0.0 };
    const Box box{ 90.0, 190.0, 20.0, 20.0 };

    PredictorResult r = predictAnchor(center, vel, box, cfg, st);
    check(!r.applied, "★ 关闭时不预测");
    check(r.idleReason == PredictorResult::IdleReason::Disabled, "idleReason = Disabled");
    checkNear(r.predictedCenter.x, center.x, 1e-12, "★ 关闭时中心原样返回 x");
    checkNear(r.predictedCenter.y, center.y, 1e-12, "★ 关闭时中心原样返回 y");
    checkNear(r.lead.x, 0.0, 1e-12, "关闭时推进量为 0");
    checkNear(r.lead.y, 0.0, 1e-12, "关闭时推进量为 0");

    // ★ 即使把另外两个参数设成"看起来很凶"的值，关闭就是关闭
    cfg.maxVelocityPxPerSec = 1.0;
    cfg.maxLeadRatio = 0.001;
    PredictorResult r2 = predictAnchor(center, vel, box, cfg, st);
    check(!r2.applied, "★ 关闭时速度/距离上限也不生效（总开关优先）");
    checkNear(r2.predictedCenter.x, center.x, 1e-12, "关闭时中心仍原样返回");
}

static void testPredictorBasic()
{
    section("② 基础预测：提前时间只用 lead_ms，不做任何自动累加");

    const Vec2 center{ 100.0, 200.0 };

    // 速度 1000 px/s，lead_ms = 50 ⇒ 应推进 50px
    {
        PredictorState st;
        PredictorConfig cfg;
        cfg.leadMs = 50.0;
        PredictorResult r = predictAnchor(center, Vec2{ 1000.0, 0.0 },
                                          Box{ 90, 190, 20, 20 }, cfg, st);
        check(r.applied, "★ 有速度+有提前时间 ⇒ 预测生效");
        checkNear(r.leadSec, 0.050, 1e-12, "★ leadSec 恰为 50ms（没有额外累加）");
        checkNear(r.lead.x, 50.0, 1e-9, "★ 推进 1000 × 0.05 = 50px");
        checkNear(r.predictedCenter.x, 150.0, 1e-9, "★ 预测中心 = 100 + 50");
        checkNear(r.predictedCenter.y, 200.0, 1e-9, "y 方向速度为 0 ⇒ 不推进");
    }

    // ★ 提前时间与 lead_ms 严格成正比，且不掺入任何常量
    {
        PredictorState st;
        PredictorConfig cfg;
        cfg.leadMs = 100.0;
        PredictorResult r = predictAnchor(center, Vec2{ 1000.0, 0.0 },
                                          Box{ 90, 190, 20, 20 }, cfg, st);
        checkNear(r.leadSec, 0.100, 1e-12, "★ lead_ms=100 ⇒ leadSec 恰 0.1s");
        checkNear(r.lead.x, 100.0, 1e-9, "★ 推进 1000 × 0.1 = 100px");
    }

    // 斜向速度：方向必须保住
    {
        PredictorState st;
        PredictorConfig cfg;
        cfg.leadMs = 100.0;
        PredictorResult r = predictAnchor(center, Vec2{ 300.0, 400.0 },
                                          Box{ 90, 190, 200, 200 }, cfg, st);
        checkNear(r.lead.x, 30.0, 1e-9, "斜向 x 分量");
        checkNear(r.lead.y, 40.0, 1e-9, "斜向 y 分量");
        checkNear(r.lead.norm(), 50.0, 1e-9, "斜向模长 500 × 0.1 = 50");
    }
}

static void testNoAutoAccumulation()
{
    section("③★ 提前时间不受任何外部注入影响（来源单一）");

    // 这是本次语义变更的核心：predictAnchor 不再接收 autoLatencyMs，
    // 因此编译期就不可能"替你加一段"。这里用同配置多次调用验证稳定。
    const Vec2 center{ 100.0, 200.0 };
    const Vec2 vel{ 1000.0, 0.0 };
    const Box box{ 90, 190, 20, 20 };

    PredictorConfig cfg;
    cfg.leadMs = 46.0;

    PredictorState st;
    const PredictorResult r1 = predictAnchor(center, vel, box, cfg, st);
    const PredictorResult r2 = predictAnchor(center, vel, box, cfg, st);

    checkNear(r1.lead.x, 46.0, 1e-9, "★ lead_ms=46 ⇒ 推进恰为 46px");
    checkNear(r2.lead.x, 46.0, 1e-9, "★ 重复调用结果稳定（无隐藏状态累积）");
    checkNear(r1.leadSec, 0.046, 1e-12, "★ leadSec 恰为 0.046（未被放大）");
}

static void testRateLimits()
{
    section("④⑤ 速度上限（钳住保方向）与距离上限（对角线倍数）");

    const Vec2 center{ 100.0, 200.0 };
    const Box box{ 90, 190, 30.0, 40.0 };   // 对角线 = 50

    // ── 速度上限：钳住大小、保留方向 ──
    {
        PredictorConfig cfg;
        cfg.leadMs = 50.0;
        cfg.maxVelocityPxPerSec = 1000.0;
        PredictorState st;
        PredictorResult r = predictAnchor(center, Vec2{ 5000.0, 0.0 }, box, cfg, st);
        check(r.applied, "限速后仍然预测（不是放弃）");
        check(r.velocityClamped, "★ 标记 velocityClamped");
        checkNear(r.rawVelocity.x, 5000.0, 1e-9, "原始速度被保留在遥测里");
        checkNear(r.clampedVelocity.x, 1000.0, 1e-9, "★ 速度被钳到上限");
        checkNear(r.lead.x, 50.0, 1e-9, "★ 推进按钳后速度算：1000 × 0.05 = 50px");
    }

    // ★ 钳住必须保方向 —— 斜向超速时，方向不能变
    {
        PredictorConfig cfg;
        cfg.leadMs = 100.0;
        cfg.maxVelocityPxPerSec = 500.0;
        PredictorState st;
        PredictorResult r = predictAnchor(center, Vec2{ 3000.0, 4000.0 }, box, cfg, st);
        check(r.velocityClamped, "斜向超速被钳");
        checkNear(r.clampedVelocity.norm(), 500.0, 1e-9, "★ 钳后模长 = 上限");
        checkNear(r.clampedVelocity.x, 300.0, 1e-9, "★ 方向保住的 x 分量 0.6×500");
        checkNear(r.clampedVelocity.y, 400.0, 1e-9, "★ 方向保住的 y 分量 0.8×500");
    }

    // 未超上限时不应触发钳制
    {
        PredictorConfig cfg;
        cfg.leadMs = 50.0;
        cfg.maxVelocityPxPerSec = 10000.0;
        PredictorState st;
        PredictorResult r = predictAnchor(center, Vec2{ 500.0, 0.0 }, box, cfg, st);
        check(!r.velocityClamped, "未超上限时不标记钳制");
        checkNear(r.lead.x, 25.0, 1e-9, "未超上限时按原始速度推进");
    }

    // ── 距离上限：对角线倍数 ──
    {
        PredictorConfig cfg;
        cfg.leadMs = 100.0;
        cfg.maxLeadRatio = 1.0;    // 最多提前一个对角线 = 50px
        PredictorState st;
        PredictorResult r = predictAnchor(center, Vec2{ 10000.0, 0.0 }, box, cfg, st);
        check(r.applied, "撞距离上限仍然预测");
        check(r.leadClamped, "★ 标记 leadClamped");
        checkNear(r.maxLeadPx, 50.0, 1e-9, "★ 上限 = 1.0 × 对角线 50px");
        checkNear(r.lead.norm(), 50.0, 1e-9, "★ 推进被顶到 50px");
    }

    // ★ 距离上限是【相对量】—— 同样的倍率，大框放得多、小框放得少
    {
        PredictorConfig cfg;
        cfg.leadMs = 100.0;
        cfg.maxLeadRatio = 1.0;
        PredictorState stA, stB;
        const Box small{ 0, 0, 10.0, 10.0 };   // 对角线 ≈ 14.14
        const Box large{ 0, 0, 300.0, 400.0 }; // 对角线 = 500

        PredictorResult rS = predictAnchor(center, Vec2{ 100000.0, 0.0 }, small, cfg, stA);
        PredictorResult rL = predictAnchor(center, Vec2{ 100000.0, 0.0 }, large, cfg, stB);
        check(rS.leadClamped && rL.leadClamped, "两者都被距离上限顶住");
        checkNear(rS.lead.norm(), 10.0 * std::sqrt(2.0), 1e-6,
                  "★ 小框上限 = 对角线 (≈14.14px)");
        checkNear(rL.lead.norm(), 500.0, 1e-6, "★ 大框上限 = 对角线 (500px)");
        check(rL.lead.norm() > rS.lead.norm(),
              "★ 同样的倍率、大框允许推得更远（相对口径生效）");
    }

    // 倍率 0 = 不限制
    {
        PredictorConfig cfg;
        cfg.leadMs = 100.0;
        cfg.maxLeadRatio = 0.0;
        PredictorState st;
        PredictorResult r = predictAnchor(center, Vec2{ 10000.0, 0.0 }, box, cfg, st);
        check(!r.leadClamped, "★ 倍率 0 ⇒ 不做距离钳制");
        checkNear(r.lead.x, 1000.0, 1e-6, "倍率 0 时推进不被削");
        checkNear(r.maxLeadPx, 0.0, 1e-12, "倍率 0 时上限读数为 0（不限制）");
    }

    // 速度上限为 0 = 不限制
    {
        PredictorConfig cfg;
        cfg.leadMs = 1.0;
        cfg.maxVelocityPxPerSec = 0.0;
        PredictorState st;
        PredictorResult r = predictAnchor(center, Vec2{ 999999.0, 0.0 }, box, cfg, st);
        check(!r.velocityClamped, "★ 速度上限 0 ⇒ 不做速度钳制");
    }
}

static void testVelocityDegenerate()
{
    section("⑥ 速度为 0 / 非有限值时不预测");

    const Vec2 center{ 100.0, 200.0 };
    const Box box{ 90, 190, 20, 20 };
    PredictorConfig cfg;
    cfg.leadMs = 50.0;

    PredictorState st;
    PredictorResult r = predictAnchor(center, Vec2{ 0.0, 0.0 }, box, cfg, st);
    check(!r.applied, "★ 速度为 0 ⇒ 不预测");
    check(r.idleReason == PredictorResult::IdleReason::NotInitialized,
          "idleReason = NotInitialized");

    PredictorState st2;
    const double nan = std::nan("");
    PredictorResult r2 = predictAnchor(center, Vec2{ nan, 0.0 }, box, cfg, st2);
    check(!r2.applied, "★ 速度非有限值 ⇒ 不预测（不把 NaN 泄进控制回路）");
}

// ── 整链 ─────────────────────────────────────────────────────────────────

static ControllerConfig makeCfg()
{
    ControllerConfig cfg;
    cfg.buckets.byClassId = { Bucket::Aim };
    cfg.aimPoint.yOffset = 0.5;
    cfg.aimPoint.yOffsetMax = 0.5;
    cfg.pid.kpX = 35.0;
    cfg.pid.kpY = 35.0;
    return cfg;
}

static void testChainDisabledIsIdentity()
{
    section("⑦★ 整链：lead_ms=0 时与未加该功能逐帧完全一致");

    const double dt = 1.0 / 120.0;

    auto run = [&](double leadMs) {
        ControllerConfig cfg = makeCfg();
        cfg.predictor.leadMs = leadMs;
        AimController ac;
        ac.setConfig(cfg);

        std::vector<Counts> trace;
        std::vector<Vec2> anchors;
        ControlInput in;
        in.cross = Vec2{ 320, 240 };
        in.dtSec = dt;
        for (int i = 0; i < 60; ++i)
        {
            Candidate c;
            const double x = 400.0 + i * 4.0;   // 匀速右移
            c.box = Box{ x, 200.0, 40.0, 80.0 };
            c.classId = 0;
            c.confidence = 0.9;
            in.candidates.clear();
            in.candidates.push_back(c);
            in.frameIndex = static_cast<uint64_t>(i);
            const ControlOutput out = ac.update(in);
            trace.push_back(out.counts);
            anchors.push_back(out.anchor);
        }
        return std::make_pair(trace, anchors);
    };

    // 基线：关闭时两次独立运行必须逐帧一致
    const auto off1 = run(0.0);
    const auto off2 = run(0.0);

    bool countsIdentical = true;
    bool anchorsIdentical = true;
    for (size_t i = 0; i < off1.first.size(); ++i)
    {
        if (off1.first[i].x != off2.first[i].x || off1.first[i].y != off2.first[i].y)
            countsIdentical = false;
        if (std::fabs(off1.second[i].x - off2.second[i].x) > 1e-12 ||
            std::fabs(off1.second[i].y - off2.second[i].y) > 1e-12)
            anchorsIdentical = false;
    }
    check(countsIdentical, "★ 关闭时 counts 逐帧一致");
    check(anchorsIdentical, "★ 关闭时瞄点逐帧一致");

    // ★ 打开预测后必须【真的不一样】，否则说明上面那条恒等是空断言
    const auto on = run(50.0);
    bool differs = false;
    for (size_t i = 0; i < off1.second.size(); ++i)
    {
        if (std::fabs(off1.second[i].x - on.second[i].x) > 0.5)
            differs = true;
    }
    check(differs, "★ 打开预测后瞄点确实前移（证明上面那条恒等断言有区分度）");
    check(on.second[40].x > off1.second[40].x,
          "★ 目标向右移动 ⇒ 预测后的瞄点更靠右");
}

static void testChainSnapClearsPredictor()
{
    section("⑧★ 目标跳变时预测状态被清空（不崩、不残留）");

    const double dt = 1.0 / 120.0;
    ControllerConfig cfg = makeCfg();
    cfg.predictor.leadMs = 10.0;
    AimController ac;
    ac.setConfig(cfg);

    ControlInput in;
    in.cross = Vec2{ 320, 240 };
    in.dtSec = dt;

    for (int i = 0; i < 5; ++i)
    {
        Candidate c; c.box = Box{ 400.0 + i * 2.0, 200.0, 40.0, 80.0 };
        c.classId = 0; c.confidence = 0.9;
        in.candidates.clear();
        in.candidates.push_back(c);
        in.frameIndex = static_cast<uint64_t>(i);
        ac.update(in);
    }

    // 瞬移（远超对角线 × kSnapMult）⇒ 判 Snap。必须能正常返回。
    Candidate c; c.box = Box{ 2000.0, 200.0, 40.0, 80.0 };
    c.classId = 0; c.confidence = 0.9;
    in.candidates.clear();
    in.candidates.push_back(c);
    in.frameIndex = 100;
    const ControlOutput out = ac.update(in);
    check(out.engaged, "Snap 拍仍能正常 engage（不崩）");
    check(out.predictor.idleReason != control::PredictorResult::IdleReason::Disabled,
          "Snap 拍预测器仍处于启用态");
}

static void testChainRateLimitsThroughController()
{
    section("⑨★ 上限经整链生效（不是只在预测器里）");

    const double dt = 1.0 / 120.0;

    auto runWith = [&](double ratio) {
        ControllerConfig cfg = makeCfg();
        cfg.predictor.leadMs = 80.0;
        cfg.predictor.maxLeadRatio = ratio;
        AimController ac;
        ac.setConfig(cfg);

        ControlInput in;
        in.cross = Vec2{ 320, 240 };
        in.dtSec = dt;
        Vec2 lastAnchor;
        for (int i = 0; i < 30; ++i)
        {
            Candidate c;
            c.box = Box{ 400.0 + i * 8.0, 200.0, 40.0, 80.0 };  // 快速移动
            c.classId = 0; c.confidence = 0.9;
            in.candidates.clear();
            in.candidates.push_back(c);
            in.frameIndex = static_cast<uint64_t>(i);
            lastAnchor = ac.update(in).anchor;
        }
        return lastAnchor;
    };

    const Vec2 unlimited = runWith(0.0);
    const Vec2 capped = runWith(0.1);

    check(capped.x < unlimited.x, "★ 距离上限经整链真的把推进量收小了");
}

static void testChainLeadScalesWithConfig()
{
    section("⑩★ 推进量与 lead_ms 成正比（配置真的驱动，不是摆设）");

    const double dt = 1.0 / 120.0;

    auto runLead = [&](double leadMs) {
        ControllerConfig cfg = makeCfg();
        cfg.predictor.leadMs = leadMs;
        AimController ac;
        ac.setConfig(cfg);

        ControlInput in;
        in.cross = Vec2{ 0, 0 };
        in.dtSec = dt;
        Vec2 anchor;
        for (int i = 0; i < 40; ++i)
        {
            Candidate c;
            c.box = Box{ 400.0 + i * 5.0, 200.0, 40.0, 80.0 };
            c.classId = 0; c.confidence = 0.9;
            in.candidates.clear();
            in.candidates.push_back(c);
            in.frameIndex = static_cast<uint64_t>(i);
            anchor = ac.update(in).anchor;
        }
        return anchor.x;
    };

    const double x20 = runLead(20.0);
    const double x60 = runLead(60.0);

    check(x60 > x20, "★ lead_ms 越大 ⇒ 推进越多");

    // 场景：目标每帧右移 5px，dt = 1/120 s ⇒ 稳态速度 ≈ 600 px/s。
    // Δlead = 40ms ⇒ 两者推进量之差 ≈ 600 × 0.04 = 24px。
    // 容差给宽（±8px）：滤波器速度估计需要若干帧才收敛，不是精确 600。
    checkNear(x60 - x20, 24.0, 8.0,
              "★ 差值符合 速度 × Δlead 的量级（≈600 px/s × 40ms = 24px）");
}

int main()
{
    std::printf("=== 在途补偿（预测提前量）回归 ===\n\n");

    testPredictorDisabled();
    testPredictorBasic();
    testNoAutoAccumulation();
    testRateLimits();
    testVelocityDegenerate();
    testChainDisabledIsIdentity();
    testChainSnapClearsPredictor();
    testChainRateLimitsThroughController();
    testChainLeadScalesWithConfig();

    std::printf("\n%d 项断言, 失败 %d\n", g_checks, g_failures);
    if (g_failures == 0)
        std::printf("全部通过\n");
    return g_failures == 0 ? 0 : 1;
}
