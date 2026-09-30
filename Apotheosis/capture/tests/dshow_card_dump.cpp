#include "capture/dshow_capture.h"
#include "capture/gpu_color_ops.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <exception>
#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>

int Nv12ColorParityTest()
{
    for (const int side : {32, 416, 800})
    {
        cv::Mat nv12(side + side / 2, side, CV_8UC1);
        for (int row = 0; row < side; ++row)
            for (int col = 0; col < side; ++col)
                nv12.at<unsigned char>(row, col) = static_cast<unsigned char>(16 + (row * 5 + col * 3) % 220);
        for (int row = side; row < nv12.rows; ++row)
            for (int col = 0; col < side; col += 2)
            {
                nv12.at<unsigned char>(row, col) = static_cast<unsigned char>(16 + (row + col * 7) % 225);
                nv12.at<unsigned char>(row, col + 1) = static_cast<unsigned char>(16 + (row * 3 + col) % 225);
            }

        cv::Mat expected;
        cv::cvtColor(nv12, expected, cv::COLOR_YUV2BGR_NV12);
        GpuImage input, output;
        if (!input.upload(nv12.data, nv12.rows, side, 1, nv12.step)
            || !output.create(side, side, 3))
            return 2;
        launch_nv12_to_bgr_bt601_limited_u8(
            input.data(), input.step(), input.data() + static_cast<size_t>(side) * input.step(),
            input.step(), output.data(), output.step(), side, side, nullptr);
        if (cudaGetLastError() != cudaSuccess || cudaDeviceSynchronize() != cudaSuccess)
            return 3;
        cv::Mat actual;
        output.download(actual);
        const double maxDiff = cv::norm(expected, actual, cv::NORM_INF);
        std::printf("NV12 %dx%d CPU/GPU max channel difference: %.0f\n", side, side, maxDiff);
        if (maxDiff > 2.0) return 4;
    }
    return 0;
}

int main(int argc, char** argv)
{
    if (argc > 1 && std::strcmp(argv[1], "--nv12-test") == 0)
        return Nv12ColorParityTest();
    const auto devices = dshow::EnumerateDevices();
    std::printf("DirectShow video devices: %zu\n", devices.size());
    for (const auto& device : devices)
    {
        std::printf("[%d] %s\n", device.index, device.name.c_str());
        for (const auto& cap : device.caps)
        {
            std::printf("    %s %dx%d @", cap.format.c_str(), cap.width, cap.height);
            for (int fps : cap.fps) std::printf(" %d", fps);
            std::printf(" fps\n");
        }
    }
    if (argc > 1)
    {
        for (const auto& device : devices)
        {
            if (!std::strstr(device.name.c_str(), argv[1])) continue;
            try
            {
                const auto cap = device.caps.empty() ? MFCapability{"YUY2", 1920, 1080, {60}, true}
                                                      : device.caps.front();
                const char* format = argc > 2 ? argv[2] : cap.format.c_str();
                const int width = argc > 3 ? std::atoi(argv[3]) : cap.width;
                const int height = argc > 4 ? std::atoi(argv[4]) : cap.height;
                const int fps = argc > 5 ? std::atoi(argv[5])
                    : cap.fps.empty() ? 60 : cap.fps.back();
                const int side = argc > 6 ? std::atoi(argv[6]) : 320;
                auto capture = dshow::Create(device.index, width, height,
                                             fps, format, side);
                for (int i = 0; i < 5; ++i)
                {
                    auto frame = capture->GetNextFrameGpu();
                    std::printf("read %d: %s\n", i + 1, frame.empty() ? "no frame" : "frame received");
                    if (!frame.empty())
                    {
                        cv::Mat bgr;
                        frame.download(bgr);
                        if (bgr.empty() || bgr.type() != CV_8UC3
                            || bgr.cols != side || bgr.rows != side)
                            return 5;
                        const auto center = bgr.at<cv::Vec3b>(side / 2, side / 2);
                        std::printf("output %dx%d BGR center=%u,%u,%u\n", bgr.cols, bgr.rows,
                                    center[0], center[1], center[2]);
                        return 0;
                    }
                }
                return 2;
            }
            catch (const std::exception& error)
            {
                std::printf("DirectShow open failed: %s\n", error.what());
                return 3;
            }
        }
        std::printf("No matching DirectShow device: %s\n", argv[1]);
        return 4;
    }
    return devices.empty() ? 1 : 0;
}
