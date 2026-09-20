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
#include "Apotheosis.h"
#include "keyboard_listener.h"
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

std::atomic<bool> detection_resolution_changed(false);
std::atomic<bool> capture_method_changed(false);
std::atomic<bool> capture_fps_changed(false);
std::atomic<bool> detector_model_changed(false);
std::atomic<bool> input_method_changed(false);

std::string g_iconLastError;

std::atomic<bool> g_replay_playback_active(false);
std::atomic<int>  g_replay_playback_frame(0);

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
    static std::mutex reconnectMutex;
    std::lock_guard<std::mutex> reconnect(reconnectMutex);
    const auto cfg = runtime_config::read();
    std::unique_ptr<MakcuConnection> oldMakcu;
    std::unique_ptr<MakcuNewConnection> oldNew;
    std::unique_ptr<MakcuNewConnection> oldNewKbd;
    std::unique_ptr<KmboxNetConnection> oldKmboxNet;
    {
        std::lock_guard<std::mutex> lock(inputDeviceMutex);
        oldMakcu.reset(makcuSerial);
        oldNew.reset(makcuNewSerial);
        oldNewKbd.reset(makcuNewSerialKbd);
        oldKmboxNet.reset(kmboxNetSerial);
        makcuSerial = nullptr;
        makcuNewSerial = nullptr;
        makcuNewSerialKbd = nullptr;
        kmboxNetSerial = nullptr;
    }
    oldMakcu.reset();
    oldNew.reset();
    oldNewKbd.reset();
    oldKmboxNet.reset();
    std::unique_ptr<MakcuConnection> nextMakcu;
    std::unique_ptr<MakcuNewConnection> nextNew;
    std::unique_ptr<MakcuNewConnection> nextNewKbd;
    std::unique_ptr<KmboxNetConnection> nextKmboxNet;
    if (cfg->input_method == "MAKCU")
    {
        nextMakcu = std::make_unique<MakcuConnection>(cfg->makcu_port, cfg->makcu_baudrate);
        if (!nextMakcu->isOpen()) nextMakcu.reset();
    }
    else if (cfg->input_method == "MAKCUNEW")
    {
        nextNew = std::make_unique<MakcuNewConnection>(cfg->makcu_new_port, cfg->makcu_new_baudrate);
        if (!nextNew->isOpen()) nextNew.reset();

        // 第二台(键盘)。端口为空 = 未配置, 保持 nullptr 让驱动回落。
        // 与第一台端口相同也视为未配置(避免对同一个串口开两次)。
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
        if (!nextKmboxNet->isOpen()) nextKmboxNet.reset();
    }
    {
        std::lock_guard<std::mutex> lock(inputDeviceMutex);
        makcuSerial = nextMakcu.release();
        makcuNewSerial = nextNew.release();
        makcuNewSerialKbd = nextNewKbd.release();
        kmboxNetSerial = nextKmboxNet.release();
    }
}

void assignInputDevices()
{
}

static void applyLightPalette(QApplication& app)
{
    app.setStyle(QStyleFactory::create("Fusion"));

    QPalette pal;
    pal.setColor(QPalette::Window, QColor("#F4F6FA"));
    pal.setColor(QPalette::WindowText, QColor("#17191F"));
    pal.setColor(QPalette::Base, QColor("#FFFFFF"));
    pal.setColor(QPalette::AlternateBase, QColor("#FBFCFD"));
    pal.setColor(QPalette::Text, QColor("#17191F"));
    pal.setColor(QPalette::Button, QColor("#F8F9FB"));
    pal.setColor(QPalette::ButtonText, QColor("#17191F"));
    pal.setColor(QPalette::ToolTipBase, QColor("#17191F"));
    pal.setColor(QPalette::ToolTipText, QColor("#FFFFFF"));
    pal.setColor(QPalette::PlaceholderText, QColor("#98A1B0"));
    pal.setColor(QPalette::Highlight, QColor("#5865D8"));
    pal.setColor(QPalette::HighlightedText, QColor("#FFFFFF"));
    pal.setColor(QPalette::Disabled, QPalette::Text, QColor("#B8C0CC"));
    pal.setColor(QPalette::Disabled, QPalette::ButtonText, QColor("#B8C0CC"));
    app.setPalette(pal);

    QFont appFont;
    appFont.setFamilies({QStringLiteral("Segoe UI Variable"),
                         QStringLiteral("Microsoft YaHei UI"),
                         QStringLiteral("Segoe UI")});
    appFont.setPixelSize(13);
    app.setFont(appFont);
}

static QString loadStyleSheet()
{
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

        applyLightPalette(app);
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
