#pragma once
#include <windows.h>
#include <Processing.NDI.Lib.h>
#include <memory>
#include <string>

namespace ndi {
// One process-wide SDK initialization. Successful loads remain alive until
// process exit; unsuccessful loads can be retried after installing the runtime.
// No NDI import library is linked into either executable.
class Runtime {
public:
    static std::shared_ptr<Runtime> Load(std::string& error);
    ~Runtime();
#define NDI_ENTRY(name) decltype(&NDIlib_##name) name = nullptr
    NDI_ENTRY(initialize);
    NDI_ENTRY(destroy);
    NDI_ENTRY(find_create_v2);
    NDI_ENTRY(find_destroy);
    NDI_ENTRY(find_wait_for_sources);
    NDI_ENTRY(find_get_current_sources);
    NDI_ENTRY(recv_create_v3);
    NDI_ENTRY(recv_destroy);
    NDI_ENTRY(recv_connect);
    NDI_ENTRY(recv_capture_v3);
    NDI_ENTRY(recv_free_video_v2);
    NDI_ENTRY(recv_get_no_connections);
    NDI_ENTRY(send_create);
    NDI_ENTRY(send_destroy);
    NDI_ENTRY(send_send_video_async_v2);
    NDI_ENTRY(send_get_no_connections);
    NDI_ENTRY(send_get_source_name);
#undef NDI_ENTRY
private:
    Runtime() = default;
    HMODULE module_ = nullptr;
    bool initialized_ = false;
};
}
