#define WIN32_LEAN_AND_MEAN
#define _WINSOCKAPI_
#include <winsock2.h>
#include <Windows.h>
#include <timeapi.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <exception>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <thread>

#include <QApplication>
#include "style/Theme.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QPalette>
#include <QStyleFactory>

#include "capture.h"
#include "capture/auto_capture.h"
#include "mouse.h"
#include "mouse/Makcu.h"
#include "mouse/MakcuNew.h"
#include "mouse/kmboxNetConnection.h"
#include "mouse/dhzbox_driver.h"
#include "mouse/ferrum_driver.h"
#include "mouse/cat_driver.h"
#include "mouse/cpbox_driver.h"
#include "mouse/windows_driver.h"
#include "Apotheosis.h"
#include "keyboard_listener.h"
#include "macro/macro_engine.h"
#include "app_log.h"
#include "preview_window.h"
#include "other_tools.h"
#include "mem/gpu_resource_manager.h"
#include "mem/cpu_affinity_manager.h"
#include "runtime/cuda_availability.h"
#include "runtime/inference_session.h"
#include "runtime/latency_probe.h"
#include "runtime/live_tune.h"
#include "runtime/config_snapshot.h"
#include "runtime/aim_telemetry.h"
#include "runtime/aim_loop.h"     // resetMouse() —— 换设备后丢弃旧的 MouseThread
#include "runtime/sched_boost.h"
#include "auth/auth_state.h"

#include "tensorrt/nvinf.h"

#include "MainWindow.h"
#include "widgets/IconFont.h"
#include "widgets/LoginDialog.h"
#include "config/ConfigManager.h"
#include "config/config_bridge.h"
#include "config/config_profiles.h"

std::condition_variable frameCV;
std::atomic<bool> shouldExit(false);
std::atomic<bool> aiming(false);
std::atomic<bool> session_stop_requested(true);
std::recursive_mutex configMutex;
std::mutex inputDeviceMutex;

TrtDetector trt_detector;

IDetector* g_detector = nullptr;
runtime::InferenceSession* g_inference_session = nullptr;
Config config;

MakcuConnection* makcuSerial = nullptr;
MakcuNewConnection* makcuNewSerial = nullptr;
// 第二台 MAKCUNEW(键盘那台)。nullptr = 未配置, 键盘动作回落到第一台。
MakcuNewConnection* makcuNewSerialKbd = nullptr;
KmboxNetConnection* kmboxNetSerial = nullptr;
std::string kmboxNetLastError;
std::shared_ptr<mouse_driver::IDriver> dhzboxDriver;
std::shared_ptr<mouse_driver::IDriver> ferrumDriver;
std::shared_ptr<mouse_driver::WindowsDriver> windowsDriver;
std::shared_ptr<mouse_driver::IDriver> catDriver;
std::shared_ptr<mouse_driver::CpboxDriver> cpboxDriver;
std::string catLastError;
std::string ferrumLastError;
std::string dhzboxLastError;
std::string cpboxLastError;

std::atomic<bool> detection_resolution_changed(false);
std::atomic<bool> capture_method_changed(false);
std::atomic<bool> capture_fps_changed(false);
std::atomic<bool> detector_model_changed(false);
std::atomic<bool> input_method_changed(false);

std::string g_iconLastError;

std::atomic<bool> g_replay_playback_active(false);
std::atomic<int>  g_replay_playback_frame(0);
std::atomic<int>  g_replay_playback_total(0);
std::atomic<unsigned int> g_replay_playback_request(0);

static int FatalExit(const std::string& message)
{
    std::cerr << message << std::endl;
    MessageBoxA(nullptr, message.c_str(), "Apotheosis", MB_ICONERROR | MB_OK);
    return -1;
}

static void HandleThreadCrash(const char* name, const std::exception* ex)
{
    std::cerr << "[Thread] " << name << " crashed: "
              << (ex ? ex->what() : "unknown exception") << std::endl;
    shouldExit = true;
    detectionBuffer.cv.notify_all();
}

