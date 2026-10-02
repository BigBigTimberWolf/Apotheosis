#pragma once
#include <mutex>
#include <string>

namespace macros {
struct Status {
    bool running = false;
    std::string id, message = u8"宏编排已关闭";
    int step = 0, total = 0;
};
void tick(); // Called by the keyboard thread. Never sleeps for action delays.
void stopAll();
void shutdown();
void runOnce(const std::string& id);
void simulate(const std::string& id);
std::string ruleDiagnostics();
// Queue a short keyboard/mouse press without blocking the aim loop.
void requestAutoFlash(const std::string& key, int aimHotkeyIndex);
void cancelAutoFlash();
void setEditing(bool editing);
Status status();
bool ownsOutput();
std::recursive_mutex& outputMutex();
// Stop/release before connections are replaced; prevent restarts during reconnect.
class DevicePause {
public:
    DevicePause();
    ~DevicePause();
    DevicePause(const DevicePause&) = delete;
    DevicePause& operator=(const DevicePause&) = delete;
};
} // namespace macros
