#pragma once

#include <algorithm>
#include <climits>
#include <cstdlib>
#include <sstream>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

struct MFCapability
{
    std::string      format;
    int              width  = 0;
    int              height = 0;
    std::vector<int> fps;

    bool supported = false;
};

struct MFDeviceInfo
{
    int                       index = 0;
    std::string               name;
    std::string               friendly_name;
    std::vector<MFCapability> caps;
    bool                      caps_probed = false;
    std::string               probe_error;
    bool                      directshow_fallback = false;
    int                       directshow_index = -1;
};

namespace mfcap
{

inline int FormatLatencyRank(const std::string& format, bool supported)
{
    if (!supported)        return 100;
    if (format == "NV12")  return 1;
    if (format == "YUY2")  return 2;
    if (format == "RGB24") return 3;
    if (format == "RGB32") return 3;
    if (format == "MJPG")  return 9;
    return 50;
}

inline std::vector<std::string> Formats(const MFDeviceInfo& dev,
                                        bool supported_only = true)
{
    std::vector<std::string> out;
    for (const auto& c : dev.caps)
    {
        if (supported_only && !c.supported) continue;
        if (std::find(out.begin(), out.end(), c.format) == out.end())
            out.push_back(c.format);
    }
    std::stable_sort(out.begin(), out.end(),
                     [](const std::string& a, const std::string& b) {
                         return FormatLatencyRank(a, true) < FormatLatencyRank(b, true);
                     });
    return out;
}

inline std::vector<std::pair<int, int>> Resolutions(const MFDeviceInfo& dev,
                                                    const std::string& format)
{
    std::vector<std::pair<int, int>> out;
    for (const auto& c : dev.caps)
    {
        if (c.format != format) continue;
        const std::pair<int, int> wh{ c.width, c.height };
        if (std::find(out.begin(), out.end(), wh) == out.end())
            out.push_back(wh);
    }
    std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) {
        return static_cast<long long>(a.first) * a.second
             > static_cast<long long>(b.first) * b.second;
    });
    return out;
}

inline std::vector<int> FpsList(const MFDeviceInfo& dev, const std::string& format,
                                int width, int height)
{
    for (const auto& c : dev.caps)
        if (c.format == format && c.width == width && c.height == height)
            return c.fps;
    return {};
}

inline bool Supports(const MFDeviceInfo& dev, const std::string& format,
                     int width, int height, int fps)
{
    for (const auto& c : dev.caps)
    {
        if (c.format != format || c.width != width || c.height != height)
            continue;
        if (fps <= 0) return true;
        for (int f : c.fps)
            if (std::abs(f - fps) <= 1) return true;
        return false;
    }
    return false;
}

inline bool Validate(const MFDeviceInfo& dev, const std::string& format,
                     int width, int height, int fps)
{
    bool decodable = false;
    for (const auto& c : dev.caps)
        if (c.format == format) { decodable = c.supported; break; }
    if (!decodable) return false;

    return Supports(dev, format, width, height, fps);
}

inline bool PickBest(const MFDeviceInfo& dev, int want_w, int want_h, int want_fps,
                     std::string& format, int& width, int& height, int& fps,
                     std::string* out_reason = nullptr)
{
    struct Candidate { std::string format; int width, height, fps; };
    std::vector<Candidate> cands;
    for (const auto& c : dev.caps)
    {
        if (!c.supported || c.fps.empty()) continue;
        for (int f : c.fps)
            cands.push_back({ c.format, c.width, c.height, f });
    }
    if (cands.empty()) return false;

    const auto score = [&](const Candidate& c) {
        const int       fps_ok  = (want_fps > 0 && c.fps >= want_fps) ? 0 : 1;
        const int       fps_gap = (want_fps > 0) ? std::max(0, want_fps - c.fps) : 0;
        const int       exact   = (want_w > 0 && want_h > 0
                                   && c.width == want_w && c.height == want_h) ? 0 : 1;
        const int       rank    = FormatLatencyRank(c.format, true);
        const long long px      = static_cast<long long>(c.width) * c.height;
        const long long dpx     = std::llabs(px - static_cast<long long>(want_w) * want_h);
        return std::make_tuple(exact, fps_ok, fps_gap, rank, dpx, -c.fps);
    };

    const Candidate* best = &cands.front();
    auto best_score = score(*best);
    for (const auto& c : cands)
    {
        auto s = score(c);
        if (s < best_score) { best_score = s; best = &c; }
    }

    format = best->format;
    width  = best->width;
    height = best->height;
    fps    = best->fps;

    if (out_reason)
    {
        std::ostringstream os;
        os << "设备能力内选优: " << format << " " << width << "x" << height
           << "@" << fps;
        if (want_w > 0 && want_h > 0 && (width != want_w || height != want_h))
            os << " (设备在目标帧率下没有 " << want_w << "x" << want_h
               << ", 最接近的是 " << width << "x" << height << ")";
        else if (want_fps > 0 && fps < want_fps)
            os << " (该组合下设备最高只能到 " << fps << "fps, 目标 "
               << want_fps << "fps 达不到)";
        else if (format == "MJPG")
            os << " (未压缩格式在目标分辨率/帧率下不可用, 改用 MJPG 换帧率)";
        else
            os << " (未压缩格式, 无编解码往返)";
        *out_reason = os.str();
    }
    return true;
}

inline std::string Describe(const MFDeviceInfo& dev)
{
    if (dev.caps.empty())
        return !dev.probe_error.empty() ? dev.probe_error
             : dev.caps_probed ? "(device reported no video capabilities)" : "(not probed)";

    std::ostringstream os;
    bool first = true;
    for (const auto& c : dev.caps)
    {
        if (!first) os << ", ";
        first = false;
        os << c.format << " " << c.width << "x" << c.height << "@";
        for (size_t i = 0; i < c.fps.size(); ++i)
        {
            if (i) os << "/";
            os << c.fps[i];
        }
        if (!c.supported) os << " (unsupported)";
    }
    return os.str();
}

}
