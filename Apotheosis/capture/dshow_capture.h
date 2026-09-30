#pragma once

#include "capture.h"
#include "capture_card_caps.h"

#include <memory>
#include <string>
#include <vector>

namespace dshow
{
struct Device
{
    int index = -1;
    std::string name;
    std::vector<MFCapability> caps;
};

std::vector<Device> EnumerateDevices();
const Device* FindByName(const std::vector<Device>& devices,
                         const std::string& name);
std::unique_ptr<IScreenCapture> Create(int index, int width, int height,
                                       int fps, const std::string& format,
                                       int output_side);
}
