
#include "mouse/aim_path.h"
#include "mouse/trigger_fsm.h"
#include "mouse/trigger_release.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

static int g_failures = 0;

static void check(bool ok, const std::string& what)
{
    if (!ok)
    {
        std::printf("  [FAIL] %s\n", what.c_str());
        ++g_failures;
    }
}

struct TickResult
{
    int press = 0;
    int release = 0;
    int fired = 0;
    std::vector<int> press_ticks;
};

static TickResult run(boss::TriggerFsm& fsm, int ticks, int64_t t0, int step_ms,
                      bool in_zone, int track_id,
                      bool hold_mode = false,
                      int fire_delay = 0, int duration = 0, int interval = 200,
                      int switch_cd = 0)
{
    TickResult r;
    for (int i = 0; i < ticks; ++i)
    {
        boss::TriggerFsm::Input in;
        in.in_zone = in_zone;
        in.track_id = track_id;
        in.now_ms = t0 + static_cast<int64_t>(i) * step_ms;

        const auto a = fsm.tick(in, hold_mode, fire_delay, duration, interval,
                                switch_cd, 0, 0, 0);
        if (a.press_left)   { ++r.press;   r.press_ticks.push_back(i); }
        if (a.release_left) ++r.release;
        if (a.fired)        ++r.fired;
    }
    return r;
}

static void test_hit_zone()
{
    std::printf("\n[1] 命中区几何 (以框为基准, 与瞄点解耦)\n");
    double hx = 0.0, hy = 0.0;

    check(boss::TriggerFsm::inHitZone(140, 230, 100, 200, 80, 60, 100, &hx, &hy),
          "框中心算在命中区内");
    check(std::abs(hx - 40.0) < 1e-9 && std::abs(hy - 30.0) < 1e-9,
          "100% 时半宽=40 / 半高=30");

    check(boss::TriggerFsm::inHitZone(140 + 40, 230, 100, 200, 80, 60, 100),
          "正好在右边界上算命中(含等号)");
    check(!boss::TriggerFsm::inHitZone(140 + 40.1, 230, 100, 200, 80, 60, 100),
          "超出右边界 0.1px 就不算命中");

    check(boss::TriggerFsm::inHitZone(140 + 19, 230 + 14, 100, 200, 80, 60, 50),
          "50% 时 (中心+19, 中心+14) 仍算命中");
    check(!boss::TriggerFsm::inHitZone(140 + 21, 230, 100, 200, 80, 60, 50),
          "50% 时横向超出 21px 不算命中");

    check(boss::TriggerFsm::inHitZone(140, 230 - 44, 100, 200, 80, 60, 200),
          "200% 时框上方 44px 处也算命中(预开火)");
    check(!boss::TriggerFsm::inHitZone(140, 230 - 44, 100, 200, 80, 60, 100),
          "100% 时同一位置【不】算命中");

    check(!boss::TriggerFsm::inHitZone(140 + 100, 230, 100, 200, 80, 60, 0),
          "y_percent=0 被夹到 0.1, 不会退化成永远命中");
}

static void test_zero_delay()
{
    std::printf("\n[2] 零延迟: 进区那一拍就开火\n");
    {
        boss::TriggerFsm f;
        const auto r = run(f, 1, 1000, 8,   true, 1, false,   0);
        check(r.fired == 1, "★ 进区第一拍就 fire (不是等一拍)");
        check(r.press == 1, "★ 第一拍按下左键");
        check(f.pressed(), "相位进入 Pressed");
    }
    {
        boss::TriggerFsm f;
        const auto r = run(f, 20, 1000, 8,   false, 1, false, 0);
        check(r.fired == 0, "不在命中区时一拍都不开火");
        check(r.press == 0, "不在命中区时不按左键");
    }
}

