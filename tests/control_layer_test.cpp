
#include "control/aim_controller.h"
#include "control/alpha_beta_filter.h"
#include "control/anchor.h"
#include "control/pid_controller.h"
#include "control/selector.h"
#include "control/stabilizer.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

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

static void testSelector()
{
    section("① 筛选 + 选靶");

    const SelectorConfig noConfGate;

    ClassBuckets buckets;
    buckets.byClassId = { Bucket::Aim, Bucket::Delete, Bucket::Filter, Bucket::Aim };

    std::vector<Candidate> cands;
    Candidate a; a.box = Box{ 100, 100, 40, 80 }; a.classId = 0; a.confidence = 0.9;
    Candidate b; b.box = Box{ 300, 100, 40, 80 }; b.classId = 1; b.confidence = 0.9;
    Candidate c; c.box = Box{ 500, 100, 40, 80 }; c.classId = 2; c.confidence = 0.9;
    Candidate d; d.box = Box{ 105, 300, 40, 80 }; d.classId = 3; d.confidence = 0.9;
    cands = { a, b, c, d };

    const std::vector<size_t> aimIdx = filterAimCandidates(cands, buckets, noConfGate);
    check(aimIdx.size() == 2, "Delete 与 Filter 被筛掉, 只剩 2 个 Aim");
    check(aimIdx[0] == 0 && aimIdx[1] == 3, "留下的是下标 0 与 3");

    ClassBuckets small;
    small.byClassId = { Bucket::Aim };
    Candidate odd; odd.box = Box{ 0, 0, 10, 10 }; odd.classId = 99;
    std::vector<Candidate> one = { odd };
    check(filterAimCandidates(one, small, noConfGate).empty(),
          "越界 classId 视为 Delete (未知类别不瞄)");

    Candidate bad; bad.box = Box{ 0, 0, 0, 0 }; bad.classId = 0;
    std::vector<Candidate> inv = { bad };
    check(filterAimCandidates(inv, buckets, noConfGate).empty(), "无效框(w=0)被排除");

    SelectorConfig sc;
    sc.hysteresisRatio = 1.3;
    SelectorState st;
    const Vec2 cross{ 110, 140 };
    std::vector<Candidate> two = { a, d };
    const std::vector<size_t> idx2 = filterAimCandidates(two, buckets, noConfGate);
    TargetSelection sel = selectTarget(two, idx2, cross, sc, st);
    check(sel.found, "选到了目标");
    checkNear(sel.distancePx, 10.0, 1e-9, "选中的是更近的 a (距离 10px)");

    const Vec2 cross2{ 121, 250 };
    const double distA = (Vec2{120,140} - cross2).norm();
    const double distD = (Vec2{125,340} - cross2).norm();
    check(distD < distA, "前提: d 比 a 更近");
    check(distD * 1.3 >= distA, "前提: d 的领先幅度【不到】k=1.3 倍");

    SelectorState st2;
    std::vector<Candidate> two2 = { a, d };
    TargetSelection first = selectTarget(two2, idx2, cross, sc, st2);
    check(first.classId == 0, "首帧锁定 a");

    TargetSelection second = selectTarget(two2, idx2, cross2, sc, st2);
    check(second.classId == 0,
          "滞回生效: d 虽更近但没到 1.3 倍, 仍保持锁定 a (classId=0)");

    SelectorConfig noHyst = sc;
    noHyst.hysteresisRatio = 1.0;
    SelectorState st3;
    selectTarget(two2, idx2, cross, noHyst, st3);
    TargetSelection switched = selectTarget(two2, idx2, cross2, noHyst, st3);
    check(switched.classId == 3,
          "k=1.0(无滞回) 时改选更近的 d —— 证明滞回确实来自 hysteresisRatio");

    {
        SelectorConfig far;
        far.hysteresisRatio = 1.3;
        far.maxDistancePx = 50.0;
        SelectorState stF;
        const Vec2 crossF{ 110, 140 };
        std::vector<Candidate> one2 = { a };
        const std::vector<size_t> idxA = filterAimCandidates(one2, buckets, noConfGate);
        check(selectTarget(one2, idxA, crossF, far, stF).found,
              "maxDistancePx=50 时 10px 的目标可选");

        std::vector<Candidate> farAway = { d };
        const std::vector<size_t> idxD = filterAimCandidates(farAway, buckets, noConfGate);
        SelectorState stF2;
        check(!selectTarget(farAway, idxD, crossF, far, stF2).found,
              "★ maxDistancePx=50 时超距目标被排除 (found=false)");

        SelectorConfig nolimit = far;
        nolimit.maxDistancePx = 0.0;
        SelectorState stF3;
        check(selectTarget(farAway, idxD, crossF, nolimit, stF3).found,
              "maxDistancePx=0 (不限制) 时同一目标可选 —— 证明上一条来自该参数");
    }

    SelectorState st4;
    selectTarget(two2, idx2, cross, sc, st4);
    std::vector<Candidate> onlyD = { d };
    const std::vector<size_t> idxOnlyD = filterAimCandidates(onlyD, buckets, noConfGate);
    TargetSelection afterGone = selectTarget(onlyD, idxOnlyD, cross2, sc, st4);
    check(afterGone.found && afterGone.classId == 3, "锁定目标消失后改选剩下的 d");

    SelectorState st5;
    std::vector<Candidate> none;
    TargetSelection empty = selectTarget(none, {}, cross, sc, st5);
    check(!empty.found, "无候选时 found=false");
    check(!st5.locked, "无候选时锁定状态被复位");
}

