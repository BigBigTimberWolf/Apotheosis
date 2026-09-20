#ifndef RUNTIME_INFERENCE_SESSION_H
#define RUNTIME_INFERENCE_SESSION_H

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

class IDetector;

namespace runtime
{

bool preload_model_metadata(const std::string& model_path, bool persist_config, std::string* error = nullptr);

class InferenceSession
{
public:
    InferenceSession();
    ~InferenceSession();

    InferenceSession(const InferenceSession&) = delete;
    InferenceSession& operator=(const InferenceSession&) = delete;

    bool start(const std::string& backend, const std::string& model_path);
    void stop();

    bool running() const noexcept { return running_.load(std::memory_order_acquire); }
    std::string last_error() const;

private:
    void join_all_locked();

    void stop_locked();
    mutable std::mutex mutex_;
    std::atomic<bool> running_{false};

    std::unique_ptr<IDetector> detector_owned_;
    IDetector* detector_raw_ = nullptr;

    std::thread capture_thread_;
    std::thread detector_thread_;
    std::thread heartbeat_thread_;

    std::string current_backend_;
    std::string current_model_path_;
    std::string last_error_;
};

}

#endif // RUNTIME_INFERENCE_SESSION_H
