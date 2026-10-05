#pragma once

#include "capture/capture_card_caps.h"

#include <string>
#include <vector>

namespace capture_card
{

std::vector<MFDeviceInfo> ProbeAll();
std::vector<MFDeviceInfo> ProbeMediaFoundation();
std::vector<MFDeviceInfo> ProbeDirectShow();

std::vector<MFDeviceInfo> ProbeOne(int device_index);

const MFDeviceInfo* FindByName(const std::vector<MFDeviceInfo>& devs,
                               const std::string& friendly_name);

}
