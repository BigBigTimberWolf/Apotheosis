#pragma once

#include "mouse_driver.h"

#include <memory>
#include <mutex>
#include <string>

namespace mouse_driver {

class CpboxDriver final : public IDriver
{
public:
    explicit CpboxDriver(std::string port, int baud = 115200, int timeoutMs = 20);
    ~CpboxDriver() override;

    const char* name() const override { return "CPBOX"; }
    uint32_t capabilities() const override;
    bool isOpen() const override;
    std::string lastError() const override;
    bool move(int dx, int dy) override;
    bool leftDown() override;
    bool leftUp() override;
    bool rightDown() override;
    bool rightUp() override;
    bool middleDown() override { return false; }
    bool middleUp() override { return false; }
    bool button(int button, bool down) override;
    int physicalButtonPressed(int button) const override;
    bool maskPhysicalButton(int button, bool masked) override;
    bool maskPhysicalAxis(int axis, bool masked) override;
    bool directSend() const override { return true; }

private:
    struct Api;
    bool loadApi();
    bool actionResult(int result);
    void updateError();

    mutable std::mutex mutex_;
    void* module_ = nullptr;
    std::unique_ptr<Api> api_;
    std::string lastError_;
    int axisMask_ = 0;
    bool open_ = false;
};

} // namespace mouse_driver