static void testStabilizer()
{
    section("② 稳定器 (认目标 + 剔除异常框, 不滤波)");

    StabilizerConfig cfg;
    cfg.matchCenterRatio = 0.5;
    cfg.areaRatioTol = 2.0;
    cfg.kSnapMult = 1.15;
    cfg.minAspect = 0.2;
    cfg.maxAspect = 5.0;

    check(aspectRatioPlausible(Box{0,0,40,80}, cfg), "40x80 (宽高比 0.5) 合法");
    check(aspectRatioPlausible(Box{0,0,200,40}, cfg), "200x40 (宽高比恰为 5.0) 在闭边界上, 合法");
    check(!aspectRatioPlausible(Box{0,0,201,40}, cfg), "201x40 (宽高比 > 5.0) 被拒");
    check(!aspectRatioPlausible(Box{0,0,400,40}, cfg), "400x40 (宽高比 10) 被拒");

    StabilizerState st;
    Candidate c1; c1.box = Box{ 100, 100, 40, 80 };
    StabilizerResult r1 = stabilize(c1, cfg, st);
    check(r1.verdict == StabilizerVerdict::NoHistory, "首帧判为 NoHistory");
    check(r1.accepted, "首帧 accepted");

    Candidate c2; c2.box = Box{ 103.7, 101.2, 40.5, 79.3 };
    StabilizerResult r2 = stabilize(c2, cfg, st);
    check(r2.box.x == c2.box.x && r2.box.y == c2.box.y &&
          r2.box.w == c2.box.w && r2.box.h == c2.box.h,
          "★ 输出框与输入框逐位相同 —— 稳定器不做任何位置平滑 (D6)");

    StabilizerState st2;
    Candidate base; base.box = Box{ 100, 100, 40, 80 };
    stabilize(base, cfg, st2);
    Candidate teleport; teleport.box = Box{ 500, 500, 40, 80 };
    StabilizerResult rT = stabilize(teleport, cfg, st2);
    check(rT.verdict == StabilizerVerdict::Snap, "中心瞬移 ⇒ Snap (下游需硬重置)");

    StabilizerState st3;
    stabilize(base, cfg, st3);
    Candidate grew; grew.box = Box{ 100, 100, 200, 400 };
    StabilizerResult rG = stabilize(grew, cfg, st3);
    check(rG.verdict == StabilizerVerdict::Snap, "面积突变 ⇒ Snap");

    {
        StabilizerState stSz;
        Candidate base2; base2.box = Box{ 100, 100, 40, 80 };
        stabilize(base2, cfg, stSz);

        Candidate sameCenterGrew; sameCenterGrew.box = Box{ 60, 20, 120, 240 };
        checkNear(sameCenterGrew.box.centerX(), 120.0, 1e-9, "前提: 新框中心不变");
        checkNear(sameCenterGrew.box.centerY(), 140.0, 1e-9, "前提: 新框中心不变");
        checkNear(sameCenterGrew.box.area() / base2.box.area(), 9.0, 1e-9,
                  "前提: 面积比 9 倍 (超出 areaRatioTol=2)");

        const StabilizerResult rSz = stabilize(sameCenterGrew, cfg, stSz);
        check(rSz.verdict == StabilizerVerdict::Snap,
              "★ 中心不动但面积 9 倍 ⇒ Snap (证明尺寸判据独立生效, "
              "不是靠'中心太远'那条回退路径)");

        StabilizerState stOk;
        stabilize(base2, cfg, stOk);
        Candidate sameCenterSame; sameCenterSame.box = Box{ 105, 110, 30, 60 };
        checkNear(sameCenterSame.box.centerX(), 120.0, 1e-9, "前提: 中心不变");
        checkNear(sameCenterSame.box.area() / base2.box.area() > 0.5, true, 0.0,
                  "前提: 面积比在容差内");
        check(stabilize(sameCenterSame, cfg, stOk).verdict == StabilizerVerdict::Ok,
              "中心不动且面积在容差内 ⇒ Ok (证明上面的 Snap 不是无差别触发)");
    }

    StabilizerState st4;
    stabilize(base, cfg, st4);
    const Box before = st4.lastBox;
    Candidate weird; weird.box = Box{ 100, 100, 400, 40 };
    StabilizerResult rW = stabilize(weird, cfg, st4);
    check(rW.verdict == StabilizerVerdict::Rejected, "宽高比离谱 ⇒ Rejected");
    check(!rW.accepted, "Rejected 时 accepted=false");
    check(st4.lastBox.x == before.x && st4.lastBox.w == before.w,
          "★ 被剔除的框不成为下一帧基准 (否则误检会带偏基准)");
}

