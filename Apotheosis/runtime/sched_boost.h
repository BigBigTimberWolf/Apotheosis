#pragma once

namespace sched_boost
{
bool boostProcessPriority();

void* registerCurrentThreadWithMmcss(const char* taskName);

void unregisterCurrentThread(void* handle);

class ScopedThreadBoost
{
public:
    explicit ScopedThreadBoost(const char* taskName);
    ~ScopedThreadBoost();

    ScopedThreadBoost(const ScopedThreadBoost&) = delete;
    ScopedThreadBoost& operator=(const ScopedThreadBoost&) = delete;

    bool active() const { return handle_ != nullptr; }

private:
    void* handle_ = nullptr;
};
}