static void test_delay()
{
    std::printf("\n[3] 进区后延迟开火\n");
    boss::TriggerFsm f;
    const auto r = run(f, 10, 1000, 8, true, 1, false,   40);
    check(r.fired == 1, "延迟模式下只开火一次");
    check(!r.press_ticks.empty() && r.press_ticks[0] == 5,
          "★ 40ms / 8ms 每拍 ⇒ 第 5 拍开火 (不是第 0 拍)");

    {
        boss::TriggerFsm f2;
        boss::TriggerFsm::Input in;
        auto act = [&](bool zone, int64_t t) {
            in.in_zone = zone; in.track_id = 1; in.now_ms = t;
            return f2.tick(in, false, 40, 0, 200, 0, 0, 0, 0);
        };
        act(true, 1000); act(true, 1008); act(true, 1016);
        act(false, 1024); act(false, 1032);
        check(!act(true, 1040).fired, "离开命中区后延迟计时重来(1040 未开火)");
        check(!act(true, 1048).fired, "重来后 1048 仍未到 40ms");
        check(!act(true, 1056).fired, "重来后 1056 仍未到 40ms");
        check(!act(true, 1064).fired, "重来后 1064 仍未到 40ms");
        check(!act(true, 1072).fired, "★ 重来后 1072 仍未到 40ms (没有沿用旧计时)");
        check(act(true, 1080).fired, "★ 从 1040 起满 40ms ⇒ 1080 开火");
    }

    {
        boss::TriggerFsm f;
        boss::TriggerFsm::Input in;
        auto act = [&](bool zone, int64_t t) {
            in.in_zone = zone; in.track_id = 1; in.now_ms = t;
            return f.tick(in, false, 40, 0, 200, 0, 0, 0, 0);
        };
        check(!act(true, 1000).fired, "t=1000 进区, 未到 40ms");
        check(!act(true, 1030).fired, "t=1030 仍在 Delay 相位");
        check(f.phase() == boss::TriggerPhase::Delay,
              "★ 前提: 此刻确实处于 Delay 相位");
        check(!act(false, 1035).fired, "Delay 相位中途离开命中区 → 不开火");
        check(!act(true, 1040).fired,
              "★ Delay 相位中途离开后, 计时必须重来 (1040 不开火)");
        check(act(true, 1080).fired, "★ 从 1040 起满 40ms ⇒ 1080 才开火");
    }
}

static void test_burst_mode()
{
    std::printf("\n[4] 连点模式 (duration>0)\n");
    boss::TriggerFsm f;
    const auto r = run(f, 30, 0, 10, true, 1,   false,
                         0,   30,   50);
    check(r.press >= 3, "连点模式下持续在区内会反复开火");
    check(r.press == r.release || r.press == r.release + 1,
          "★ 每一发都配对的按下/抬起(不会卡在按下)");
    check(r.fired == r.press, "每发 fire 标记与 press 一一对应");
}

static void test_hold_mode()
{
    std::printf("\n[5] 长按模式 (duration=0)\n");
    {
        boss::TriggerFsm f;
        const auto r = run(f, 20, 0, 10, true, 1,   true, 0, 0, 200);
        check(r.press == 1, "★ 长按模式在区内只按一次");
        check(r.release == 0, "★ 长按模式在区内不松手");
    }
    {
        boss::TriggerFsm f;
        boss::TriggerFsm::Input in;
        auto act = [&](bool zone, int64_t t) {
            in.in_zone = zone; in.track_id = 1; in.now_ms = t;
            return f.tick(in,   true, 0, 0, 200, 0, 0, 0, 0);
        };
        check(act(true, 0).press_left, "进区按下");
        check(!act(true, 10).release_left, "留在区内不松手");
        check(act(false, 20).release_left, "★ 离开命中区才松手");
        check(!f.pressed(), "松手后相位不再是 Pressed");
    }
}

static void test_switch_cooldown()
{
    std::printf("\n[6] 换目标冷却\n");
    {
        boss::TriggerFsm f;
        boss::TriggerFsm::Input in;
        auto act = [&](bool zone, int id, int64_t t) {
            in.in_zone = zone; in.track_id = id; in.now_ms = t;
            return f.tick(in,   true, 0, 0, 200,   100, 0, 0, 0);
        };
        check(act(true, 1, 0).press_left, "目标 1 进区长按开火");
        check(f.phase() == boss::TriggerPhase::Pressed, "★ 前提: 处于 Pressed 相位");

        const auto a = act(true, 2, 10);
        check(!a.release_left, "★ 换目标但在区内时【不】松手");
        check(f.phase() == boss::TriggerPhase::Pressed,
              "★ 换目标但在区内时相位仍是 Pressed (没有进转火冷却)");
    }
    {
        boss::TriggerFsm f;
        boss::TriggerFsm::Input in;
        auto act = [&](bool zone, int id, int64_t t) {
            in.in_zone = zone; in.track_id = id; in.now_ms = t;
            return f.tick(in,   true, 0, 0, 200,   100, 0, 0, 0);
        };
        act(true, 1, 0);
        act(false, 1, 10);
        act(false, 2, 20);
        check(f.phase() == boss::TriggerPhase::SwitchCooldown,
              "★ 换目标且不在命中区 ⇒ 进入转火冷却");
        act(false, 2, 120);
        check(f.phase() != boss::TriggerPhase::SwitchCooldown,
              "转火冷却到点后退出");
    }
    {
        boss::TriggerFsm f;
        boss::TriggerFsm::Input in;
        auto act = [&](bool zone, int id, int64_t t) {
            in.in_zone = zone; in.track_id = id; in.now_ms = t;
            return f.tick(in, false, 0, 0, 200,   0, 0, 0, 0);
        };
        act(true, 1, 0);
        act(false, 1, 10);
        act(false, 2, 20);
        check(f.phase() != boss::TriggerPhase::SwitchCooldown,
              "switch_cd=0 时不进转火冷却");
    }
}

