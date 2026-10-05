#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include "mouse/cpbox_driver.h"

#include <algorithm>
#include <cstring>
#include <filesystem>

namespace mouse_driver {
namespace {
using OpenFn = const char* (__cdecl*)(const char*, int, int);
using CloseFn = void (__cdecl*)();
using LastErrorFn = const char* (__cdecl*)();
using MoveFn = int (__cdecl*)(int, int);
using AxisMaskFn = int (__cdecl*)(int);
using ListenFn = int (__cdecl*)();
using ActionFn = int (__cdecl*)();

template <typename T>
T symbol(HMODULE module, const char* name)
{
    return reinterpret_cast<T>(GetProcAddress(module, name));
}
} // namespace

struct CpboxDriver::Api
{
    OpenFn open{};
    CloseFn close{};
    LastErrorFn lastError{};
    MoveFn move{};
    AxisMaskFn setAxisMask{};
    ListenFn listenLeft{}, listenRight{}, listenMid{}, listenSide1{}, listenSide2{};
    ActionFn downLeft{}, upLeft{}, downRight{}, upRight{};
    ActionFn maskUpLeft{}, maskUpRight{}, unmaskLeft{}, unmaskRight{}, clearForce{};
};

CpboxDriver::CpboxDriver(std::string port, int baud, int timeoutMs)
    : api_(std::make_unique<Api>())
{
    if (port.empty()) {
        lastError_ = "CPBox serial port is not configured";
        return;
    }
    if (!loadApi()) return;

    const char* result = api_->open(port.c_str(), baud, timeoutMs);
    if (!result || std::strcmp(result, "success") != 0) {
        lastError_ = result ? result : "cpbox_open returned a null result";
        FreeLibrary(static_cast<HMODULE>(module_));
        module_ = nullptr;
        api_.reset();
        return;
    }
    open_ = true;
    api_->setAxisMask(0);
}

CpboxDriver::~CpboxDriver()
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (open_ && api_) {
        api_->clearForce();
        api_->setAxisMask(0);
        api_->close();
        open_ = false;
    }
    if (module_) {
        FreeLibrary(static_cast<HMODULE>(module_));
        module_ = nullptr;
    }
}

bool CpboxDriver::loadApi()
{
    std::array<wchar_t, 32768> path{};
    const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (!length || length >= path.size()) {
        lastError_ = "Cannot locate application directory for cpbox_mouse_dll.dll";
        return false;
    }
    const auto dllPath = std::filesystem::path(path.data()).parent_path() / L"cpbox_mouse_dll.dll";
    HMODULE module = LoadLibraryW(dllPath.c_str());
    if (!module) module = LoadLibraryW(L"cpbox_mouse_dll.dll");
    if (!module) {
        lastError_ = "cpbox_mouse_dll.dll not found beside the application";
        return false;
    }

    module_ = module;
    api_->open = symbol<OpenFn>(module, "cpbox_open");
    api_->close = symbol<CloseFn>(module, "cpbox_close");
    api_->lastError = symbol<LastErrorFn>(module, "cpbox_last_error");
    api_->move = symbol<MoveFn>(module, "cpbox_move");
    api_->setAxisMask = symbol<AxisMaskFn>(module, "cpbox_set_axis_mask");
    api_->listenLeft = symbol<ListenFn>(module, "cpbox_listen_left");
    api_->listenRight = symbol<ListenFn>(module, "cpbox_listen_right");
    api_->listenMid = symbol<ListenFn>(module, "cpbox_listen_mid");
    api_->listenSide1 = symbol<ListenFn>(module, "cpbox_listen_side1");
    api_->listenSide2 = symbol<ListenFn>(module, "cpbox_listen_side2");
    api_->downLeft = symbol<ActionFn>(module, "cpbox_down_left");
    api_->upLeft = symbol<ActionFn>(module, "cpbox_up_left");
    api_->downRight = symbol<ActionFn>(module, "cpbox_down_right");
    api_->upRight = symbol<ActionFn>(module, "cpbox_up_right");
    api_->maskUpLeft = symbol<ActionFn>(module, "cpbox_mask_up_left");
    api_->maskUpRight = symbol<ActionFn>(module, "cpbox_mask_up_right");
    api_->unmaskLeft = symbol<ActionFn>(module, "cpbox_unmask_left");
    api_->unmaskRight = symbol<ActionFn>(module, "cpbox_unmask_right");
    api_->clearForce = symbol<ActionFn>(module, "cpbox_clear_force");

    if (!api_->open || !api_->close || !api_->lastError || !api_->move ||
        !api_->setAxisMask || !api_->listenLeft || !api_->listenRight ||
        !api_->listenMid || !api_->listenSide1 || !api_->listenSide2 ||
        !api_->downLeft || !api_->upLeft || !api_->downRight || !api_->upRight ||
        !api_->maskUpLeft || !api_->maskUpRight || !api_->unmaskLeft ||
        !api_->unmaskRight || !api_->clearForce) {
        lastError_ = "cpbox_mouse_dll.dll is missing required SDK exports";
        FreeLibrary(module);
        module_ = nullptr;
        api_ = std::make_unique<Api>();
        return false;
    }
    return true;
}

