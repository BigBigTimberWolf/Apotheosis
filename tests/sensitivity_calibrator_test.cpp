#include "control/sensitivity_calibrator.h"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <thread>

int main()
{
    control::SensitivityCalibrator calibrator;
    calibrator.start();
    for (int i = 0; i < 500 && !calibrator.isReady(); ++i)
        calibrator.feed(300.0 - i, 2, 1.0 / 120.0);
    if (!calibrator.isReady() ||
        std::abs(calibrator.estimatedK() - 0.5) > 0.01)
    {
        std::puts("deterministic sensitivity fit failed");
        return 1;
    }
    const auto completed = calibrator.status();
    if (!completed.ready || completed.progress != 1.0)
    {
        std::puts("completed calibration status was not reported");
        return 1;
    }

    std::atomic<bool> invalid{ false };
    std::thread feeder([&] {
        for (int i = 0; i < 5000; ++i)
            calibrator.feed(300.0 - i, 2, 1.0 / 120.0);
    });
    std::thread reader([&] {
        for (int i = 0; i < 5000; ++i)
        {
            const auto status = calibrator.status();
            if (status.sampleCount < 0 || status.validBatches < 0 ||
                status.progress < 0.0 || status.progress > 1.0 ||
                !std::isfinite(status.estimatedK))
                invalid.store(true);
            if (i % 500 == 0)
                calibrator.start();
            if (i % 500 == 499)
                calibrator.stop();
        }
    });
    feeder.join();
    reader.join();
    if (invalid.load())
    {
        std::puts("inconsistent concurrent calibrator status");
        return 1;
    }
    return 0;
}
