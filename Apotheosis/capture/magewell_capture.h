#pragma once

#include "capture.h"

#include <memory>
#include <string>
#include <vector>

namespace magewell
{

struct Device
{
    std::string key;
    std::string name;
    int signal_width = 0;
    int signal_height = 0;
    int signal_fps = 0;
};

bool IsDeviceKey(const std::string& key);
bool SdkEnabled();
std::vector<Device> EnumerateDevices();
std::unique_ptr<IScreenCapture> Create(const std::string& key, int output_side,
                                       int target_fps);

}
