#include "stream_capture.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <thread>
#include <vector>

namespace stream_capture {
namespace {

int64_t steadyNowNs()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

cv::Mat centerCrop(const cv::Mat& image, int side)
{
    if (image.empty()) return {};
    const int cropSide = std::min(image.cols, image.rows);
    const cv::Rect roi((image.cols - cropSide) / 2,
                       (image.rows - cropSide) / 2, cropSide, cropSide);
    cv::Mat square = image(roi);
    if (cropSide == side) return square.clone();
    if (cropSide > side)
    {
        const cv::Rect inner((cropSide - side) / 2,
                             (cropSide - side) / 2, side, side);
        return square(inner).clone();
    }
    cv::Mat resized;
    cv::resize(square, resized, cv::Size(side, side), 0.0, 0.0, cv::INTER_LINEAR);
    return resized;
}

class NetworkCapture final : public IScreenCapture
{
public:
    NetworkCapture(std::string url, int outputSide, bool tcp)
        : url_(std::move(url)), outputSide_(std::clamp(outputSide, 32, 2048)),
          tcp_(tcp),
          worker_([this] { run(); })
    {
    }

    ~NetworkCapture() override
    {
        stopping_.store(true);
        cv_.notify_all();
        if (worker_.joinable()) worker_.join();
    }

    cv::Mat GetNextFrameCpu() override
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (!hasFrame_) return {};
        hasFrame_ = false;
        deliveredNs_.store(pendingNs_);
        return std::move(pending_);
    }

    bool WaitFrame(int milliseconds) override
    {
        std::unique_lock<std::mutex> lock(mu_);
        return cv_.wait_for(lock, std::chrono::milliseconds(std::max(0, milliseconds)),
                            [this] { return hasFrame_ || stopping_.load(); })
            && hasFrame_;
    }

    bool SupportsEventWait() const override { return true; }
    bool HandlesTargetFps() const override { return true; }
    int GetSourceFpsEstimate() const override { return sourceFps_.load(); }
    int64_t GetLastFrameCaptureNs() const override { return deliveredNs_.load(); }

private:
    void pauseBeforeRetry()
    {
        std::unique_lock<std::mutex> lock(mu_);
        cv_.wait_for(lock, std::chrono::milliseconds(500),
                     [this] { return stopping_.load(); });
    }

    void run()
    {
        auto lastErrorLog = std::chrono::steady_clock::time_point{};
        if (tcp_)
            std::cout << "[Capture] TCP listening; waiting for OBS/FFmpeg sender."
                      << std::endl;
        while (!stopping_.load())
        {
            cv::VideoCapture stream;
            try
            {
                const auto openStarted = std::chrono::steady_clock::now();
                const std::vector<int> options{
                    cv::CAP_PROP_OPEN_TIMEOUT_MSEC, tcp_ ? 3000 : 2000,
                    cv::CAP_PROP_READ_TIMEOUT_MSEC, 500,
                };
                if (!stream.open(url_, cv::CAP_FFMPEG, options))
                {
                    const auto now = std::chrono::steady_clock::now();
                    if (now - lastErrorLog >= std::chrono::seconds(5))
                    {
                        if (tcp_ && now - openStarted >= std::chrono::seconds(1))
                            std::cout << "[Capture] TCP waiting for OBS/FFmpeg sender."
                                      << std::endl;
                        else
                            std::cerr << (tcp_
                                ? "[Capture] TCP listen failed; check address and port."
                                : "[Capture] Network stream unavailable; waiting for sender.")
                                      << std::endl;
                        lastErrorLog = now;
                    }
                    sourceFps_.store(0);
                    // OpenCV times out a waiting TCP accept. Rebind at once so
                    // OBS can connect after Apotheosis starts; only back off
                    // when opening failed immediately (for example, port in use).
                    if (!tcp_ || now - openStarted < std::chrono::milliseconds(250))
                        pauseBeforeRetry();
                    continue;
                }

                std::cout << "[Capture] Network stream connected (FFmpeg)." << std::endl;
                int frameCount = 0;
                auto fpsStart = std::chrono::steady_clock::now();
                while (!stopping_.load())
                {
                    cv::Mat decoded;
                    if (!stream.read(decoded)) break;
                    if (decoded.empty()) continue;

                    cv::Mat cropped = centerCrop(decoded, outputSide_);
                    if (cropped.empty()) continue;
                    const int64_t timestamp = steadyNowNs();
                    {
                        std::lock_guard<std::mutex> lock(mu_);
                        pending_ = std::move(cropped); // latest frame wins
                        pendingNs_ = timestamp;
                        hasFrame_ = true;
                    }
                    cv_.notify_one();

                    ++frameCount;
                    const auto now = std::chrono::steady_clock::now();
                    const double elapsed = std::chrono::duration<double>(now - fpsStart).count();
                    if (elapsed >= 1.0)
                    {
                        sourceFps_.store(static_cast<int>(frameCount / elapsed));
                        frameCount = 0;
                        fpsStart = now;
                    }
                }
            }
            catch (const cv::Exception& e)
            {
                std::cerr << "[Capture] Network decoder error: " << e.what() << std::endl;
            }
            catch (const std::exception& e)
            {
                std::cerr << "[Capture] Network capture error: " << e.what() << std::endl;
            }
            sourceFps_.store(0);
            if (!stopping_.load())
            {
                std::cout << "[Capture] Network stream disconnected; reconnecting."
                          << std::endl;
                if (!tcp_) pauseBeforeRetry();
            }
        }
    }