template <typename Func>
static std::thread StartThreadGuarded(const char* name, Func func)
{
    return std::thread([name, func]() mutable {
        try
        {
            func();
        }
        catch (const std::exception& e)
        {
            HandleThreadCrash(name, &e);
        }
        catch (...)
        {
            HandleThreadCrash(name, nullptr);
        }
    });
}

void createInputDevices()
{
    macros::DevicePause macroPause;
    static std::mutex reconnectMutex;
    std::lock_guard<std::mutex> reconnect(reconnectMutex);
    const auto cfg = runtime_config::read();

    // ★ 必须在【销毁旧连接之前】先丢掉 MouseThread。
    //
    // MouseThread 里持有 MakcuConnection*/MakcuNewConnection* 的裸指针, 而且它的
    // 析构/复位路径会去调 driver_->leftUp()/rightUp()。如果先 destroy 旧连接再
    // 调 resetMouse(), 这一步就会解引用【已释放】的对象 —— use-after-free。
    // 之前把 resetMouse() 放在函数末尾就是这个错误顺序。
    runtime::aim_loop::resetMouse();

    std::unique_ptr<MakcuConnection> oldMakcu;
    std::unique_ptr<MakcuNewConnection> oldNew;
    std::unique_ptr<MakcuNewConnection> oldNewKbd;
    std::unique_ptr<KmboxNetConnection> oldKmboxNet;
    std::shared_ptr<mouse_driver::IDriver> oldDhzbox;
    std::shared_ptr<mouse_driver::IDriver> oldFerrum;
    std::shared_ptr<mouse_driver::IDriver> oldCat;
    std::shared_ptr<mouse_driver::CpboxDriver> oldCpbox;
    std::shared_ptr<mouse_driver::WindowsDriver> oldWindows;
    {
        std::lock_guard<std::mutex> lock(inputDeviceMutex);
        oldMakcu.reset(makcuSerial);
        oldNew.reset(makcuNewSerial);
        oldNewKbd.reset(makcuNewSerialKbd);
        oldKmboxNet.reset(kmboxNetSerial);
        oldDhzbox = std::move(dhzboxDriver);
        oldFerrum = std::move(ferrumDriver);
        oldCat = std::move(catDriver);
        oldCpbox = std::move(cpboxDriver);
        oldWindows = std::move(windowsDriver);
        makcuSerial = nullptr;
        makcuNewSerial = nullptr;
        makcuNewSerialKbd = nullptr;
        kmboxNetSerial = nullptr;
    }
    oldMakcu.reset();
    oldNew.reset();
    oldNewKbd.reset();
    oldKmboxNet.reset();
    oldDhzbox.reset();
    oldFerrum.reset();
    oldCat.reset();
    oldCpbox.reset();
    oldWindows.reset();
    std::unique_ptr<MakcuConnection> nextMakcu;
    std::unique_ptr<MakcuNewConnection> nextNew;
    std::unique_ptr<MakcuNewConnection> nextNewKbd;
    std::unique_ptr<KmboxNetConnection> nextKmboxNet;
    std::shared_ptr<mouse_driver::IDriver> nextDhzbox;
    std::shared_ptr<mouse_driver::IDriver> nextFerrum;
    std::shared_ptr<mouse_driver::IDriver> nextCat;
    std::shared_ptr<mouse_driver::CpboxDriver> nextCpbox;
    std::shared_ptr<mouse_driver::WindowsDriver> nextWindows;
    if (cfg->input_method == "WINDOWS")
        nextWindows = std::make_shared<mouse_driver::WindowsDriver>();
    else if (cfg->input_method == "MAKCU")
    {
        // ── 混合模式 (hybrid) ─────────────────────────────────────────────
        //
        // 鼠标那台的固件已重写为【纯 ASCII】(km.move / km.left(1) / ...), 与官方
        // SDK 的 MakcuConnection 无缝对接; 键盘那台 (KBD_PASSTHROUGH) 仍是二进制,
        // 继续走 MakcuNewConnection。两条链路协议不同, 必须用 WrappedHybridDriver
        // 分别转发 —— 见 mouse_driver.h 的说明。
        //
        // 开启条件: MAKCU 方式 + 填了键盘串口 (makcu_new_port_kbd)。
        // 不填键盘口 = 只有鼠标那台, 行为与纯 WrappedMakcuDriver 完全一致
        // (键盘能力位不声明, tapKey/maskRealKeyboard 直接失败)。
        nextMakcu = std::make_unique<MakcuConnection>(cfg->makcu_port, cfg->makcu_baudrate);
        if (!nextMakcu->isOpen()) nextMakcu.reset();

        if (!cfg->makcu_new_port_kbd.empty() &&
            cfg->makcu_new_port_kbd != cfg->makcu_port)
        {
            nextNewKbd = std::make_unique<MakcuNewConnection>(
                cfg->makcu_new_port_kbd, cfg->makcu_new_baudrate_kbd);
            if (!nextNewKbd->isOpen())
            {
                std::cerr << "[Apotheosis] keyboard port " << cfg->makcu_new_port_kbd
                          << " failed to open; keyboard injection and auto-stop "
                          << "keyboard masking will be unavailable. Mouse is unaffected."
                          << std::endl;
                nextNewKbd.reset();
            }
            else
            {
                std::cout << "[Apotheosis] Hybrid input: mouse on " << cfg->makcu_port
                          << " (ASCII), keyboard on " << cfg->makcu_new_port_kbd
                          << " (binary)." << std::endl;
            }
        }
    }
    else if (cfg->input_method == "MAKCUNEW")
    {
        nextNew = std::make_unique<MakcuNewConnection>(cfg->makcu_new_port, cfg->makcu_new_baudrate);
        if (!nextNew->isOpen()) nextNew.reset();

        // 第二台(键盘)。端口为空 = 未配置, 保持 nullptr 让驱动回落。
        // 与第一台端口相同也视为未配置(避免对同一个串口开两次)。
        //
        // ★ 注意: 这里【不】改走 hybrid。MAKCUNEW 方式下鼠标那台用的是
        //   MakcuNewConnection(二进制), 与键盘那台同协议, WrappedMakcuNewDriver
        //   已经能正确处理双硬件; 换成 MakcuConnection 反而会因为协议不符而失灵。
        if (!cfg->makcu_new_port_kbd.empty() &&
            cfg->makcu_new_port_kbd != cfg->makcu_new_port)
        {
            nextNewKbd = std::make_unique<MakcuNewConnection>(
                cfg->makcu_new_port_kbd, cfg->makcu_new_baudrate_kbd);
            if (!nextNewKbd->isOpen())
            {
                std::cerr << "[Apotheosis] keyboard MAKCUNEW port "
                          << cfg->makcu_new_port_kbd
                          << " failed to open; keyboard falls back to the mouse unit."
                          << std::endl;
                nextNewKbd.reset();
            }
        }
    }
    else if (cfg->input_method == "KMBOXNET")
    {
        nextKmboxNet = std::make_unique<KmboxNetConnection>(
            cfg->kmbox_net_ip, cfg->kmbox_net_port, cfg->kmbox_net_uuid);
        if (!nextKmboxNet->isOpen())
        {
            std::lock_guard<std::mutex> lock(inputDeviceMutex);
            kmboxNetLastError = nextKmboxNet->lastError();
            nextKmboxNet.reset();
        }
        else
        {
            std::lock_guard<std::mutex> lock(inputDeviceMutex);
            kmboxNetLastError.clear();
        }
    }
    else if (cfg->input_method == "DHZBOX_MINI")
    {
        nextDhzbox = std::make_shared<mouse_driver::DhzboxMiniDriver>(
            cfg->dhzbox_ip, static_cast<unsigned short>(cfg->dhzbox_port), cfg->dhzbox_key);
        { std::lock_guard<std::mutex> lock(inputDeviceMutex); dhzboxLastError = nextDhzbox->lastError(); }
        if (!nextDhzbox->isOpen()) nextDhzbox.reset();
    }
    else if (cfg->input_method == "CAT")
    {
        nextCat = std::make_shared<mouse_driver::CatDriver>(cfg->cat_ip,
            static_cast<unsigned short>(cfg->cat_port),cfg->cat_uuid,static_cast<unsigned short>(cfg->cat_monitor_port));
        { std::lock_guard<std::mutex> lock(inputDeviceMutex); catLastError=nextCat->lastError(); }
        if(!nextCat->isOpen()) nextCat.reset();
    }
    else if (cfg->input_method == "FERRUM")
    {
        nextFerrum = std::make_shared<mouse_driver::FerrumDriver>(cfg->ferrum_port, cfg->ferrum_baudrate);
        { std::lock_guard<std::mutex> lock(inputDeviceMutex); ferrumLastError = nextFerrum->lastError(); }
        if (!nextFerrum->isOpen()) nextFerrum.reset();
    }
    else if (cfg->input_method == "CPBOX")
    {
        nextCpbox = std::make_shared<mouse_driver::CpboxDriver>(cfg->cpbox_port);
        { std::lock_guard<std::mutex> lock(inputDeviceMutex); cpboxLastError = nextCpbox->lastError(); }
        if (!nextCpbox->isOpen()) nextCpbox.reset();
    }
    {
        std::lock_guard<std::mutex> lock(inputDeviceMutex);
        makcuSerial = nextMakcu.release();
        makcuNewSerial = nextNew.release();
        makcuNewSerialKbd = nextNewKbd.release();
        kmboxNetSerial = nextKmboxNet.release();
        dhzboxDriver = std::move(nextDhzbox);
        ferrumDriver = std::move(nextFerrum);
        catDriver = std::move(nextCat);
        cpboxDriver = std::move(nextCpbox);
        windowsDriver = std::move(nextWindows);
    }
    // 注意: 这里【不再】调用 resetMouse() —— 它已经在函数开头、销毁旧连接之前调过了。
    // 放在这里会造成"旧连接已释放、MouseThread 还指着它"的释放后使用。
}

