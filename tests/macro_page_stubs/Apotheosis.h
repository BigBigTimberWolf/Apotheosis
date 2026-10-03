#pragma once
// Test stand-in for the application's global header: only what the macro page uses.
#include "config/config.h"
#include <mutex>
extern Config config;
extern std::recursive_mutex configMutex;
