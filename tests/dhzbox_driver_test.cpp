#include "mouse/dhzbox_driver.h"
#include <ws2tcpip.h>
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

int main() {
    using mouse_driver::DhzboxMiniDriver;
    assert(DhzboxMiniDriver::encode("move(12,-3)", 1) == "npwf(12,-3)");
    assert(DhzboxMiniDriver::encode("Left(1)", 26) == "Left(1)");

    WSADATA wsa{};
    assert(WSAStartup(MAKEWORD(2, 2), &wsa) == 0);
    SOCKET receiver = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    assert(receiver != INVALID_SOCKET);
    sockaddr_in local{};
    local.sin_family = AF_INET;
    inet_pton(AF_INET, "127.0.0.1", &local.sin_addr);
    assert(bind(receiver, reinterpret_cast<sockaddr*>(&local), sizeof(local)) == 0);
    int len = sizeof(local);
    assert(getsockname(receiver, reinterpret_cast<sockaddr*>(&local), &len) == 0);
    DWORD timeout = 1000;
    setsockopt(receiver, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
    {
        DhzboxMiniDriver driver("127.0.0.1", ntohs(local.sin_port), 0);
        assert(driver.isOpen());
        char buf[80]{};
        int n = recv(receiver, buf, sizeof(buf) - 1, 0);
        assert(n > 0);
        const std::string monitor(buf, n);
        assert(monitor.rfind("monitor(", 0) == 0);
        const unsigned short monitorPort = static_cast<unsigned short>(std::stoi(monitor.substr(8)));
        sockaddr_in monitorDest = local;
        monitorDest.sin_port = htons(monitorPort);
        const std::string state = "0|0|1|0|0|";
        assert(sendto(receiver, state.data(), static_cast<int>(state.size()), 0,
                      reinterpret_cast<const sockaddr*>(&monitorDest), sizeof(monitorDest)) > 0);
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        assert(driver.physicalButtonPressed(2) == 1);
        assert(driver.move(300, -1));
        int sumX = 0, sumY = 0;
        for (int i = 0; i < 3; ++i) {
            n = recv(receiver, buf, sizeof(buf) - 1, 0);
            assert(n > 0);
            buf[n] = 0;
            int x, y;
            assert(std::sscanf(buf, "move(%d,%d)", &x, &y) == 2);
            assert(x <= 127 && x >= -127 && y <= 127 && y >= -127);
            sumX += x; sumY += y;
        }
        assert(sumX == 300 && sumY == -1);
    }
    closesocket(receiver);
    WSACleanup();
}