void reconnectMouseDevice()
{
    macros::DevicePause macroPause;
    static std::mutex reconnectMtx;
    std::lock_guard<std::mutex> reconnect(reconnectMtx);
    const auto cfg = runtime_config::read();

    // 同 createInputDevices: 先丢 MouseThread(此时旧连接仍有效), 再销毁旧连接。
    runtime::aim_loop::resetMouse();

    std::unique_ptr<MakcuConnection> oldMakcu;
    std::unique_ptr<MakcuNewConnection> oldNew;
    std::unique_ptr<KmboxNetConnection> oldKmboxNet;
    std::shared_ptr<mouse_driver::IDriver> oldDhzbox;
    std::shared_ptr<mouse_driver::IDriver> oldFerrum;
    std::shared_ptr<mouse_driver::IDriver> oldCat;
    std::shared_ptr<mouse_driver::CpboxDriver> oldCpbox;
    std::shared_ptr<mouse_driver::WindowsDriver> oldWindows;
    {
        std::lock_guard<std::mutex> lock(inputDeviceMutex);
        oldMakcu.reset(makcuSerial);
        oldNew.reset(makcuNewSerial);
        oldKmboxNet.reset(kmboxNetSerial);
        oldDhzbox = std::move(dhzboxDriver);
        oldFerrum = std::move(ferrumDriver);
        oldCat = std::move(catDriver);
        oldCpbox = std::move(cpboxDriver);
        oldWindows = std::move(windowsDriver);
        makcuSerial = nullptr;
        makcuNewSerial = nullptr;
        kmboxNetSerial = nullptr;
    }
    oldMakcu.reset();
    oldNew.reset();
    oldKmboxNet.reset();
    oldDhzbox.reset();
    oldFerrum.reset();
    oldCat.reset();
    oldCpbox.reset();
    oldWindows.reset();

    std::unique_ptr<MakcuConnection> nextMakcu;
    std::unique_ptr<MakcuNewConnection> nextNew;
    std::unique_ptr<KmboxNetConnection> nextKmboxNet;
    std::shared_ptr<mouse_driver::IDriver> nextDhzbox;
    std::shared_ptr<mouse_driver::IDriver> nextFerrum;
    std::shared_ptr<mouse_driver::IDriver> nextCat;
    std::shared_ptr<mouse_driver::CpboxDriver> nextCpbox;
    std::shared_ptr<mouse_driver::WindowsDriver> nextWindows;

    if (cfg->input_method == "WINDOWS")
        nextWindows = std::make_shared<mouse_driver::WindowsDriver>();
    else if (cfg->input_method == "MAKCU")
    {
        nextMakcu = std::make_unique<MakcuConnection>(cfg->makcu_port, cfg->makcu_baudrate);
        if (!nextMakcu->isOpen()) nextMakcu.reset();
    }
    else if (cfg->input_method == "MAKCUNEW")
    {
        nextNew = std::make_unique<MakcuNewConnection>(cfg->makcu_new_port, cfg->makcu_new_baudrate);
        if (!nextNew->isOpen()) nextNew.reset();
    }
    else if (cfg->input_method == "KMBOXNET")
    {
        nextKmboxNet = std::make_unique<KmboxNetConnection>(
            cfg->kmbox_net_ip, cfg->kmbox_net_port, cfg->kmbox_net_uuid);
        if (!nextKmboxNet->isOpen())
        {
            std::lock_guard<std::mutex> lock(inputDeviceMutex);
            kmboxNetLastError = nextKmboxNet->lastError();
            nextKmboxNet.reset();
        }
        else
        {
            std::lock_guard<std::mutex> lock(inputDeviceMutex);
            kmboxNetLastError.clear();
        }
    }
    else if (cfg->input_method == "DHZBOX_MINI")
    {
        nextDhzbox = std::make_shared<mouse_driver::DhzboxMiniDriver>(
            cfg->dhzbox_ip, static_cast<unsigned short>(cfg->dhzbox_port), cfg->dhzbox_key);
        { std::lock_guard<std::mutex> lock(inputDeviceMutex); dhzboxLastError = nextDhzbox->lastError(); }
        if (!nextDhzbox->isOpen()) nextDhzbox.reset();
    }
    else if (cfg->input_method == "CAT")
    {
        nextCat = std::make_shared<mouse_driver::CatDriver>(cfg->cat_ip,
            static_cast<unsigned short>(cfg->cat_port),cfg->cat_uuid,static_cast<unsigned short>(cfg->cat_monitor_port));
        { std::lock_guard<std::mutex> lock(inputDeviceMutex); catLastError=nextCat->lastError(); }
        if(!nextCat->isOpen()) nextCat.reset();
    }
    else if (cfg->input_method == "FERRUM")
    {
        nextFerrum = std::make_shared<mouse_driver::FerrumDriver>(cfg->ferrum_port, cfg->ferrum_baudrate);
        { std::lock_guard<std::mutex> lock(inputDeviceMutex); ferrumLastError = nextFerrum->lastError(); }
        if (!nextFerrum->isOpen()) nextFerrum.reset();
    }
    else if (cfg->input_method == "CPBOX")
    {
        nextCpbox = std::make_shared<mouse_driver::CpboxDriver>(cfg->cpbox_port);
        { std::lock_guard<std::mutex> lock(inputDeviceMutex); cpboxLastError = nextCpbox->lastError(); }
        if (!nextCpbox->isOpen()) nextCpbox.reset();
    }

    {
        std::lock_guard<std::mutex> lock(inputDeviceMutex);
        makcuSerial = nextMakcu.release();
        makcuNewSerial = nextNew.release();
        kmboxNetSerial = nextKmboxNet.release();
        dhzboxDriver = std::move(nextDhzbox);
        ferrumDriver = std::move(nextFerrum);
        catDriver = std::move(nextCat);
        cpboxDriver = std::move(nextCpbox);
        windowsDriver = std::move(nextWindows);
    }
    // 注意: resetMouse() 已在函数开头调过(那时旧连接仍有效)。
    // 绝不能放在这里 —— 旧连接已经释放, 而 MouseThread 的驱动还指着它们。
}