static void test_reset_releases()
{
    std::printf("\n[7] reset 把按住的左键还回去\n");
    {
        boss::TriggerFsm f;
        run(f, 3, 0, 10, true, 1,   true, 0, 0, 200);
        check(f.pressed(), "前提: 长按模式下确实按着左键");
        check(f.reset() == true, "★ reset 报告「之前按着」 ⇒ 调用方必须 releaseLeftButton");
        check(!f.pressed(), "reset 后不再是 Pressed");
    }
    {
        boss::TriggerFsm f;
        check(f.reset() == false, "★ 没按着时 reset 返回 false (不谎报)");
    }
    {
        boss::TriggerFsm f;
        boss::TriggerFsm::Input in;
        in.in_zone = true; in.track_id = 7; in.now_ms = 0;
        f.tick(in, false, 0, 0, 200, 100, 0, 0, 0);
        f.reset();
        in.in_zone = false; in.track_id = 9; in.now_ms = 10;
        f.tick(in, false, 0, 0, 200, 100, 0, 0, 0);
        check(f.phase() != boss::TriggerPhase::SwitchCooldown,
              "★ reset 后第一帧不算「换目标」");
    }
}

static void test_target_loss_releases()
{
    boss::TriggerFsm trigger;
    boss::ScopeController scope;
    run(trigger, 1, 0, 10, true, 1, true, 0, 0, 200);
    scope.tick(true, true, 2, 0, 0);
    const auto held = boss::releaseOnTargetLoss(trigger, scope, 2);
    check(held.left && held.right, "目标消失时释放长按的左键和右键");
    const auto again = boss::releaseOnTargetLoss(trigger, scope, 2);
    check(!again.left && !again.right, "连续丢框不会重复发送松键");

    boss::ScopeController tapScope;
    tapScope.tick(true, true, 1, 0, 0);
    const auto tap = boss::releaseOnTargetLoss(trigger, tapScope, 1);
    check(tap.right, "点按开镜时先完成待释放的短按");
    check(tapScope.engaged(), "丢框后保留点按开镜状态，重获目标不再点一次");
    check(!tapScope.tick(true, true, 1, 0, 20).press_right,
          "重获目标不会把游戏里的镜关掉");
}

static void test_linear_passthrough()
{
    std::printf("\n[8] 直线模式逐位透传\n");
    boss::AimPathDriver d;
    boss::AimPathDriver::Params p;
    p.mode = boss::AimPathDriver::Mode::Linear;
    d.configure(p);

    for (double bx : {0.0, 7.0, -13.0, 250.0})
    {
        const auto r = d.step(500, 500, 400, 400, 0.008, 1, bx, -bx);
        check(r.move_x == bx && r.move_y == -bx,
              "★ 直线模式必须逐位透传 base_dx/base_dy (不缩放不旋转)");
    }
}

