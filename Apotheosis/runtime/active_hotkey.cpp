#include "active_hotkey.h"

namespace runtime
{

std::atomic<int> g_active_hotkey_index{ -1 };
std::atomic<int> g_secondary_aim_hotkey_index{ -1 };
std::atomic<bool> g_hotkey_activation_dirty{ false };
std::mutex g_model_metadata_mutex;
detector::ModelMetadata g_model_metadata{};

}
