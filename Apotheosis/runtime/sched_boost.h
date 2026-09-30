#pragma once

#include <memory>
#include <string>

namespace sched_boost
{
bool boostProcessPriority(bool enabled = true);

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
    int original_priority_ = 0;
    bool priority_raised_ = false;
};

// Owned by the worker thread. Updates its scheduling when performance mode
// changes, and restores the original priority when the mode is disabled.
class LiveThreadBoost
{
public:
    void update(bool enabled, const char* taskName = "Games");

private:
    bool enabled_ = false;
    std::string task_name_;
    std::unique_ptr<ScopedThreadBoost> boost_;
};
}