static void test_no_scaling()
{
    std::printf("\n[9] ★★ 曲线只旋转不缩放 (核心不变量)\n");
    for (int m = 1; m <= 3; ++m)
    {
        boss::AimPathDriver d;
        boss::AimPathDriver::Params p;
        p.mode = static_cast<boss::AimPathDriver::Mode>(m);
        p.strength = 1.0;
        p.cy1 = 0.35;
        p.cy2 = -0.35;
        d.configure(p);

        const double bx = 40.0, by = 0.0;
        const double base_mag = std::hypot(bx, by);
        int nonzero = 0;
        for (int i = 0; i < 40; ++i)
        {
            const auto r = d.step(900, 500, 500, 500, 0.008, 1, bx, by);
            const double mag = std::hypot(r.move_x, r.move_y);
            if (mag <= 1e-9) continue;
            ++nonzero;
            check(std::abs(mag - base_mag) < 1e-6,
                  "★ mode=" + std::to_string(m) + " 第 " + std::to_string(i) +
                  " 拍幅值精确等于基向量幅值 (只旋转不缩放)");
        }
        check(nonzero > 0, "★ mode=" + std::to_string(m) + " 确实产生了输出");
    }

    {
        boss::AimPathDriver d;
        boss::AimPathDriver::Params p;
        p.mode = boss::AimPathDriver::Mode::WindMouse;
        p.strength = 1.0;
        d.configure(p);
        bool off_axis = false;
        for (int i = 0; i < 40; ++i)
        {
            const auto r = d.step(900, 500, 500, 500, 0.008, 1, 40.0, 0.0);
            if (std::abs(r.move_y) > 1e-3 && std::abs(r.move_x - 40.0) > 1e-3)
                off_axis = true;
        }
        check(off_axis, "★ 曲线确实把输出方向转离了基向量 (y 分量出现)");
    }

    {
        boss::AimPathDriver d0;
        boss::AimPathDriver::Params p0;
        p0.mode = boss::AimPathDriver::Mode::WindMouse;
        p0.strength = 0.0;
        d0.configure(p0);
        bool always_passthrough = true;
        for (int i = 0; i < 30; ++i)
        {
            const auto r = d0.step(900, 500, 500, 500, 0.008, 1, 40.0, 0.0);
            if (r.move_x != 40.0 || r.move_y != 0.0) always_passthrough = false;
        }
        check(always_passthrough,
              "★ strength=0 ⇒ 曲线完全退出, 每拍逐位等于基向量");
    }
}

static void test_wind_gate()
{
    std::printf("\n[10] WindMouse 门控: 小误差走直线\n");
    boss::AimPathDriver d;
    boss::AimPathDriver::Params p;
    p.mode = boss::AimPathDriver::Mode::WindMouse;
    p.strength = 1.0;
    p.wind_threshold_px = 10.0;
    d.configure(p);

    for (double err : {0.0, 3.0, 9.9, -10.0})
    {
        const auto r = d.step(500 + err, 500 + err, 500, 500, 0.008, 1, 5.0, -3.0);
        check(r.move_x == 5.0 && r.move_y == -3.0,
              "门控内 (err=" + std::to_string(err) + ") 必须逐位透传直线");
    }

    {
        boss::AimPathDriver d2;
        d2.configure(p);
        bool diverged = false;
        for (int i = 0; i < 20; ++i)
        {
            const auto r = d2.step(540, 500, 500, 500, 0.008, 1, 5.0, -3.0);
            if (!(r.move_x == 5.0 && r.move_y == -3.0)) diverged = true;
        }
        check(diverged,
              "★ 单轴误差 40px > 阈值 ⇒ 跑几拍后曲线接管(不再逐位透传)");
    }

    {
        boss::AimPathDriver d4;
        d4.configure(p);
        const auto r = d4.step(540, 500, 500, 500, 0.008, 1, 5.0, -3.0);
        check(r.move_x == 5.0 && r.move_y == -3.0,
              "★ 曲线接管的【第一拍】仍逐位透传 (entry_fade 从 0 起)");
    }

    {
        boss::AimPathDriver d3;
        boss::AimPathDriver::Params p0 = p;
        p0.wind_threshold_px = 0.0;
        d3.configure(p0);
        const auto r = d3.step(500, 500, 500, 500, 0.008, 1, 5.0, -3.0);
        check(r.move_x == 5.0 && r.move_y == -3.0,
              "阈值为 0 且误差为 0 ⇒ 仍旁路");
    }

    {
        boss::AimPathDriver d5;
        d5.configure(p);
        bool diverged = false;
        for (int i = 0; i < 20; ++i)
        {
            const auto r = d5.step(900, 500, 500, 500, 0.008, 1, 5.0, -3.0);
            if (!(r.move_x == 5.0 && r.move_y == -3.0)) diverged = true;
        }
        check(diverged, "★ 跑热阶段确实走了曲线(前提成立)");

        const auto r = d5.step(500 + 6, 500 + 6, 500, 500, 0.008, 1, 5.0, -3.0);
        check(r.move_x == 5.0 && r.move_y == -3.0,
              "★★ 已跑热的驱动器: 误差落回门控带内 ⇒ 立刻逐位透传 (门控真的生效)");

        bool re_diverged = false;
        for (int i = 0; i < 20; ++i)
        {
            const auto r2 = d5.step(900, 500, 500, 500, 0.008, 1, 5.0, -3.0);
            if (!(r2.move_x == 5.0 && r2.move_y == -3.0)) re_diverged = true;
        }
        check(re_diverged, "★ 误差再次超阈值 ⇒ 曲线重新接管(重新起了一段)");
    }
}

