#include "capture/dxgi_capture_logic.h"

#include <cstdio>
#include <cstring>
#include <vector>

namespace {
int failures = 0;
void check(bool okay, const char* message)
{
    if (!okay) { std::printf("FAIL: %s\n", message); ++failures; }
}

using namespace dxgi_capture;

OutputInfo output(const char* name, bool primary, int w = 1920, int h = 1080)
{
    OutputInfo o;
    o.deviceName = name;
    o.width = w;
    o.height = h;
    o.primary = primary;
    return o;
}
} // namespace

int main()
{
    // Choosing the monitor.
    {
        const std::vector<OutputInfo> outputs{output("\\\\.\\DISPLAY2", false), output("\\\\.\\DISPLAY1", true),
                                              output("\\\\.\\DISPLAY3", false)};
        auto c = chooseOutput(outputs, "\\\\.\\DISPLAY3");
        check(c.index == 2 && !c.fellBack, "a stored monitor is found by its device name");
        c = chooseOutput(outputs, "");
        check(c.index == 1 && !c.fellBack, "no choice means the primary monitor, silently");
        c = chooseOutput(outputs, "\\\\.\\DISPLAY9");
        check(c.index == 1 && c.fellBack, "a monitor that is gone falls back to the primary and says so");
        const std::vector<OutputInfo> noPrimary{output("A", false), output("B", false)};
        check(chooseOutput(noPrimary, "").index == 0, "without a flagged primary the first monitor is used");
        check(chooseOutput({}, "A").index == -1, "no monitors at all is reported, not guessed");
    }

    // The centred square.
    {
        auto g = centerCrop(1920, 1080, 640);
        check(g.size == 640 && g.x == 640 && g.y == 220 && g.side == 640 && !g.scale,
              "a 640 square is cut from the middle of a 1080p monitor");
        g = centerCrop(2560, 1440, 320);
        check(g.x == 1120 && g.y == 560 && g.size == 320, "the cut is centred on a 1440p monitor");
        g = centerCrop(1280, 720, 1024);
        check(g.size == 720 && g.x == 280 && g.y == 0 && g.side == 1024 && g.scale,
              "a monitor smaller than the side is cut at its shorter side and scaled up");
        g = centerCrop(1921, 1081, 640);
        check(g.x == 640 && g.y == 220 && g.x + g.size <= 1921 && g.y + g.size <= 1081,
              "odd monitor sizes stay inside the image");
        check(centerCrop(1920, 1080, 8).side == 32, "a tiny wanted side is raised to the minimum");
        check(centerCrop(1920, 1080, 99999).side == 2048 && centerCrop(1920, 1080, 99999).size == 1080,
              "an oversized side is clamped and the cut stays on the monitor");
        check(centerCrop(0, 1080, 640).side == 0 && centerCrop(1920, -1, 640).side == 0,
              "an invalid monitor size yields no geometry");
    }

    // Time handling.
    {
        check(ticksToNs(10000000, 10000000) == 1000000000LL, "10 MHz: one second of ticks is 1e9 ns");
        check(ticksToNs(15, 10000000) == 1500, "sub-second ticks convert exactly");
        const int64_t longUptime = 10000000LL * 60 * 60 * 24 * 400; // 400 days at 10 MHz
        check(ticksToNs(longUptime, 10000000) == 60LL * 60 * 24 * 400 * 1000000000LL,
              "a very long uptime does not overflow");
        check(ticksToNs(-1, 10000000) == 0 && ticksToNs(5, 0) == 0, "bad inputs convert to 0");
        check(ageUs(5000000, 2000000) == 3000, "age in microseconds");
        check(ageUs(5000000, 0) == -1 && ageUs(1000, 2000) == -1, "unknown or future timestamps are unknown");
        check(ageUs(100000000000LL, 1000) == -1, "an absurdly old frame is reported as unknown");
    }

    // Pixel conversion.
    {
        // 2x2 region inside a padded 3x3 BGRA image (stride 16 bytes).
        std::vector<uint8_t> src(16 * 3, 0xEE);
        auto put = [&](int row, int col, uint8_t b, uint8_t g, uint8_t r, uint8_t a) {
            uint8_t* p = &src[static_cast<size_t>(row) * 16 + static_cast<size_t>(col) * 4];
            p[0] = b; p[1] = g; p[2] = r; p[3] = a;
        };
        put(1, 1, 10, 20, 30, 255);
        put(1, 2, 11, 21, 31, 0);
        put(2, 1, 12, 22, 32, 128);
        put(2, 2, 13, 23, 33, 1);
        std::vector<uint8_t> dst(2 * 8, 0xCC); // stride 8 > 6: padding must stay untouched
        check(bgraToBgr(&src[16 + 4], 16, 2, 2, dst.data(), 8), "a padded region converts");
        const uint8_t want[] = {10, 20, 30, 11, 21, 31, 0xCC, 0xCC, 12, 22, 32, 13, 23, 33, 0xCC, 0xCC};
        check(std::memcmp(dst.data(), want, sizeof(want)) == 0,
              "channel order is kept, alpha dropped, destination padding untouched");
        check(!bgraToBgr(nullptr, 16, 2, 2, dst.data(), 8) && !bgraToBgr(src.data(), 16, 2, 2, nullptr, 8),
              "null buffers are refused");
        check(!bgraToBgr(src.data(), 4, 2, 2, dst.data(), 8) && !bgraToBgr(src.data(), 16, 2, 2, dst.data(), 5) &&
              !bgraToBgr(src.data(), 16, 0, 2, dst.data(), 8),
              "strides or sizes that would read or write out of bounds are refused");
    }

    // Frame rate estimate.
    {
        FpsMeter meter;
        const auto start = std::chrono::steady_clock::now();
        for (int i = 0; i < 130; ++i) meter.tick(start + std::chrono::milliseconds(i * 10));
        check(meter.fps() >= 98 && meter.fps() <= 102, "100 frames per second is measured as about 100");
        meter.idle(start + std::chrono::milliseconds(1300 + 500));
        check(meter.fps() > 0, "a short pause keeps the last estimate");
        meter.idle(start + std::chrono::milliseconds(1300 + 3000));
        check(meter.fps() == 0, "a long silence reports 0");
    }

    // Recovery policy.
    {
        check(!decideRecovery(Failure::Timeout, 0).retry && !decideRecovery(Failure::Timeout, 0).giveUp,
              "a timeout is just an unchanged screen");
        check(decideRecovery(Failure::Fatal, 0).giveUp, "a fatal failure ends the source");
        auto d = decideRecovery(Failure::AccessLost, 0);
        check(d.retry && d.delayMs == 100 && !d.giveUp, "access loss is retried soon");
        check(decideRecovery(Failure::Unavailable, 10).delayMs == 600, "the delay grows with repeated failures");
        check(decideRecovery(Failure::AccessLost, 500).delayMs == 1000, "the delay is capped");
        check(decideRecovery(Failure::AccessLost, 600).giveUp, "endless failures eventually end the source");
    }

    std::printf("dxgi capture logic: %d failures\n", failures);
    return failures ? 1 : 0;
}