static void testAlphaBeta()
{
    section("③ α-β 滤波");

    checkNear(AlphaBetaFilter::alphaForTau(0.03, 0.0), 0.0, 1e-12, "dt=0 ⇒ α=0");
    checkNear(AlphaBetaFilter::alphaForTau(0.0, 0.008), 1.0, 1e-12, "τ=0 ⇒ α=1 (直通)");
    const double a = AlphaBetaFilter::alphaForTau(0.03, 0.00833);
    check(a > 0.0 && a < 1.0, "正常 dt 下 α ∈ (0,1)");
    checkNear(a, 1.0 - std::exp(-0.00833/0.03), 1e-12, "α = 1−exp(−dt/τ)");
    const double b = AlphaBetaFilter::betaForTau(0.03, 0.00833);
    check(b > 0.0 && b < a, "β < α (β 的收敛更慢, 否则速度会发散)");

    AlphaBetaFilter f;
    check(!f.initialized(), "初始未初始化");
    f.observe(Vec2{100, 200}, 0.00833);
    check(f.initialized(), "首帧后已初始化");
    checkNear(f.position().x, 100.0, 1e-9, "首帧位置=观测");

    f.reset();
    f.observe(Vec2{0, 0}, 0.00833);
    f.observe(Vec2{100, 0}, 0.00833);
    const double after1 = f.position().x;
    check(after1 > 0.0 && after1 < 100.0,
          "阶跃后一拍位置在 (0,100) 之间 —— 既没不动也没瞬间到达");

    for (int i = 0; i < 200; ++i)
        f.observe(Vec2{100, 0}, 0.00833);
    checkNear(f.position().x, 100.0, 1e-6, "持续喂同一点 ⇒ 收敛到该点");

    AlphaBetaFilter f2;
    for (int i = 0; i < 50; ++i)
        f2.observe(Vec2{ static_cast<double>(i) * 10.0, 0 }, 0.00833);
    f2.reset();
    check(!f2.initialized(), "reset 后回到未初始化");
    f2.observe(Vec2{0, 0}, 0.00833);
    checkNear(f2.position().x, 0.0, 1e-9, "reset 后首帧直接采纳观测 (速度已清零)");

    {
        AlphaBetaFilter g;
        g.observe(Vec2{500, 500}, 0.00833);
        checkNear(g.position().x, 500.0, 1e-9, "首帧位置=观测");
        g.observe(Vec2{500, 500}, 0.00833);
        checkNear(g.position().x, 500.0, 1e-9,
                  "★ 首帧速度为零: 观测不动时第二拍位置也不动 "
                  "(若首帧给了非零速度, 这里会被外推出去)");
    }

    {
        const std::vector<Vec2> seq = {
            {0,0}, {10,0}, {20,0}, {30,0}, {40,0}
        };
        AlphaBetaFilter fresh;
        for (const Vec2& p : seq) fresh.observe(p, 0.00833);

        AlphaBetaFilter reused;
        for (int i = 0; i < 30; ++i) reused.observe(Vec2{1000.0 + i * 50.0, 700}, 0.00833);
        reused.reset();
        for (const Vec2& p : seq) reused.observe(p, 0.00833);

        checkNear(fresh.position().x, reused.position().x, 0.0,
                  "★ 复位后与全新滤波器逐位相同 —— 旧速度没有残留");
    }
}

static void testAnchor()
{
    section("④ 瞄点 (中心点 + y偏移)");

    const Vec2 center{ 200, 300 };
    const Box box{ 180, 260, 40, 80 };

    checkNear(anchorFromOffset(center, box, 1.0).y, 260.0, 1e-9, "yOffset=1 ⇒ 框顶 (260)");
    checkNear(anchorFromOffset(center, box, 0.5).y, 300.0, 1e-9, "yOffset=0.5 ⇒ 中心 (300)");
    checkNear(anchorFromOffset(center, box, 0.0).y, 340.0, 1e-9, "yOffset=0 ⇒ 框底 (340)");
    checkNear(anchorFromOffset(center, box, 0.0).x, 200.0, 1e-9, "x 不受 yOffset 影响");

    AimPointConfig cfg;
    cfg.yOffset = 0.25;
    cfg.yOffsetMax = 0.25;
    for (uint64_t i = 0; i < 5; ++i)
        checkNear(computeAnchor(center, box, cfg, i).y, 320.0, 1e-9,
                  "yOffset 区间退化时恒定 (0.25 ⇒ 320)");

    AimPointConfig rnd;
    rnd.yOffset = 0.4;
    rnd.yOffsetMax = 0.6;
    rnd.randomSeed = 12345;
    bool inRange = true;
    for (uint64_t i = 0; i < 200; ++i)
    {
        const double y = computeAnchor(center, box, rnd, i).y;
        if (y < 292.0 - 1e-9 || y > 308.0 + 1e-9) inRange = false;
    }
    check(inRange, "随机 yOffset 始终落在 [yOffset, yOffsetMax] 对应的区间内");

    const double y1 = computeAnchor(center, box, rnd, 7).y;
    const double y2 = computeAnchor(center, box, rnd, 7).y;
    checkNear(y1, y2, 0.0, "同一 frameIndex 结果完全可复现 (单测不会闪烁)");

    bool sawDifferent = false;
    const double yA = computeAnchor(center, box, rnd, 1).y;
    for (uint64_t i = 2; i < 50; ++i)
        if (std::fabs(computeAnchor(center, box, rnd, i).y - yA) > 1e-9) sawDifferent = true;
    check(sawDifferent, "不同帧的随机 yOffset 确实不同 (不是伪随机失效)");

    AimPointConfig rev;
    rev.yOffset = 0.6;
    rev.yOffsetMax = 0.4;
    rev.randomSeed = 1;
    const double yr = computeAnchor(center, box, rev, 3).y;
    check(yr >= 292.0 - 1e-9 && yr <= 308.0 + 1e-9, "yOffset/yOffsetMax 写反也落在同一区间");
}