static void test_determinism()
{
    std::printf("\n[11] 曲线确定性 (同一 target_id 可复现)\n");
    auto sample = [](int target_id) {
        boss::AimPathDriver d;
        boss::AimPathDriver::Params p;
        p.mode = boss::AimPathDriver::Mode::WindMouse;
        p.strength = 1.0;
        d.configure(p);
        std::vector<double> out;
        for (int i = 0; i < 25; ++i)
        {
            const auto r = d.step(900, 500, 500, 500, 0.008, target_id, 40.0, 0.0);
            out.push_back(r.move_x);
            out.push_back(r.move_y);
        }
        return out;
    };
    check(sample(42) == sample(42), "★ 同一 target_id 两次跑出的轨迹完全一致");

    {
        const auto s42 = sample(42);
        const auto s43 = sample(43);
        check(s42 != s43, "不同 target_id 的两条序列不完全相同");

        double max_early_diff = 0.0;
        for (int i = 0; i < 10; ++i)
        {
            const double y42 = s42[i * 2 + 1];
            const double y43 = s43[i * 2 + 1];
            max_early_diff = std::max(max_early_diff, std::abs(y42 - y43));
        }
        check(max_early_diff > 0.1,
              "★★ target_id 真的参与摇路径: 前 10 拍的横向分量存在明显差异"
              " (实测差异=" + std::to_string(max_early_diff) + ")");
    }

    {
        boss::AimPathDriver::Params p;
        p.mode = boss::AimPathDriver::Mode::WindMouse;
        p.strength = 1.0;

        auto warm = [&](boss::AimPathDriver& d) {
            for (int i = 0; i < 40; ++i)
                d.step(900, 500, 500, 500, 0.008, 3, 40.0, 0.0);
        };

        boss::AimPathDriver with_reset;
        with_reset.configure(p);
        warm(with_reset);
        with_reset.reset();

        boss::AimPathDriver without_reset;
        without_reset.configure(p);
        warm(without_reset);

        bool identical = true;
        for (int i = 0; i < 20; ++i)
        {
            const auto a = with_reset.step(900, 500, 500, 500, 0.008, 7, 40.0, 0.0);
            const auto b = without_reset.step(900, 500, 500, 500, 0.008, 7, 40.0, 0.0);
            if (a.move_x != b.move_x || a.move_y != b.move_y) identical = false;
        }
        check(identical,
              "★ reset 后换 id 与不 reset 换 id 逐位一致(reset 等价于换段)");
    }

    {
        boss::AimPathDriver d;
        boss::AimPathDriver::Params p;
        p.mode = boss::AimPathDriver::Mode::WindMouse;
        p.strength = 1.0;
        d.configure(p);
        for (int i = 0; i < 30; ++i)
            d.step(900, 500, 500, 500, 0.008, 42, 40.0, 0.0);
        d.reset();
        const auto r = d.step(900, 500, 500, 500, 0.008, 42, 40.0, 0.0);
        check(r.move_x == 40.0 && r.move_y == 0.0,
              "★ reset 后第一拍逐位透传(entry_fade 从 0 起, 起段不突变)");
    }
}

static void test_path_reset()
{
    std::printf("\n[12] 曲线 reset 清掉路径状态\n");
    boss::AimPathDriver d;
    boss::AimPathDriver::Params p;
    p.mode = boss::AimPathDriver::Mode::WindMouse;
    p.strength = 1.0;
    d.configure(p);

    for (int i = 0; i < 10; ++i)
        d.step(900, 500, 500, 500, 0.008, 1, 40.0, 0.0);

    d.reset();
    boss::AimPathDriver fresh;
    fresh.configure(p);

    const auto a = d.step(900, 500, 500, 500, 0.008, 1, 40.0, 0.0);
    const auto b = fresh.step(900, 500, 500, 500, 0.008, 1, 40.0, 0.0);
    check(a.move_x == b.move_x && a.move_y == b.move_y,
          "★ reset 后的第一拍 == 全新驱动器的第一拍");
}

int main()
{
    std::printf("=== 自动扳机 + 轨迹曲线回归 (2026-09-17 恢复) ===\n");

    test_hit_zone();
    test_zero_delay();
    test_delay();
    test_burst_mode();
    test_hold_mode();
    test_switch_cooldown();
    test_reset_releases();
    test_target_loss_releases();

    test_linear_passthrough();
    test_no_scaling();
    test_wind_gate();
    test_determinism();
    test_path_reset();

    std::printf("\n=== %d 项失败 ===\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
