#include "sched_boost.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <avrt.h>

#include <iostream>

#pragma comment(lib, "avrt.lib")

namespace sched_boost
{

bool boostProcessPriority()
{
    if (SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS))
        return true;

    std::cerr << "[Sched] SetPriorityClass(HIGH) failed, gle=" << GetLastError()
              << " (继续以默认优先级运行)" << std::endl;
    return false;
}

void* registerCurrentThreadWithMmcss(const char* taskName)
{
    const wchar_t* task = L"Games";
    wchar_t wide[128] = {};
    if (taskName && *taskName)
    {
        const int n = MultiByteToWideChar(CP_UTF8, 0, taskName, -1, wide,
                                          static_cast<int>(std::size(wide)));
        if (n > 0)
            task = wide;
    }

    DWORD taskIndex = 0;
    HANDLE h = AvSetMmThreadCharacteristicsW(task, &taskIndex);
    if (!h)
    {
        std::cerr << "[Sched] AvSetMmThreadCharacteristics failed, gle="
                  << GetLastError() << " (推理线程按普通优先级运行)" << std::endl;
        return nullptr;
    }

    if (!AvSetMmThreadPriority(h, AVRT_PRIORITY_CRITICAL))
    {
        std::cerr << "[Sched] AvSetMmThreadPriority failed, gle=" << GetLastError()
                  << " (保持 MMCSS 默认档)" << std::endl;
    }

    return h;
}

void unregisterCurrentThread(void* handle)
{
    if (handle)
        AvRevertMmThreadCharacteristics(static_cast<HANDLE>(handle));
}

ScopedThreadBoost::ScopedThreadBoost(const char* taskName)
{
    handle_ = registerCurrentThreadWithMmcss(taskName);
}

ScopedThreadBoost::~ScopedThreadBoost()
{
    unregisterCurrentThread(handle_);
    handle_ = nullptr;
}

}