static void testPid()
{
    section("⑤⑥ PID + 量化结转");

    const double dt = 1.0 / 120.0;

    {
        PidConfig cfg;
        PidController pid(cfg);
        const Counts c = pid.update(Vec2{110, 0}, Vec2{100, 0}, dt);
        check(c.x == 3, "纯 P: e=10 ⇒ 3 counts (dt·35·10 = 2.917 ⇒ round 3)");
        check(c.y == 0, "y 误差 0 ⇒ 输出 0");
    }

    {
        PidConfig cfg;
        cfg.kpX = cfg.kpY = 0.0;
        PidController pid(cfg);
        const Counts c = pid.update(Vec2{1000, 1000}, Vec2{0, 0}, dt);
        check(c.x == 0 && c.y == 0, "Kp=0 ⇒ 输出恒 0");
    }

    {
        PidConfig cfg;
        cfg.kpX = cfg.kpY = 0.1;
        PidController pid2(cfg);
        int total = 0;
        for (int i = 0; i < 500; ++i)
            total += pid2.update(Vec2{100.6, 0}, Vec2{100, 0}, dt).x;
        check(total == 0,
              "★ 结转不放大: 500 拍累积 0.25 counts (<1) ⇒ 总下发为 0");

        PidController pid3(cfg);
        int total3 = 0;
        for (int i = 0; i < 500; ++i)
            total3 += pid3.update(Vec2{103.0, 0}, Vec2{100, 0}, dt).x;
        check(total3 == 1, "★ 结转生效: 500 拍累积 1.25 counts ⇒ 总下发 1");

        PidController pid4(cfg);
        int total4 = 0;
        for (int i = 0; i < 2500; ++i)
            total4 += pid4.update(Vec2{100.6, 0}, Vec2{100, 0}, dt).x;
        check(total4 == 1,
              "★ 余量结转生效: 0.0005 counts/拍 累加 2500 拍 ⇒ 必须出 1 "
              "(不结转的话永远是 0, 就是当年'卡在差两像素不动'的 bug)");
    }

    {
        PidConfig cfg;
        cfg.kpX = 100.0;
        cfg.kpY = 0.0;
        PidController pid(cfg);
        const Counts c = pid.update(Vec2{200, 200}, Vec2{100, 100}, dt);
        check(c.x != 0, "Kp_x=100 ⇒ x 有输出");
        check(c.y == 0, "★ Kp_y=0 ⇒ y 无输出 (证明两套增益独立)");
    }

    {
        PidConfig cfg;
        cfg.kpX = 100000.0;
        cfg.maxOutputCounts = 50;
        PidController pid(cfg);
        const Counts c = pid.update(Vec2{10000, 0}, Vec2{0, 0}, dt);
        check(std::abs(c.x) <= 50, "输出被限幅夹住 (|counts| <= maxOutputCounts)");
    }

    {
        PidConfig cfg;
        cfg.kpX = 100000.0;
        PidController pid(cfg);
        const Counts tiny = pid.update(Vec2{0.01, 0}, Vec2{0, 0}, dt);
        check(tiny.x > 0,
              "★ 无死区: 误差 0.01px 也照常出力 (若有人加回死区, 这里会变 0)");

        PidController pid2(cfg);
        pid2.update(Vec2{0.01, 0}, Vec2{0, 0}, dt);
        checkNear(pid2.telemetry().x.p, 0.01, 1e-12,
                  "★ P 项经过原点: 0.01px 误差 ⇒ P=0.01 (连续, 无死区台阶)");
    }

    {
        auto dTermAfterStep = [dt](double tau) {
            PidConfig c;
            c.kpX = 1.0;
            c.kdX = 1.0;
            c.tauDerivSec = tau;
            PidController p(c);
            p.update(Vec2{0, 0}, Vec2{0, 0}, dt);
            p.update(Vec2{100, 0}, Vec2{0, 0}, dt);
            return p.telemetry().x.d;
        };
        const double dNoLp = dTermAfterStep(0.0);
        const double dLp = dTermAfterStep(0.020);
        check(dNoLp > 0.0, "D 项在误差阶跃时为正 (τ=0)");
        check(dLp > 0.0, "D 项在误差阶跃时为正 (有低通)");
        check(dLp < dNoLp,
              "★ D 项低通生效: 同样阶跃下 τ=20ms 的 D 项小于 τ=0 (直通) —— "
              "这就是压制零惯性急停尖峰的机制");

        PidConfig z; z.kpX = 1.0; z.kdX = 0.0;
        PidController pz(z);
        pz.update(Vec2{0, 0}, Vec2{0, 0}, dt);
        pz.update(Vec2{100, 0}, Vec2{0, 0}, dt);
        checkNear(pz.telemetry().x.d, 0.0, 1e-12, "★ kd=0 ⇒ D 项恒为 0");

        PidConfig c2; c2.kpX = 1.0; c2.kdX = 1.0; c2.tauDerivSec = 0.020;
        PidController p2(c2);
        p2.update(Vec2{0, 0}, Vec2{0, 0}, dt);
        p2.update(Vec2{100, 0}, Vec2{0, 0}, dt);
        const double d1 = p2.telemetry().x.d;
        p2.update(Vec2{100, 0}, Vec2{0, 0}, dt);
        const double d2 = p2.telemetry().x.d;
        check(d2 < d1,
              "★ 误差不再变化时 D 项经低通衰减 (不是保持尖峰值)");
    }

    {
        PidConfig a; a.kpX = 1.0; a.pFullScalePx = 0.0;
        PidConfig b; b.kpX = 1.0; b.pFullScalePx = 10.0;
        PidController pa(a), pb(b);
        const Counts ca = pa.update(Vec2{100, 0}, Vec2{0, 0}, dt);
        const Counts cb = pb.update(Vec2{100, 0}, Vec2{0, 0}, dt);
        check(ca.x > cb.x, "★ pFullScalePx 生效: 大误差下饱和版输出更小");
        checkNear(pb.telemetry().x.p, 10.0, 1e-9, "★ P 项被夹到 pFullScalePx");

        PidController pa2(a), pb2(b);
        const Counts sa = pa2.update(Vec2{3, 0}, Vec2{0, 0}, dt);
        const Counts sb = pb2.update(Vec2{3, 0}, Vec2{0, 0}, dt);
        check(sa.x == sb.x, "★ 小误差(|e|<满量程)时饱和不介入 —— 两侧输出一致");
        checkNear(pb2.telemetry().x.p, 3.0, 1e-9, "小误差时 P 项未被夹");
    }

    {
        PidConfig cfg;
        cfg.kpX = 1e6;
        cfg.maxOutputCounts = 10;
        PidController pid(cfg);
        int total = 0;
        for (int i = 0; i < 100; ++i)
            total += pid.update(Vec2{10000, 0}, Vec2{0, 0}, dt).x;
        check(total == 1000,
              "★ 限幅截掉的位移不攒欠账: 100 拍 × 限幅 10 ⇒ 总下发恰为 1000");
        check(std::fabs(pid.telemetry().x.carry) <= 1.0,
              "★ 撞限幅时 carry 保持有界 (不积累欠账)");
    }

    {
        PidController pid;
        const Counts c0 = pid.update(Vec2{200, 0}, Vec2{0, 0}, 0.0);
        check(c0.x == 0 && c0.y == 0, "dt=0 ⇒ 输出 0");
        const Counts cN = pid.update(Vec2{200, 0}, Vec2{0, 0}, -1.0);
        check(cN.x == 0 && cN.y == 0, "dt<0 ⇒ 输出 0 (不以假 dt 出假速度)");
    }

    {
        PidConfig cfg;
        cfg.kpX = 1.0;
        cfg.kiX = 20.0;
        PidController pid(cfg);
        int total = 0;
        for (int i = 0; i < 120; ++i)
            total += pid.update(Vec2{50, 0}, Vec2{0, 0}, dt).x;
        check(total > 0, "Ki>0 时持续误差累积出输出");
        check(pid.telemetry().x.i != 0.0, "积分器有值");
    }

    {
        PidConfig cfg;
        cfg.kiX = 20.0;
        cfg.tauUnwindSec = 0.030;
        PidController pid(cfg);
        for (int i = 0; i < 20; ++i)
            pid.update(Vec2{50, 0}, Vec2{0, 0}, dt);
        const double before = pid.telemetry().x.i;
        check(before > 0.0, "同向累积后积分为正");

        pid.update(Vec2{-50, 0}, Vec2{0, 0}, dt);
        check(pid.telemetry().unwoundX, "反向时标出 unwoundX");
        const double after = pid.telemetry().x.i;
        check(std::fabs(after) < std::fabs(before), "★ 反向时积分数值被削减 (回吐生效)");
    }

    {
        const double decayFast = std::exp(-dt / 0.030);
        const double decaySlow = std::exp(-dt / 0.200);
        check(decayFast < decaySlow,
              "★ τ=30ms 的衰减因子小于 τ=200ms —— 用户修正了历史过慢的 0.2s");

        auto integralAfterTwoReverse = [dt](double tau) {
            PidConfig c;
            c.kiX = 20.0;
            c.tauUnwindSec = tau;
            PidController p(c);
            for (int i = 0; i < 20; ++i) p.update(Vec2{50, 0}, Vec2{0, 0}, dt);
            p.update(Vec2{-50, 0}, Vec2{0, 0}, dt);
            p.update(Vec2{-50, 0}, Vec2{0, 0}, dt);
            return std::fabs(p.telemetry().x.i);
        };
        check(integralAfterTwoReverse(0.030) < integralAfterTwoReverse(0.200),
              "★ 实跑验证: τ=30ms 时反向两拍后的积分小于 τ=200ms");
    }

    {
        PidConfig cfg;
        cfg.kiX = 1000.0;
        cfg.iMax = 5.0;
        PidController pid(cfg);
        for (int i = 0; i < 1000; ++i)
            pid.update(Vec2{500, 0}, Vec2{0, 0}, dt);
        check(std::fabs(pid.telemetry().x.i) <= 5.0 + 1e-9,
              "★ 积分被 clamp 夹住 (iMax=5) —— 抗饱和生效");
    }

    {
        const double r52 = stabilityRatio(52.0, dt);
        check(r52 > 0.9 && r52 < 1.0, "Kp=52 时稳定线读数接近但未越过 1.0");
        const double r100 = stabilityRatio(100.0, dt);
        check(r100 > 1.0, "★ Kp=100 时读数 > 1.0 (与历史实测'Kp=100 抽'一致)");
        const double r30 = stabilityRatio(30.0, dt);
        check(r30 < 1.0, "Kp=30 时读数 < 1.0 (与历史实测'Kp=30 稳'一致)");
        check(r100 > r30, "读数随 Kp 单调增");
    }

    {
        PidConfig cfg;
        cfg.kiX = 20.0;
        PidController pid(cfg);
        for (int i = 0; i < 20; ++i) pid.update(Vec2{50, 0}, Vec2{0, 0}, dt);
        check(pid.telemetry().x.i != 0.0, "复位前积分非零");

        pid.reset();
        check(pid.telemetry().x.i == 0.0, "reset 后遥测积分清零");

        PidController clean(cfg);
        const Counts afterReset = pid.update(Vec2{50, 0}, Vec2{0, 0}, dt);
        const Counts fromClean = clean.update(Vec2{50, 0}, Vec2{0, 0}, dt);
        check(afterReset.x == fromClean.x,
              "★ reset 后第一拍的输出 == 全新控制器第一拍的输出 "
              "(证明内部积分/余量/历史真的被清了, 不是只清了遥测)");

        PidController dirty(cfg);
        for (int i = 0; i < 20; ++i) dirty.update(Vec2{50, 0}, Vec2{0, 0}, dt);
        const Counts withoutReset = dirty.update(Vec2{50, 0}, Vec2{0, 0}, dt);
        check(withoutReset.x >= afterReset.x,
              "不复位时输出不小于复位后 (证明上面那条比较是有区分度的)");
    }

    {
        // 验证 Smith 在途自身位移补偿 (一帧拉枪)
        PidConfig cfg;
        cfg.kpX = 50.0;
        cfg.kPxPerCount = 0.593;
        cfg.inflightBeta = 0.8;
        cfg.deadTimeMs = 46.0;

        PidController pid(cfg);
        // 第一拍：目标在 100px 处，发出大力度拉枪
        const Counts c1 = pid.update(Vec2{100, 0}, Vec2{0, 0}, dt);
        check(c1.x > 0, "★ 第一拍向目标猛拉");

        // 第二拍：由于 46ms 延迟画面尚未改变（仍然看到 100px）
        // 但已下发的 counts 应当被折算扣除，避免重复下令
        const Counts c2 = pid.update(Vec2{100, 0}, Vec2{0, 0}, dt);
        check(c2.x < c1.x, "★ 第二拍因在途位移扣除，输出力度显著减小（精准刹车）");

        // 对照：无在途补偿时（kPxPerCount = 0），第二拍依然在 41 counts 左右满额拉枪
        PidConfig cfgUncomp = cfg;
        cfgUncomp.kPxPerCount = 0.0;
        PidController pidUncomp(cfgUncomp);
        const Counts u1 = pidUncomp.update(Vec2{100, 0}, Vec2{0, 0}, dt);
        const Counts u2 = pidUncomp.update(Vec2{100, 0}, Vec2{0, 0}, dt);
        check(u2.x >= 40, "无补偿时第二拍继续满额拉枪（过冲成因）");
        check(c2.x < u2.x, "★ Smith 补偿确实成功抑制了重复下令！");
    }
}

