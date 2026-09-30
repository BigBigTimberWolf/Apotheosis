#pragma once

#include <filesystem>
#include <cstddef>
#include <string>

class Config;

// Empty means that an input could not be read; callers must not reuse a cache then.
std::string calibrationDatasetFingerprint(const Config& config);
std::string modelContentFingerprint(const void* data, size_t size);
std::string engineCacheFingerprint(const std::filesystem::path& modelPath,
                                   const Config& config);
