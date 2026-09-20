#pragma once
#include <cstdint>
namespace runtime
{
struct FrameContext
{
    uint64_t sequence = 0;
    int64_t captured_ns = 0;
    int width = 0, height = 0;
};
}
