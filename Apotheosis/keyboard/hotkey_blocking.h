#pragma once
#include <memory>
#include <string>
class Config;

namespace hotkey_blocking {
// Updates are serialized by the macro scheduler's state mutex. Arm before a
// press rather than reacting to a polled down that the game has already seen.
void update(const std::shared_ptr<const Config>& config, bool macroEditing);
void clear();
std::string status();
std::string axisStatus();
}
