#ifndef RUNTIME_ACTIVE_HOTKEY_H
#define RUNTIME_ACTIVE_HOTKEY_H

#include <atomic>
#include <mutex>

#include "detector/model_inspector.h"

namespace runtime
{

extern std::atomic<int> g_active_hotkey_index;
extern std::atomic<int> g_secondary_aim_hotkey_index;
extern std::atomic<bool> g_hotkey_activation_dirty;

extern std::mutex g_model_metadata_mutex;
extern detector::ModelMetadata g_model_metadata;

}

#endif // RUNTIME_ACTIVE_HOTKEY_H
