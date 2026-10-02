#pragma once
#include "mouse_driver.h"
#include <mutex>
#include <set>
#include <bitset>

namespace mouse_driver {
// Reads user/external transitions. Our tagged SendInput events cannot retrigger macros.
bool windowsPhysicalKeyPressed(int virtualKey);
int64_t windowsWheelCounter(bool up);
bool windowsTypeText(const std::string& utf8);
bool windowsMoveAbsolute(int x,int y);
bool windowsSetBlockedHotkeys(const std::bitset<256>& keys);
class WindowsDriver final : public IDriver {
public:
    WindowsDriver();
    ~WindowsDriver() override;
    const char* name() const override { return "WINDOWS"; }
    uint32_t capabilities() const override;
    bool isOpen() const override;
    std::string lastError() const override;
    bool move(int dx,int dy) override;
    bool leftDown() override { return button(1,true); }
    bool leftUp() override { return button(1,false); }
    bool rightDown() override { return button(2,true); }
    bool rightUp() override { return button(2,false); }
    bool middleDown() override { return button(3,true); }
    bool middleUp() override { return button(3,false); }
    bool wheel(int delta) override;
    bool tapKey(int hidKey,int holdMs,int mod=0) override;
    bool keyDown(int hidKey) override;
    bool keyUp(int hidKey) override;
    bool button(int channel,bool down);
    int physicalButtonPressed(int button) const override;
    bool directSend() const override { return true; }
private:
    bool key(int hidKey,bool down);
    bool result(unsigned int sent);
    mutable std::recursive_mutex mutex_;
    std::set<int> heldKeys_, heldButtons_;
    std::string error_;
};
} // namespace mouse_driver