static void testFullChain()
{
    section("整链 ①→⑥");

    ControllerConfig cfg;
    cfg.buckets.byClassId = { Bucket::Aim };
    cfg.selector.hysteresisRatio = 1.3;
    cfg.aimPoint.yOffset = 0.5;
    cfg.aimPoint.yOffsetMax = 0.5;
    cfg.pid.kpX = 35.0;
    cfg.pid.kpY = 35.0;

    const double dt = 1.0 / 120.0;

    {
        AimController ac;
        ac.setConfig(cfg);
        ControlInput in;
        in.cross = Vec2{ 320, 240 };
        in.dtSec = dt;
        const ControlOutput out = ac.update(in);
        check(!out.engaged, "无候选时不 engage");
        check(out.counts.x == 0 && out.counts.y == 0, "无候选时输出 0");
        check(out.idleReason == ControlOutput::IdleReason::NoCandidates,
              "idleReason = NoCandidates");
    }

    {
        AimController ac;
        ac.setConfig(cfg);
        ControlInput in;
        in.cross = Vec2{ 320, 240 };
        in.dtSec = dt;
        Candidate c; c.box = Box{ 300, 200, 40, 80 }; c.classId = 0; c.confidence = 0.9;
        in.candidates.push_back(c);
        in.detectionFresh = false;
        const ControlOutput out = ac.update(in);
        check(!out.engaged, "检测不新鲜时不 engage");
        check(out.idleReason == ControlOutput::IdleReason::StaleDetection,
              "idleReason = StaleDetection");
    }

    {
        AimController ac;
        ac.setConfig(cfg);
        ControlInput in;
        in.cross = Vec2{ 320, 240 };
        in.dtSec = dt;
        Candidate c; c.box = Box{ 380, 300, 40, 80 }; c.classId = 0; c.confidence = 0.9;
        in.candidates.push_back(c);

        const ControlOutput out = ac.update(in);
        check(out.engaged, "有目标时 engage");
        check(out.error.x > 0, "误差 x 为正 (目标在右)");
        check(out.error.y > 0, "误差 y 为正 (目标在下)");
        check(out.counts.x > 0, "★ 输出 x 为正 (往右拉)");
        check(out.counts.y > 0, "★ 输出 y 为正 (往下拉)");

        checkNear(out.anchor.x, 400.0, 1.0, "锚点 x ≈ 框中心 x (400)");
        checkNear(out.anchor.y, 340.0, 1.0, "锚点 y ≈ 框中心 y (340)");
    }

    {
        AimController ac;
        ac.setConfig(cfg);
        ControlInput in;
        in.dtSec = dt;
        in.cross = Vec2{ 320, 240 };

        Candidate c; c.box = Box{ 300, 200, 40, 80 }; c.classId = 0; c.confidence = 0.9;
        for (int i = 0; i < 10; ++i)
        {
            in.candidates = { c };
            in.frameIndex = static_cast<uint64_t>(i);
            ac.update(in);
        }
        check(ac.filter()->initialized(), "正常跟随中滤波器已初始化");

        Candidate tele; tele.box = Box{ 900, 900, 40, 80 }; tele.classId = 0; tele.confidence = 0.9;
        in.candidates = { tele };
        in.frameIndex = 100;
        const ControlOutput out = ac.update(in);
        check(out.engaged, "瞬移后仍然 engage");
        checkNear(ac.filter()->position().x, 920.0, 1.0,
                  "★ 瞬移后滤波器被复位并直接采纳新位置 (而不是从旧位置慢慢滑过去)");
    }

    {
        AimController ac;
        ac.setConfig(cfg);
        ControlInput in;
        in.dtSec = dt;
        in.cross = Vec2{ 320, 240 };
        Candidate weird; weird.box = Box{ 300, 200, 800, 20 };
        weird.classId = 0; weird.confidence = 0.9;
        in.candidates = { weird };
        const ControlOutput out = ac.update(in);
        check(!out.engaged, "宽高比离谱的框不 engage");
        check(out.idleReason == ControlOutput::IdleReason::RejectedByStabilizer,
              "idleReason = RejectedByStabilizer");
    }

    {
        AimController ac;
        ac.setConfig(cfg);
        ControlInput in;
        in.dtSec = dt;
        in.cross = Vec2{ 320, 240 };
        Candidate c; c.box = Box{ 380, 300, 40, 80 }; c.classId = 0; c.confidence = 0.9;
        in.candidates = { c };
        ac.update(in);
        check(ac.filter()->initialized(), "更新后已初始化");
        ac.reset();
        check(!ac.filter()->initialized(), "reset 后滤波器未初始化");
    }

    {
        AimController ac;
        ac.setConfig(cfg);
        check(ac.filter() != nullptr, "默认有滤波器");
        ac.setFilter(nullptr);
        check(ac.filter() != nullptr, "setFilter(nullptr) 恢复默认 α-β (不是变成没有滤波)");
    }
}

