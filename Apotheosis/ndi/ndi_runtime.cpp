#include "ndi_runtime.h"
#include <filesystem>
#include <mutex>
#include <vector>

namespace ndi {
std::shared_ptr<Runtime> Runtime::Load(std::string& error) {
    static std::mutex mutex;
    static std::shared_ptr<Runtime> instance;
    std::lock_guard lock(mutex);
    error.clear();
    if (instance) return instance;
    auto rt = std::shared_ptr<Runtime>(new Runtime);
    wchar_t exe[32768]{};
    GetModuleFileNameW(nullptr, exe, static_cast<DWORD>(std::size(exe)));
    std::vector<std::filesystem::path> paths{
        std::filesystem::path(exe).parent_path() / L"Processing.NDI.Lib.x64.dll"};
    for (const auto* env : {L"NDI_RUNTIME_DIR_V6", L"NDI_RUNTIME_DIR_V5"}) {
        wchar_t folder[32768]{};
        const DWORD n = GetEnvironmentVariableW(env, folder, static_cast<DWORD>(std::size(folder)));
        if (n > 0 && n < std::size(folder))
            paths.push_back(std::filesystem::path(folder) / L"Processing.NDI.Lib.x64.dll");
    }
    for (const auto& path : paths) {
        rt->module_ = LoadLibraryExW(path.c_str(), nullptr,
            LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if (rt->module_) break;
    }
    if (!rt->module_) {
        error = "NDI runtime not found. Install NDI 6 Runtime (https://ndi.link/NDIRedistV6), "
                "or put Processing.NDI.Lib.x64.dll beside the executable.";
        return {};
    }
#define LOAD(name) rt->name = reinterpret_cast<decltype(rt->name)>(GetProcAddress(rt->module_, "NDIlib_" #name)); \
    if (!rt->name) { error = "NDI runtime is missing NDIlib_" #name; return {}; }
    LOAD(initialize) LOAD(destroy)
    LOAD(find_create_v2) LOAD(find_destroy) LOAD(find_wait_for_sources) LOAD(find_get_current_sources)
    LOAD(recv_create_v3) LOAD(recv_destroy) LOAD(recv_connect) LOAD(recv_capture_v3)
    LOAD(recv_free_video_v2) LOAD(recv_get_no_connections)
    LOAD(send_create) LOAD(send_destroy) LOAD(send_send_video_async_v2)
    LOAD(send_get_no_connections) LOAD(send_get_source_name)
#undef LOAD
    if (!rt->initialize()) { error = "NDI initialization failed (check CPU support/runtime)."; return {}; }
    rt->initialized_ = true;
    instance = rt;
    return rt;
}
Runtime::~Runtime() {
    if (initialized_) destroy();
    if (module_) FreeLibrary(module_);
}
}
