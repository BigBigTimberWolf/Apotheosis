#pragma once

#include "mouse_driver.h"
#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <winsock2.h>

namespace mouse_driver {
class DhzboxMiniDriver final : public IDriver {
public:
    DhzboxMiniDriver(const std::string& ip, unsigned short port, int key);
    ~DhzboxMiniDriver() override;
    const char* name() const override { return "DHZBOX_MINI"; }
    uint32_t capabilities() const override;
    bool isOpen() const override { return socket_ != INVALID_SOCKET; }
    std::string lastError() const override { return error_; }
    bool move(int dx, int dy) override;
    bool button(int b, bool down) override;
    bool leftDown() override { return send("left(1)"); }
    bool leftUp() override { return send("left(0)"); }
    bool rightDown() override { return send("right(1)"); }
    bool rightUp() override { return send("right(0)"); }
    bool middleDown() override { return send("middle(1)"); }
    bool middleUp() override { return send("middle(0)"); }
    bool wheel(int delta) override;
    int physicalButtonPressed(int button) const override;
    bool maskPhysicalButton(int button, bool enabled) override;
    bool maskPhysicalAxis(int axis, bool enabled) override;
    bool directSend() const override { return true; }
    static std::string encode(const std::string& command, int key);
private:
    bool send(const std::string& command);
    void monitorLoop();
    SOCKET socket_ = INVALID_SOCKET;
    SOCKET monitor_ = INVALID_SOCKET;
    sockaddr_in endpoint_{};
    std::string error_;
    int key_ = 88;
    std::atomic<bool> stop_{false};
    std::atomic<int> buttons_{-1};
    std::thread monitorThread_;
    std::array<bool,2> ownedAxisMasks_{};
    std::mutex sendMutex_;
    std::array<bool,6> ownedMasks_{};
};
}
