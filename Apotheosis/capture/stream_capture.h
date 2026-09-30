#pragma once

#include <memory>
#include <string>

#include "capture.h"

namespace stream_capture {

bool IsNetworkSource(const std::string& source);
bool ValidateUrl(const std::string& source, const std::string& url,
                 std::string* reason = nullptr);
std::unique_ptr<IScreenCapture> Create(const std::string& source,
                                       const std::string& url, int outputSide);

} // namespace stream_capture
