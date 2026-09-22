#include "cpu_affinity_manager.h"

#include <iostream>

bool CPUAffinityManager::reserveCPUCores(int numCores)
{
    SYSTEM_INFO sysInfo;
    GetSystemInfo(&sysInfo);
    const DWORD totalCores = sysInfo.dwNumberOfProcessors;

    // <= 0 表示"不限制", 保持系统默认亲和性 (所有核可用)。
    if (numCores <= 0)
    {
        std::cout << "[CPU] cpuCoreReserveCount=" << numCores
                  << " -> 不做亲和性限制, 进程可用全部 " << totalCores
                  << " 个逻辑核。" << std::endl;
        return true;
    }

    // ★ 旧实现的 bug: 循环写成 `for (i = 2; i < totalCores && count < numCores; ...)`,
    //   在 4 核机器上请求 4 个核时 i 只能取 2、3, 于是 mask=0xC —— 只拿到 2 个核,
    //   CPU 0/1 完全闲置。请求数与实际数被静默截断, 是"解码跑不满 240fps"的根因之一。
    //   现在显式做 min(请求, 可用), 并把截断情况打印出来。
    const DWORD want = (static_cast<DWORD>(numCores) < totalCores)
                     ? static_cast<DWORD>(numCores) : totalCores;

    // 取编号最高的 want 个核: 保留"把低位核留给系统/其他进程"的原始意图,
    // 同时在 want == totalCores 时自然覆盖全部核 (4 核 + 请求 4 -> 0xF)。
    DWORD_PTR mask = 0;
    for (DWORD i = totalCores - want; i < totalCores; ++i)
        mask |= (static_cast<DWORD_PTR>(1) << i);

    if (mask == 0) mask = 1;

    if (want < static_cast<DWORD>(numCores))
    {
        std::cerr << "[CPU] cpuCoreReserveCount=" << numCores
                  << " 超过本机可用逻辑核 " << totalCores
                  << ", 实际只保留 " << want << " 个。" << std::endl;
    }

    originalMask = SetProcessAffinityMask(GetCurrentProcess(), mask);
    if (originalMask == 0)
    {
        // 进程级设置失败时退回线程级, 至少让当前线程落在指定的核上。
        if (!SetThreadAffinityMask(GetCurrentThread(), mask))
            std::cerr << "[CPU] 进程与线程亲和性设置均失败, GetLastError="
                      << GetLastError() << std::endl;
    }

    if (!SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS))
    {
        std::cerr << "[CPU] Failed to set process priority. GetLastError="
                  << GetLastError() << std::endl;
    }

    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);

    std::cout << "[CPU] Affinity tuned: mask=0x" << std::hex << mask << std::dec
              << " -> 绑定 " << want << "/" << totalCores << " 个逻辑核 (";
    for (DWORD i = 0; i < totalCores; ++i)
        if (mask & (static_cast<DWORD_PTR>(1) << i))
            std::cout << " " << i;
    std::cout << " ) with HIGH priority." << std::endl;

    return true;
}

bool CPUAffinityManager::reserveSystemMemory(size_t reservedMemoryMB)
{
    size_t reservedSize = reservedMemoryMB * 1024 * 1024;

    reservedMemory = malloc(reservedSize);
    if (reservedMemory)
    {
        memset(reservedMemory, 0, reservedSize);
        return true;
    }
    std::cerr << "[CPU] Failed to reserve system memory: " << reservedMemoryMB << " MB." << std::endl;
    return false;
}