void reconnectKeyboardDevice()
{
    macros::DevicePause macroPause;
    static std::mutex reconnectMtx;
    std::lock_guard<std::mutex> reconnect(reconnectMtx);
    const auto cfg = runtime_config::read();

    // ★ 同理, 而且这里更危险: HybridDriver 内部持有 kbdConn_ 裸指针。
    //   如果先销毁键盘连接、再 resetMouse(), 那么任何一次 tapKey/maskRealKeyboard
    //   都会解引用已释放的对象 —— 表现为"一触发瞄准(自动急停要发屏蔽命令)就出事"。
    //   顺序必须是: 先丢 MouseThread, 再销毁旧键盘连接。
    runtime::aim_loop::resetMouse();

    std::unique_ptr<MakcuNewConnection> oldNewKbd;
    {
        std::lock_guard<std::mutex> lock(inputDeviceMutex);
        oldNewKbd.reset(makcuNewSerialKbd);
        makcuNewSerialKbd = nullptr;
    }
    oldNewKbd.reset();

    std::unique_ptr<MakcuNewConnection> nextNewKbd;
    if (!cfg->makcu_new_port_kbd.empty())
    {
        nextNewKbd = std::make_unique<MakcuNewConnection>(
            cfg->makcu_new_port_kbd, cfg->makcu_new_baudrate_kbd);
        if (!nextNewKbd->isOpen())
        {
            std::cerr << "[Apotheosis] keyboard device " << cfg->makcu_new_port_kbd
                      << " failed to open." << std::endl;
            nextNewKbd.reset();
        }
        else
        {
            std::cout << "[Apotheosis] Keyboard device connected on "
                      << cfg->makcu_new_port_kbd << std::endl;
        }
    }

    {
        std::lock_guard<std::mutex> lock(inputDeviceMutex);
        makcuNewSerialKbd = nextNewKbd.release();
    }
    // 新键盘连接已装好; 下一拍 aim_loop 的 ensureMouse() 会用新指针重建 MouseThread。
}

