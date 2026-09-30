#pragma once

#include "mouse_driver.h"
#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <windows.h>

namespace mouse_driver {
class FerrumDriver final : public IDriver {
public:
    FerrumDriver(const std::string& port, unsigned int baud);
    ~FerrumDriver() override;
    const char* name() const override { return "FERRUM"; }
    uint32_t capabilities() const override {
        return kCapMove | kCapButtonLeft | kCapButtonRight | kCapButtonMiddle |
               kCapButtonSide | kCapWheel | kCapPhysicalRead | (softwareApi_ ? kCapKeyboard : 0u);
    }
    bool isOpen() const override { return open_.load(); }
    std::string lastError() const override { return error_; }
    bool move(int dx, int dy) override;
    bool button(int b, bool down) override;
    bool keyDown(int hid) override;
    bool keyUp(int hid) override;
    bool tapKey(int hid, int holdMs, int modifiers=0) override;
    bool leftDown() override { return send("km.left(1)"); }
    bool leftUp() override { return send("km.left(0)"); }
    bool rightDown() override { return send("km.right(1)"); }
    bool rightUp() override { return send("km.right(0)"); }
    bool middleDown() override { return send("km.middle(1)"); }
    bool middleUp() override { return send("km.middle(0)"); }
    bool wheel(int delta) override;
    int physicalButtonPressed(int button) const override;
    int physicalKeyPressed(int hid) const override;
    bool maskPhysicalButton(int button, bool enabled) override;
    bool maskPhysicalAxis(int axis, bool enabled) override;
    bool maskPhysicalKey(int hid, bool enabled) override;
    // Serial writes can block on the USB bridge. Keep that wait off the aim thread.
    bool directSend() const override { return false; }
private:
    bool send(const std::string& command);
    void readLoop();
    HANDLE serial_ = INVALID_HANDLE_VALUE;
    bool overlapped_ = false;
    std::atomic<bool> open_{false};
    std::atomic<bool> stop_{false};
    std::atomic<int> buttons_{-1};
    std::thread reader_;
    std::array<bool,2> ownedAxisMasks_{};
    std::mutex writeMutex_;
    std::atomic<long long> lastSlowWriteLogMs_{0};
    std::string error_;
    std::array<std::atomic<bool>,256> physicalKeys_{};
    std::atomic<bool> keysReady_{false};
    std::array<bool,6> ownedButtonMasks_{};
    std::array<bool,256> ownedKeyMasks_{};
    std::array<bool,256> injectedKeys_{};
    bool softwareApi_=false;
};
}
