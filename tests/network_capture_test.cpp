#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>

#include "capture/stream_capture.h"

#include <opencv2/videoio.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

namespace {

std::vector<char> makeTransportStream()
{
    const auto path = std::filesystem::temp_directory_path() /
        "apotheosis_network_capture_test.ts";
    cv::VideoWriter writer;
    const int codecs[] = {
        cv::VideoWriter::fourcc('m', 'p', '4', 'v'),
        cv::VideoWriter::fourcc('M', 'P', 'G', '2'),
        0,
    };
    for (const int codec : codecs)
        if (writer.open(path.string(), cv::CAP_FFMPEG, codec, 30,
                        cv::Size(320, 240)))
            break;
    if (!writer.isOpened()) return {};
    for (int i = 0; i < 90; ++i)
    {
        cv::Mat frame(240, 320, CV_8UC3,
                      cv::Scalar(i * 2 % 256, 60, 180));
        writer.write(frame);
    }
    writer.release();
    std::ifstream file(path, std::ios::binary);
    std::vector<char> bytes((std::istreambuf_iterator<char>(file)), {});
    std::error_code ec;
    std::filesystem::remove(path, ec);
    return bytes;
}

void sendUdp(const std::vector<char>& bytes, unsigned short port,
             const std::atomic<bool>& done)
{
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return;
    sockaddr_in dest{};
    dest.sin_family = AF_INET;
    dest.sin_port = htons(port);
    dest.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    for (int repeat = 0; repeat < 200 && !done.load(); ++repeat)
    {
        for (size_t offset = 0; offset < bytes.size(); offset += 1316)
        {
            const int size = static_cast<int>(std::min<size_t>(1316, bytes.size() - offset));
            sendto(s, bytes.data() + offset, size, 0,
                   reinterpret_cast<const sockaddr*>(&dest), sizeof(dest));
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(15));
    }
    closesocket(s);
}

void sendTcp(const std::vector<char>& bytes, unsigned short port)
{
    SOCKET s = INVALID_SOCKET;
    sockaddr_in dest{};
    dest.sin_family = AF_INET;
    dest.sin_port = htons(port);
    dest.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    for (int attempt = 0; attempt < 40; ++attempt)
    {
        s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (s != INVALID_SOCKET && connect(s,
            reinterpret_cast<const sockaddr*>(&dest), sizeof(dest)) == 0)
            break;
        if (s != INVALID_SOCKET) closesocket(s);
        s = INVALID_SOCKET;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    if (s == INVALID_SOCKET) return;
    for (size_t offset = 0; offset < bytes.size();)
    {
        const int size = static_cast<int>(std::min<size_t>(1316, bytes.size() - offset));
        const int sent = send(s, bytes.data() + offset, size, 0);
        if (sent <= 0) break;
        offset += static_cast<size_t>(sent);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    shutdown(s, SD_SEND);
    closesocket(s);
}

bool receiveOne(const std::string& source, unsigned short port,
                const std::vector<char>& bytes, int senderDelayMs = 400)
{
    const std::string url = source == "udp"
        ? "udp://0.0.0.0:" + std::to_string(port) + "?fifo_size=500000&overrun_nonfatal=1"
        : "tcp://0.0.0.0:" + std::to_string(port);
    auto capture = stream_capture::Create(source, url, 160);
    if (!capture) return false;
    std::atomic<bool> done{false};
    std::thread sender([&] {
        if (source == "udp") sendUdp(bytes, port, done);
        else
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(senderDelayMs));
            sendTcp(bytes, port);
        }
    });
    bool received = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(12);
    while (std::chrono::steady_clock::now() < deadline)
    {
        capture->WaitFrame(30);
        cv::Mat frame = capture->GetNextFrameCpu();
        if (!frame.empty() && frame.cols == 160 && frame.rows == 160 &&
            capture->GetLastFrameCaptureNs() > 0)
        {
            received = true;
            break;
        }
    }
    done.store(true);
    sender.join();
    return received;
}

} // namespace

int main()
{
    WSADATA winsock{};
    if (WSAStartup(MAKEWORD(2, 2), &winsock) != 0) return 1;
    const auto bytes = makeTransportStream();
    if (bytes.empty())
    {
        std::puts("FAIL: FFmpeg MPEG-TS encoder unavailable");
        WSACleanup();
        return 1;
    }
    std::printf("fixture=%zu bytes\n", bytes.size());
    std::string reason;
    if (!stream_capture::ValidateUrl("tcp", "tcp://0.0.0.0:32472", &reason) ||
        stream_capture::ValidateUrl("tcp", "tcp://0.0.0.0:32472?listen=0", &reason))
    {
        std::puts("FAIL: TCP listening URL validation");
        WSACleanup();
        return 1;
    }
    const bool udp = receiveOne("udp", 32471, bytes);
    // Start the receiver first, let its first OpenCV accept time out, then
    // start the sender exactly as OBS would after the app is already running.
    const bool tcp = receiveOne("tcp", 32472, bytes, 4500);
    std::printf("UDP=%s TCP=%s\n", udp ? "OK" : "FAIL", tcp ? "OK" : "FAIL");
    WSACleanup();
    return udp && tcp ? 0 : 1;
}
