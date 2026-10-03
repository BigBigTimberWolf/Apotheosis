// Stand-ins for the application globals the macro page touches, so the real page
// can be built and driven in a test without the rest of the application. Nothing
// here simulates input, capture or inference.
#include "Apotheosis.h"
#include "config/config_bridge.h"
#include "config/config_profiles.h"
#include "macro/macro_engine.h"
#include "keyboard/hotkey_blocking.h"
#include <mutex>
Config config;
std::recursive_mutex configMutex;
ConfigBridge& ConfigBridge::instance() { static ConfigBridge b; return b; }
void ConfigBridge::markDirty() { ++dirtyCount; }
ConfigProfiles& ConfigProfiles::instance() { static ConfigProfiles p; return p; }
namespace macros {
static int g_stops = 0; static std::string g_ran, g_sim;
void stopAll() { ++g_stops; }
void runOnce(const std::string& id) { g_ran = id; }
void simulate(const std::string& id) { g_sim = id; }
std::string ruleDiagnostics() { return "diag"; }
void setEditing(bool) {}
Status status() { Status s; s.message = "stub engine"; return s; }
int testStopCount() { return g_stops; }
std::string testLastRun() { return g_ran; }
}
namespace hotkey_blocking { std::string status() { return "blocker idle"; } }
