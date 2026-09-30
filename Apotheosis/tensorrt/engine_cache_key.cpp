#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <bcrypt.h>

#include <array>
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

#include <NvInferVersion.h>
#include <cuda_runtime.h>

#include "engine_cache_key.h"
#include "config.h"

namespace {
class Sha256
{
public:
    Sha256()
    {
        if (BCryptOpenAlgorithmProvider(&algorithm_, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0)
            return;
        if (BCryptCreateHash(algorithm_, &hash_, nullptr, 0, nullptr, 0, 0) < 0)
            hash_ = nullptr;
    }

    ~Sha256()
    {
        if (hash_) BCryptDestroyHash(hash_);
        if (algorithm_) BCryptCloseAlgorithmProvider(algorithm_, 0);
    }

    Sha256(const Sha256&) = delete;
    Sha256& operator=(const Sha256&) = delete;

    bool add(const void* data, size_t size)
    {
        if (!hash_) return false;
        const auto* bytes = static_cast<const unsigned char*>(data);
        while (size)
        {
            const auto chunk = static_cast<ULONG>(std::min<size_t>(size, 1u << 20));
            if (BCryptHashData(hash_, const_cast<PUCHAR>(bytes), chunk, 0) < 0)
                return false;
            bytes += chunk;
            size -= chunk;
        }
        return true;
    }

    bool addString(const std::string& value)
    {
        const uint64_t size = value.size();
        return add(&size, sizeof(size)) && add(value.data(), value.size());
    }

    bool addFile(const std::filesystem::path& path)
    {
        std::ifstream file(path, std::ios::binary);
        if (!file) return false;
        std::array<char, 64 * 1024> bytes{};
        while (file)
        {
            file.read(bytes.data(), bytes.size());
            const auto count = file.gcount();
            if (count > 0 && !add(bytes.data(), static_cast<size_t>(count)))
                return false;
        }
        return file.eof();
    }

    std::string finish()
    {
        std::array<unsigned char, 32> digest{};
        if (!hash_ || BCryptFinishHash(hash_, digest.data(),
                                        static_cast<ULONG>(digest.size()), 0) < 0)
            return {};
        std::ostringstream out;
        for (const auto byte : digest)
            out << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(byte);
        return out.str();
    }

private:
    BCRYPT_ALG_HANDLE algorithm_ = nullptr;
    BCRYPT_HASH_HANDLE hash_ = nullptr;
};

bool isCalibrationImage(const std::filesystem::path& path)
{
    std::string ext = path.extension().u8string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return ext == ".jpg" || ext == ".jpeg" || ext == ".png" || ext == ".bmp";
}
}

std::string calibrationDatasetFingerprint(const Config& config)
{
    Sha256 hash;
    if (!hash.addString("apotheosis-calibration-v1") ||
        !hash.addString(config.int8_calib_dir) ||
        !hash.addString(std::to_string(config.int8_calib_images)))
        return {};

    const auto dir = std::filesystem::u8path(config.int8_calib_dir);
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec))
        return hash.addString("no-images") ? hash.finish() : std::string{};

    std::vector<std::string> images;
    for (std::filesystem::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
    {
        if (it->is_regular_file(ec) && isCalibrationImage(it->path()))
            images.push_back(it->path().u8string());
        if (ec) return {};
    }
    if (ec) return {};
    std::sort(images.begin(), images.end());
    if (images.size() > static_cast<size_t>(config.int8_calib_images))
        images.resize(static_cast<size_t>(config.int8_calib_images));

    for (const auto& image : images)
    {
        const auto imagePath = std::filesystem::u8path(image);
        if (!hash.addString(imagePath.filename().u8string()) || !hash.addFile(imagePath))
            return {};
    }
    return hash.finish();
}

std::string modelContentFingerprint(const void* data, size_t size)
{
    Sha256 hash;
    return data && size && hash.add(data, size) ? hash.finish() : std::string{};
}

std::string engineCacheFingerprint(const std::filesystem::path& modelPath,
                                   const Config& config)
{
    int device = 0;
    cudaDeviceProp gpu{};
    int driverVersion = 0;
    int runtimeVersion = 0;
    if (cudaGetDevice(&device) != cudaSuccess ||
        cudaGetDeviceProperties(&gpu, device) != cudaSuccess ||
        cudaDriverGetVersion(&driverVersion) != cudaSuccess ||
        cudaRuntimeGetVersion(&runtimeVersion) != cudaSuccess)
        return {};

    Sha256 hash;
    std::ostringstream options;
    options << "apotheosis-engine-v1"
            << "|trt=" << NV_TENSORRT_MAJOR << '.' << NV_TENSORRT_MINOR << '.' << NV_TENSORRT_PATCH
            << "|cuda=" << runtimeVersion << "|driver=" << driverVersion
            << "|gpu=" << gpu.name << ':' << gpu.major << '.' << gpu.minor
            << "|precision=" << config.engine_precision
            << "|fixed=" << config.fixed_input_size
            << "|resolution=" << config.detection_resolution;
    if (!hash.addString(options.str()) || !hash.addFile(modelPath))
        return {};
    if (config.engine_precision == "int8")
    {
        const auto dataset = calibrationDatasetFingerprint(config);
        if (dataset.empty() || !hash.addString(dataset))
            return {};
    }
    return hash.finish();
}
