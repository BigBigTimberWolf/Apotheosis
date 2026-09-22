
// 亲和性修复的独立验证 —— 直接调用 CPUAffinityManager, 检查实际拿到的核。
// 用法: affinity_check [请求核数, 默认 4]

#include "cpu_affinity_manager.h"

#include <cstdio>
#include <cstdlib>

int main(int argc, char** argv)
{
    SetConsoleOutputCP(CP_UTF8);
    setvbuf(stdout, nullptr, _IONBF, 0);

    const int want = (argc > 1) ? std::atoi(argv[1]) : 4;

    SYSTEM_INFO si;
    GetSystemInfo(&si);
    printf("本机逻辑核数: %lu\n\n", static_cast<unsigned long>(si.dwNumberOfProcessors));

    CPUAffinityManager mgr;
    mgr.reserveCPUCores(want);
    printf("\n");

    // 读回真实生效的亲和性, 不依赖函数自己的打印。
    DWORD_PTR procMask = 0, sysMask = 0;
    if (GetProcessAffinityMask(GetCurrentProcess(), &procMask, &sysMask))
    {
        printf("=== 实测生效的进程亲和性 ===\n");
        printf("  进程 mask = 0x%llX   (系统 mask = 0x%llX)\n",
               static_cast<unsigned long long>(procMask),
               static_cast<unsigned long long>(sysMask));

        int n = 0;
        printf("  允许的逻辑核:");
        for (int i = 0; i < 64; ++i)
            if (procMask & (static_cast<DWORD_PTR>(1) << i)) { printf(" %d", i); ++n; }
        printf("\n  共 %d 个\n\n", n);

        const DWORD_PTR expectAll = (static_cast<DWORD_PTR>(1) << si.dwNumberOfProcessors) - 1;
        if (want >= static_cast<int>(si.dwNumberOfProcessors))
        {
            if (procMask == expectAll)
                printf(">>> 通过: 请求 %d 个核, 实际拿到全部 %d 个 (0x%llX)。\n",
                       want, n, static_cast<unsigned long long>(procMask));
            else
                printf(">>> 失败: 请求全部核, 但只拿到 %d 个 (0x%llX, 期望 0x%llX)。\n",
                       n, static_cast<unsigned long long>(procMask),
                       static_cast<unsigned long long>(expectAll));
        }
        else
        {
            const int bits = [](DWORD_PTR m) { int c = 0; while (m) { c += (m & 1); m >>= 1; } return c; }(procMask);
            if (bits == want)
                printf(">>> 通过: 请求 %d 个核, 实际 %d 个。\n", want, bits);
            else
                printf(">>> 失败: 请求 %d 个核, 实际 %d 个。\n", want, bits);
        }
    }
    else
    {
        printf("[FAIL] GetProcessAffinityMask 失败, gle=%lu\n", GetLastError());
        return 1;
    }

    return 0;
}