void assignInputDevices()
{
}


static QString loadStyleSheet()
{
    QFile resource(":/style/theme.qss");
    if (resource.open(QIODevice::ReadOnly | QIODevice::Text))
        return QString::fromUtf8(resource.readAll());

    QString appDir = QCoreApplication::applicationDirPath();
    QStringList candidates = {
        appDir + "/style/theme.qss",
        appDir + "/../style/theme.qss",
        appDir + "/../../qt_ui/style/theme.qss",
        "./style/theme.qss",
        "../qt_ui/style/theme.qss",
    };
    for (const auto& path : candidates)
    {
        QFile file(path);
        if (file.open(QIODevice::ReadOnly | QIODevice::Text))
            return QString::fromUtf8(file.readAll());
    }
    return {};
}

int main(int argc, char* argv[])
{
    timeBeginPeriod(1);
    AppLog::InstallStdStreamCapture();

    SetConsoleOutputCP(CP_UTF8);
    SetRandomConsoleTitle();
    cv::utils::logging::setLogLevel(cv::utils::logging::LOG_LEVEL_FATAL);

    {
        wchar_t exePath[MAX_PATH]{};
        if (GetModuleFileNameW(nullptr, exePath, MAX_PATH) > 0)
        {
            std::filesystem::path exeDir = std::filesystem::path(exePath).parent_path();
            std::error_code ec;
            std::filesystem::current_path(exeDir, ec);
            if (ec && config.verbose)
            {
                std::cout << "[Config] Failed to set working dir: " << exeDir.u8string()
                          << " (" << ec.message() << ")" << std::endl;
            }
        }
    }

    if (!config.loadConfig())
    {
        std::cerr << "[Config] Error with loading config!" << std::endl;
        return FatalExit("[Config] Error with loading config!");
    }

    if (config.use_process_boost)
    {
        if (sched_boost::boostProcessPriority())
            std::cout << "[Sched] Process priority -> HIGH" << std::endl;
    }

    {
        runtime::latency::FileLogConfig logCfg;
        logCfg.directory   = "logs";
        logCfg.basename    = "latency";
        logCfg.interval_ms = 1000;
        logCfg.spike_ms    = 25.0;
        if (runtime::latency::startFileLog(logCfg))
            std::cout << "[Latency] Logging to " << runtime::latency::fileLogPath() << std::endl;
        else
            std::cerr << "[Latency] File log disabled: "
                      << runtime::latency::fileLogError() << std::endl;
    }

    auth::state().initialize("http://110.42.232.243:8787");
    CPUAffinityManager cpuManager;

    if (config.cpuCoreReserveCount > 0)
    {
        if (!cpuManager.reserveCPUCores(config.cpuCoreReserveCount))
            return FatalExit("[MAIN] Failed to reserve CPU cores.");
    }

    if (config.systemMemoryReserveMB > 0)
    {
        if (!cpuManager.reserveSystemMemory(config.systemMemoryReserveMB))
            return FatalExit("[MAIN] Failed to reserve system memory.");
    }

    try
    {
        const auto& cudaStatus = runtime::probe_cuda_runtime();
        if (config.verbose)
        {
            std::cout << "[CUDA] Probe: cudart=" << cudaStatus.cudart_loadable
                      << " nvinfer=" << cudaStatus.nvinfer_loadable
                      << " nvonnxparser=" << cudaStatus.nvonnxparser_loadable
                      << " devices=" << cudaStatus.device_count
                      << " version=" << cudaStatus.cuda_runtime_version << std::endl;
        }

        if (config.backend == "TRT" && !cudaStatus.trt_ready())
        {
            std::cerr << "[MAIN] TRT backend requested but unavailable: "
                      << cudaStatus.failure_reason
                      << ". DirectML fallback has been removed; TensorRT is the only backend."
                      << std::endl;
        }

        if (cudaStatus.trt_ready())
        {
            const int required_cuda_version = 12090;
            const int max_supported_cuda_version = 12099;
            if (cudaStatus.cuda_runtime_version < required_cuda_version ||
                cudaStatus.cuda_runtime_version > max_supported_cuda_version)
            {
                const int runtime_major = cudaStatus.cuda_runtime_version / 1000;
                const int runtime_minor = (cudaStatus.cuda_runtime_version % 1000) / 10;
                std::cerr << "[MAIN] CUDA 12.9 targeted. Detected "
                          << runtime_major << "." << runtime_minor
                          << ". TRT backend may misbehave." << std::endl;
            }

            GPUResourceManager gpuManager;
            if (config.backend == "TRT")
            {
                if (config.gpuMemoryReserveMB > 0)
                {
                    if (!gpuManager.reserveGPUMemory(config.gpuMemoryReserveMB))
                        return FatalExit("[MAIN] Failed to reserve GPU memory.");
                }

                if (config.enableGpuExclusiveMode)
                {
                    if (!gpuManager.setGPUExclusiveMode())
                        return FatalExit("[MAIN] Failed to set GPU exclusive mode.");
                }
            }
        }
        if (!CreateDirectory(L"screenshots", NULL) && GetLastError() != ERROR_ALREADY_EXISTS)
        {
            std::cout << "[MAIN] Error with screenshot folder" << std::endl;
            return FatalExit("[MAIN] Error with screenshot folder");
        }

        if (!CreateDirectory(L"models", NULL) && GetLastError() != ERROR_ALREADY_EXISTS)
        {
            std::cout << "[MAIN] Error with models folder" << std::endl;
            return FatalExit("[MAIN] Error with models folder");
        }
        if (!CreateDirectory(L"models\\engines", NULL) && GetLastError() != ERROR_ALREADY_EXISTS)
        {
            std::cout << "[MAIN] Error with models\\engines folder" << std::endl;
            return FatalExit("[MAIN] Error with models\\engines folder");
        }

        std::string modelPath = "models/" + config.ai_model;

        if (!std::filesystem::exists(std::filesystem::u8path(modelPath)))
        {
            std::cerr << "[MAIN] Specified model does not exist: " << modelPath << std::endl;

            std::vector<std::string> modelFiles = getModelFiles();

            if (!modelFiles.empty())
            {
                config.ai_model = modelFiles[0];
                config.saveConfig();
                std::cout << "[MAIN] Loaded first available model: " << config.ai_model << std::endl;
            }
            else
            {
                std::cerr << "[MAIN] No models found in 'models' directory." << std::endl;
                return FatalExit("[MAIN] No models found in 'models' directory.");
            }
        }

        std::vector<std::string> availableModels = getAvailableModels();

        if (!config.ai_model.empty())
        {
            std::string candidate = "models/" + config.ai_model;
            if (!std::filesystem::exists(std::filesystem::u8path(candidate)))
            {
                std::cerr << "[MAIN] Specified model does not exist: " << candidate << std::endl;

                if (!availableModels.empty())
                {
                    config.ai_model = availableModels[0];
                    config.saveConfig("config.ini");
                    std::cout << "[MAIN] Loaded first available model: " << config.ai_model << std::endl;
                }
                else
                {
                    std::cerr << "[MAIN] No models found in 'models' directory." << std::endl;
                    return FatalExit("[MAIN] No models found in 'models' directory.");
                }
            }
        }
        else
        {
            if (!availableModels.empty())
            {
                config.ai_model = availableModels[0];
                config.saveConfig();
                std::cout << "[MAIN] No AI model specified in config. Loaded first available model: " << config.ai_model << std::endl;
            }
            else
            {
                std::cerr << "[MAIN] No AI models found in 'models' directory." << std::endl;
                return FatalExit("[MAIN] No AI models found in 'models' directory.");
            }
        }

        {
            std::string preloadError;
            runtime::preload_model_metadata(std::string("models/") + config.ai_model, true, &preloadError);
            if (!preloadError.empty())
                std::cerr << "[MAIN] Model metadata preload failed: " << preloadError << std::endl;
        }

        runtime::InferenceSession session;
        g_inference_session = &session;

        if (GetEnvironmentVariableA("APOTHEOSIS_AUTOSTART_TRT", nullptr, 0) > 0)
        {
            config.backend = "TRT";
            std::cout << "[MAIN] APOTHEOSIS_AUTOSTART_TRT=1, starting TensorRT session." << std::endl;
            session.start(config.backend, std::string("models/") + config.ai_model);
        }

        std::thread keyThread = StartThreadGuarded("KeyboardListener", [] {
            keyboardListener();
        });

        std::thread autoCapThread = StartThreadGuarded("AutoCapture", [] {
            AutoCapture::auto_capture_thread();
        });

        PreviewWindow_Start();

        welcome_message();

        QApplication app(argc, argv);
        app.setApplicationName("Apotheosis");
        app.setOrganizationName("Apotheosis");

        ApotheosisTheme::apply(app);
        IconFont::load();

        if (auto qss = loadStyleSheet(); !qss.isEmpty())
            app.setStyleSheet(qss);

        ConfigManager::instance().load("config.ini");
        ConfigBridge::instance().syncFromRuntime();

        ConfigProfiles::instance().initialize();

        live_tune::start();

        MainWindow window;
        window.resize(960, 640);
        window.show();

        QObject::connect(&app, &QCoreApplication::aboutToQuit, [] {
            ConfigBridge::instance().flush();
            shouldExit = true;
        });

        int result = app.exec();

        shouldExit = true;
        keyThread.join();
        if (autoCapThread.joinable()) autoCapThread.join();

        PreviewWindow_Stop();

        session.stop();
        g_inference_session = nullptr;

        delete makcuSerial;
        makcuSerial = nullptr;
        delete makcuNewSerial;
        makcuNewSerial = nullptr;
        delete makcuNewSerialKbd;
        makcuNewSerialKbd = nullptr;
        delete kmboxNetSerial;
        dhzboxDriver.reset();
        ferrumDriver.reset();
        catDriver.reset();
        windowsDriver.reset();
        kmboxNetSerial = nullptr;

        timeEndPeriod(1);
        return result;
    }
    catch (const std::exception& e)
    {
        std::cerr << "[MAIN] An error has occurred in the main stream: " << e.what() << std::endl;
        return FatalExit(std::string("[MAIN] An error has occurred in the main stream: ") + e.what());
    }
}
