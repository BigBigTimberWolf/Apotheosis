#pragma once

#include <memory>

class Config;

namespace runtime_config
{
std::shared_ptr<const Config> read();
void publish();
}
