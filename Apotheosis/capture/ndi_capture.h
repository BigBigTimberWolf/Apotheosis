#pragma once
#include "capture.h"
#include <memory>
#include <string>
#include <vector>

namespace ndi_capture {
// Names are copied out of SDK-owned discovery buffers. Exact matching avoids
// silently connecting to an unrelated stream after the selected sender leaves.
std::vector<std::string> Discover(int timeoutMs, std::string& error);
std::unique_ptr<IScreenCapture> Create(const std::string& sourceName, int outputSide);
}