uint32_t CpboxDriver::capabilities() const
{
    return kCapMove | kCapButtonLeft | kCapButtonRight | kCapPhysicalRead;
}

bool CpboxDriver::isOpen() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return open_;
}

std::string CpboxDriver::lastError() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return lastError_;
}

void CpboxDriver::updateError()
{
    const char* error = api_ && api_->lastError ? api_->lastError() : nullptr;
    lastError_ = error ? error : "CPBox SDK operation failed";
}

bool CpboxDriver::actionResult(int result)
{
    if (result == 1) {
        lastError_.clear();
        return true;
    }
    updateError();
    return false;
}

bool CpboxDriver::move(int dx, int dy)
{
    std::lock_guard<std::mutex> lock(mutex_);
    return open_ && actionResult(api_->move(std::clamp(dx, -32768, 32767),
                                            std::clamp(dy, -32768, 32767)));
}

bool CpboxDriver::leftDown()
{
    std::lock_guard<std::mutex> lock(mutex_);
    return open_ && actionResult(api_->downLeft());
}

bool CpboxDriver::leftUp()
{
    std::lock_guard<std::mutex> lock(mutex_);
    return open_ && actionResult(api_->upLeft());
}

bool CpboxDriver::rightDown()
{
    std::lock_guard<std::mutex> lock(mutex_);
    return open_ && actionResult(api_->downRight());
}

bool CpboxDriver::rightUp()
{
    std::lock_guard<std::mutex> lock(mutex_);
    return open_ && actionResult(api_->upRight());
}

bool CpboxDriver::button(int button, bool down)
{
    if (button == 1) return down ? leftDown() : leftUp();
    if (button == 2) return down ? rightDown() : rightUp();
    return false;
}

int CpboxDriver::physicalButtonPressed(int button) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!open_) return -1;
    switch (button) {
    case 1: return api_->listenLeft();
    case 2: return api_->listenRight();
    case 3: return api_->listenMid();
    case 4: return api_->listenSide1();
    case 5: return api_->listenSide2();
    default: return -1;
    }
}

bool CpboxDriver::maskPhysicalButton(int button, bool masked)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!open_) return false;
    if (button == 1) {
        if (masked) return actionResult(api_->maskUpLeft());
        return actionResult(api_->unmaskLeft());
    }
    if (button == 2) {
        if (masked) return actionResult(api_->maskUpRight());
        return actionResult(api_->unmaskRight());
    }
    return !masked;
}

bool CpboxDriver::maskPhysicalAxis(int axis, bool masked)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!open_ || axis < 0 || axis > 1) return false;
    const int next = masked ? (axisMask_ | (1 << axis)) : (axisMask_ & ~(1 << axis));
    if (!actionResult(api_->setAxisMask(next))) return false;
    axisMask_ = next;
    return true;
}

} // namespace mouse_driver