static void testPerClassAimPoint()
{
    section("★ 逐类别瞄点覆盖 (y_offset 真的被读)");

    const Box box{ 460, 250, 80, 100 };
    const Vec2 center{ 500, 300 };

    {
        AimPointConfig c;
        c.yOffset = 0.5; c.yOffsetMax = 0.5;
        const Vec2 a = computeAnchor(center, box, c, 0);
        check(std::abs(a.y - 300.0) < 1e-9, "无覆盖: yOffset=0.5 ⇒ 瞄点=框中心 300");
    }

    auto anchorYFor = [box](int classId, const std::vector<ClassAimPoint>& table,
                            double hotkeyLo, double hotkeyHi) {
        ControllerConfig cfg;
        cfg.buckets.byClassId.assign(64, Bucket::Aim);
        cfg.selector.hysteresisRatio = 1.0;
        cfg.stabilizer.matchCenterRatio = 100.0;
        cfg.stabilizer.kSnapMult = 1000.0;
        cfg.stabilizer.minAspect = 0.01;
        cfg.stabilizer.maxAspect = 100.0;
        cfg.aimPoint.yOffset = hotkeyLo;
        cfg.aimPoint.yOffsetMax = hotkeyHi;
        cfg.classAimPoints = table;

        AimController ac;
        ac.setConfig(cfg);
        ControlInput in;
        in.dtSec = 0.008;
        in.cross = Vec2{ 500, 300 };
        Candidate c; c.box = box; c.classId = classId; c.confidence = 0.9;
        in.candidates = { c };

        ControlOutput out;
        for (int i = 0; i < 12; ++i) { in.frameIndex = static_cast<uint64_t>(i); out = ac.update(in); }
        return out.anchor.y;
    };

    const std::vector<ClassAimPoint> table = { ClassAimPoint{ 7, 0.9, 0.9 },
                                               ClassAimPoint{ 9, 0.5, 0.5 } };

    const double headY = anchorYFor(7, table, 0.5, 0.5);
    checkNear(headY, 260.0, 0.5,
              "★★ 类别 7 命中覆盖表 ⇒ 瞄点 ~260 (贴框顶), 不是热键级的 300");

    const double bodyY = anchorYFor(9, table, 0.5, 0.5);
    checkNear(bodyY, 300.0, 0.5,
              "类别 9 覆盖为 0.5 ⇒ 瞄点仍 ~300 (覆盖表逐类生效, 不串台)");

    const double otherY = anchorYFor(42, table, 0.5, 0.5);
    checkNear(otherY, 300.0, 0.5,
              "★ 未列入覆盖表的类别 ⇒ 退回热键级 yOffset (不是失效)");

    const double headNoTable = anchorYFor(7, {}, 0.5, 0.5);
    checkNear(headNoTable, 300.0, 0.5,
              "★★ 覆盖表为空 ⇒ 类别 7 回到热键级 300 (证明 260 真的来自覆盖表)");

    const double hotkeyOnly = anchorYFor(7, {}, 0.8, 0.8);
    checkNear(hotkeyOnly, 270.0, 0.5,
              "★ 无覆盖表时热键级 0.8 ⇒ 瞄点 ~270 (热键级路径没被覆盖逻辑破坏)");

    const std::vector<ClassAimPoint> shuffled = { ClassAimPoint{ 9, 0.5, 0.5 },
                                                  ClassAimPoint{ 7, 0.9, 0.9 } };
    checkNear(anchorYFor(7, shuffled, 0.5, 0.5), 260.0, 0.5,
              "覆盖表顺序打乱 ⇒ 仍按 classId 匹配到 7 ⇒ 260");

    {
        ControllerConfig cfg;
        cfg.buckets.byClassId.assign(64, Bucket::Aim);
        cfg.selector.hysteresisRatio = 1.0;
        cfg.stabilizer.matchCenterRatio = 100.0;
        cfg.stabilizer.kSnapMult = 1000.0;
        cfg.stabilizer.minAspect = 0.01;
        cfg.stabilizer.maxAspect = 100.0;
        cfg.aimPoint.yOffset = 0.5; cfg.aimPoint.yOffsetMax = 0.5;
        cfg.classAimPoints.push_back(ClassAimPoint{ 7, 0.4, 0.8 });

        AimController ac;
        ac.setConfig(cfg);
        ControlInput in;
        in.dtSec = 0.008;
        in.cross = Vec2{ 500, 300 };
        Candidate c; c.box = box; c.classId = 7; c.confidence = 0.9;
        in.candidates = { c };

        double lo = 1e9, hi = -1e9;
        for (int i = 0; i < 200; ++i)
        {
            in.frameIndex = static_cast<uint64_t>(i);
            const ControlOutput o = ac.update(in);
            if (i < 30) continue;
            lo = std::min(lo, o.anchor.y);
            hi = std::max(hi, o.anchor.y);
        }
        checkNear(lo, 270.0, 1.0, "★ 逐类随机区间: 最小值 ~270 (offset=0.8 那一端被用到)");
        checkNear(hi, 310.0, 1.0, "★★ 逐类随机区间: 最大值 ~310 (offset=0.4 那一端也被用到)");
        check(hi - lo > 30.0,
              "★★ 逐类 lo≠hi ⇒ 瞄点真的在区间内散开 (不是恒打同一点)");
    }
}