    std::string url_;
    int outputSide_ = 320;
    bool tcp_ = false;
    std::atomic<bool> stopping_{false};
    std::atomic<int> sourceFps_{0};
    std::atomic<int64_t> deliveredNs_{0};
    std::mutex mu_;
    std::condition_variable cv_;
    cv::Mat pending_;
    int64_t pendingNs_ = 0;
    bool hasFrame_ = false;
    std::thread worker_;
};

} // namespace

bool IsNetworkSource(const std::string& source)
{
    return source == "udp" || source == "tcp";
}

namespace {

// FFmpeg's TCP protocol defaults to client mode. The capture side is always
// the server; the user does not need to know or save the FFmpeg listen option.
std::string tcpListenUrl(const std::string& url)
{
    const size_t query = url.find('?');
    if (query == std::string::npos) return url + "?listen=1";
    size_t at = query + 1;
    while (at < url.size())
    {
        const size_t next = url.find('&', at);
        if (url.substr(at, next - at) == "listen=1") return url;
        if (next == std::string::npos) break;
        at = next + 1;
    }
    if (url.back() == '?' || url.back() == '&') return url + "listen=1";
    return url + "&listen=1";
}

} // namespace

bool ValidateUrl(const std::string& source, const std::string& url,
                 std::string* reason)
{
    const auto fail = [reason](const char* text) {
        if (reason) *reason = text;
        return false;
    };
    if (!IsNetworkSource(source)) return fail("source must be tcp or udp");
    const std::string prefix = source + "://";
    if (url.rfind(prefix, 0) != 0 || url.find_first_of("\r\n \t") != std::string::npos)
        return fail("URL must start with the selected tcp:// or udp:// scheme");
    const size_t end = url.find('?');
    const std::string endpoint = url.substr(prefix.size(), end - prefix.size());
    const size_t colon = endpoint.rfind(':');
    if (colon == std::string::npos || colon + 1 >= endpoint.size())
        return fail("URL needs a port");
    if (endpoint.empty() || endpoint.front() == '@' || colon == 0)
        return fail("use an IP address such as 0.0.0.0 or 127.0.0.1");
    const std::string port = endpoint.substr(colon + 1);
    if (port.size() > 5) return fail("port out of range");
    if (!std::all_of(port.begin(), port.end(), [](unsigned char c) { return c >= '0' && c <= '9'; }))
        return fail("port must be numeric");
    int number = 0;
    for (char c : port)
    {
        number = number * 10 + (c - '0');
        if (number > 65535) return fail("port out of range");
    }
    if (number == 0) return fail("port out of range");
    if (source == "tcp" && end != std::string::npos)
    {
        size_t at = end + 1;
        while (at < url.size())
        {
            const size_t next = url.find('&', at);
            const std::string option = url.substr(at, next - at);
            if (option.rfind("listen=", 0) == 0 && option != "listen=1")
                return fail("TCP capture must listen; omit listen or use listen=1");
            if (next == std::string::npos) break;
            at = next + 1;
        }
    }
    if (reason) reason->clear();
    return true;
}

std::unique_ptr<IScreenCapture> Create(const std::string& source,
                                       const std::string& url, int outputSide)
{
    std::string reason;
    if (!ValidateUrl(source, url, &reason))
    {
        std::cerr << "[Capture] Invalid network URL: " << reason << std::endl;
        return nullptr;
    }
    return std::make_unique<NetworkCapture>(
        source == "tcp" ? tcpListenUrl(url) : url, outputSide, source == "tcp");
}

} // namespace stream_capture