static void testPerClassConfGate()
{
    section("★ 逐类别置信度门槛 (min_conf 真的被读)");

    ClassBuckets buckets;
    buckets.byClassId = { Bucket::Aim, Bucket::Aim, Bucket::Aim };

    auto mk = [](int cid, double conf) {
        Candidate c; c.box = Box{ 100.0 * cid, 100, 40, 80 };
        c.classId = cid; c.confidence = conf; return c;
    };
    std::vector<Candidate> cands = { mk(0, 0.10), mk(1, 0.30), mk(2, 0.90) };

    SelectorConfig cfg;
    cfg.minConfByClassId = { 0.35, 0.20, 0.0 };

    const std::vector<size_t> idx = filterAimCandidates(cands, buckets, cfg);
    check(idx.size() == 2, "★ 类别 0 被置信度门槛挡掉, 剩 2 个");
    check(idx[0] == 1 && idx[1] == 2, "留下的是下标 1(0.30≥0.20) 与 2(0.90, 门槛0=不限)");

    {
        SelectorConfig off;
        off.minConfByClassId = { 0.0, 0.0, 0.0 };
        check(filterAimCandidates(cands, buckets, off).size() == 3,
              "★ 门槛全 0 ⇒ 三个都留下 (0 = 不限, 不是『要 0 置信度』)");
    }

    {
        SelectorConfig empty;
        check(filterAimCandidates(cands, buckets, empty).size() == 3,
              "空门槛表 ⇒ 不过滤");
    }

    {
        SelectorConfig cfg2;
        cfg2.minConfByClassId = { 0.9 };
        check(cfg2.minConfOf(99) == 0.0, "★ 越界 classId 的门槛查询 ⇒ 0 (不限, 安全)");
        check(std::abs(cfg2.minConfOf(0) - 0.9) < 1e-9, "表内 classId 取到 0.9");
    }

    {
        SelectorConfig eq;
        eq.minConfByClassId = { 0.0, 0.30, 0.0 };
        std::vector<Candidate> c2 = { mk(1, 0.30) };
        const std::vector<size_t> r = filterAimCandidates(c2, buckets, eq);
        check(r.size() == 1,
              "★ 置信度恰好等于门槛(0.30) ⇒ 放行 (>= 语义, 边界不吃掉目标)");
    }

    {
        SelectorConfig eq;
        eq.minConfByClassId = { 0.0, 0.30, 0.0 };
        std::vector<Candidate> c2 = { mk(1, 0.2999) };
        check(filterAimCandidates(c2, buckets, eq).empty(),
              "★ 置信度略低于门槛(0.2999 < 0.30) ⇒ 被挡掉");
    }

    {
        SelectorConfig neg;
        neg.minConfByClassId = { -0.5, 0.0, 0.0 };
        check(neg.minConfOf(0) == 0.0,
              "★★ 表里存负数 ⇒ minConfOf 归零 (契约: <=0 即『不限』)");
        std::vector<Candidate> low = { mk(0, 0.0001) };
        check(filterAimCandidates(low, buckets, neg).size() == 1,
              "★★ 负门槛类别 ⇒ 极低置信度候选也放行 (确认它真的是『不限』)");
    }
}

int main()
{
    std::printf("=== 控制器层逻辑回归 ===\n");

    testSelector();
    testStabilizer();
    testAlphaBeta();
    testAnchor();
    testPid();
    testFullChain();
    testPerClassAimPoint();
    testPerClassConfGate();

    std::printf("\n%d 项断言, 失败 %d\n", g_checks, g_failures);
    if (g_failures == 0)
        std::printf("全部通过\n");
    return g_failures == 0 ? 0 : 1;
}
