cmake_minimum_required(VERSION 3.24)

project(Apotheosis LANGUAGES C CXX)

include(CTest)
option(APOTHEOSIS_LOGIC_TESTS_ONLY "Build portable logic regressions without the Windows application" OFF)
if(APOTHEOSIS_LOGIC_TESTS_ONLY)
    enable_testing()
    add_subdirectory(tests)
    return()
endif()

if(NOT WIN32)
    message(FATAL_ERROR "Apotheosis only supports Windows builds.")
endif()

if(NOT CMAKE_GENERATOR MATCHES "^Visual Studio (17 2022|18 2026)$")
    message(FATAL_ERROR "Use Visual Studio 2022 or 2026 generator. Current: ${CMAKE_GENERATOR}")
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CUDA_STANDARD 17)
set(CMAKE_CUDA_STANDARD_REQUIRED ON)

# Qt6 requires AUTOMOC for Q_OBJECT macro processing
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_PREFIX_PATH "C:/Qt/6.8.3/msvc2022_64" CACHE PATH "Qt6 prefix")

if(NOT CMAKE_CUDA_FLAGS MATCHES "(^| )--allow-unsupported-compiler($| )")
    string(APPEND CMAKE_CUDA_FLAGS " --allow-unsupported-compiler")
endif()
enable_language(CUDA)

set(SRC_DIR "${CMAKE_SOURCE_DIR}/Apotheosis")
set(PACKAGES_DIR "${CMAKE_SOURCE_DIR}/packages")
set(OPENCV_ROOT "${SRC_DIR}/modules/opencv/prebuilt/opencv/build")
set(TENSORRT_ROOT "${SRC_DIR}/modules/TensorRT-10.15.1.29" CACHE PATH "TensorRT 10.15 root")
set(CUDNN_ROOT "$ENV{ProgramW6432}/NVIDIA/CUDNN/v9.17" CACHE PATH "cuDNN 9 root")
set(MSVC_REDIST_DIR "$ENV{ProgramW6432}/Microsoft Visual Studio/18/Community/VC/Redist/MSVC/14.50.35710/x64/Microsoft.VC145.CRT" CACHE PATH "MSVC x64 runtime redist")
set(WINDOWS_DEBUGGERS_X64_DIR "$ENV{ProgramFiles\(x86\)}/Windows Kits/10/Debuggers/x64" CACHE PATH "Windows Debugging Tools x64")

function(pick_latest_dir out_var glob_pattern)
    file(GLOB candidates LIST_DIRECTORIES true "${glob_pattern}")
    if(candidates)
        list(SORT candidates COMPARE NATURAL ORDER DESCENDING)
        list(GET candidates 0 latest)
        set(${out_var} "${latest}" PARENT_SCOPE)
    else()
        set(${out_var} "" PARENT_SCOPE)
    endif()
endfunction()

function(require_file path label)
    if(NOT EXISTS "${path}")
        message(FATAL_ERROR "${label} not found: ${path}")
    endif()
endfunction()

function(copy_runtime target path)
    if(EXISTS "${path}")
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${path}" "$<TARGET_FILE_DIR:${target}>"
            VERBATIM)
    endif()
endfunction()

# ★ 2026-09-17: DirectML 后端整条移除, 所以这里不再选 DirectML 风味的 ORT 包。
#   ONNX Runtime 本身【保留】—— model_inspector.cpp 用它读 .onnx 的类别名与
#   输入输出形状 (与推理后端无关, 换模型时要用)。
pick_latest_dir(ONNXRUNTIME_DIR "${PACKAGES_DIR}/Microsoft.ML.OnnxRuntime.*")
pick_latest_dir(OPENCV_VC_DIR "${OPENCV_ROOT}/x64/vc*")
if(OPENCV_VC_DIR STREQUAL "")
    set(OPENCV_VC_DIR "${OPENCV_ROOT}/x64/vc18")
endif()

set(OPENCV_INCLUDE_DIR "${OPENCV_ROOT}/include")
set(OPENCV_LIB "${OPENCV_VC_DIR}/lib/opencv_world4130.lib")
set(OPENCV_DLL "${OPENCV_VC_DIR}/bin/opencv_world4130.dll")

if(DEFINED CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION)
    set(CPPWINRT_INCLUDE_DIR "C:/Program Files (x86)/Windows Kits/10/Include/${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}/cppwinrt")
endif()
if(NOT EXISTS "${CPPWINRT_INCLUDE_DIR}/winrt/base.h")
    pick_latest_dir(CPPWINRT_INCLUDE_DIR "C:/Program Files (x86)/Windows Kits/10/Include/*/cppwinrt")
endif()

require_file("${OPENCV_INCLUDE_DIR}/opencv2/opencv.hpp" "OpenCV headers")
require_file("${OPENCV_LIB}" "OpenCV library")
require_file("${OPENCV_DLL}" "OpenCV runtime")
# ★ ONNX Runtime 保留: model_inspector.cpp 用它读 .onnx 的类别名/输入输出形状,
#   与 DirectML 后端【无关】。DirectML 已于 2026-09-17 随 DML 后端一起移除
#   (见下方 DIRECTML_DIR / dml_detector 的删除说明)。
require_file("${ONNXRUNTIME_DIR}/build/native/include/onnxruntime_cxx_api.h" "ONNX Runtime headers")
require_file("${ONNXRUNTIME_DIR}/runtimes/win-x64/native/onnxruntime.lib" "ONNX Runtime library")
require_file("${TENSORRT_ROOT}/include/NvInfer.h" "TensorRT headers")
require_file("${TENSORRT_ROOT}/lib/nvinfer_10.lib" "TensorRT nvinfer library")
require_file("${TENSORRT_ROOT}/lib/nvonnxparser_10.lib" "TensorRT nvonnxparser library")
require_file("${CPPWINRT_INCLUDE_DIR}/winrt/base.h" "C++/WinRT headers")
require_file("${MSVC_REDIST_DIR}/vcruntime140.dll" "MSVC runtime redistributables")

find_package(CUDAToolkit REQUIRED)
find_package(Qt6 REQUIRED COMPONENTS Widgets Network)

set(QT_UI_DIR "${CMAKE_SOURCE_DIR}/qt_ui")

# ★ AVA 选靶/瞄点那一组(mouse/ava_exact/, 12 个文件)已于 2026-09-17 删除。
#   它随瞄准控制链一起失去消费者后, 曾以"编进去但没人调"的状态保留了一段时间;
#   现按要求连同构建列表一起移除。
# 原 AVA PIDF 整条管线(pidf_mode1/mode2、postprocess、update、axis_policy、
# aim_movement_pipeline、controller_orchestration、pid_input、qx_curve、
# process_humanization、humanization_math、热键旁路、mouse_output、timed_button)
# 已全部删除。

set(APP_SOURCES
    "${SRC_DIR}/Apotheosis.cpp"
    "${SRC_DIR}/capture/auto_capture.cpp"
    "${SRC_DIR}/capture/capture.cpp"
    "${SRC_DIR}/capture/stream_capture.cpp"
    "${SRC_DIR}/capture/ndi_capture.cpp"
    "${SRC_DIR}/capture/dxgi_capture.cpp"
    "${SRC_DIR}/capture/dxgi_capture.h"
    "${SRC_DIR}/capture/dxgi_capture_logic.h"
    "${SRC_DIR}/ndi/ndi_runtime.cpp"
    "${SRC_DIR}/capture/capture_card_caps.h"
    "${SRC_DIR}/capture/capture_card_probe.h"
    "${SRC_DIR}/capture/capture_utils.cpp"
    "${SRC_DIR}/capture/dshow_capture.cpp"
    "${SRC_DIR}/capture/dshow_capture.h"
    "${SRC_DIR}/capture/gpu_color_ops.cu"
    "${SRC_DIR}/capture/gpu_jpeg_decoder.cpp"
    "${SRC_DIR}/capture/magewell_capture.cpp"
    "${SRC_DIR}/capture/mf_capture.cpp"
    "${SRC_DIR}/config/config.cpp"
    "${SRC_DIR}/crosshair/crosshair_detector.cpp"
      "${SRC_DIR}/crosshair/laser_detector.cpp"
    "${SRC_DIR}/crosshair/color_picker.cpp"
    "${SRC_DIR}/crosshair/crosshair_runtime.cpp"
    "${SRC_DIR}/detector/cuda_preprocess.cu"
    "${SRC_DIR}/detector/detection_buffer.cpp"
    "${SRC_DIR}/detector/model_inspector.cpp"
    "${SRC_DIR}/detector/postProcess.cpp"
    "${SRC_DIR}/detector/trt_detector.cpp"
    "${SRC_DIR}/tensorrt/engine_cache_key.cpp"
    "${SRC_DIR}/keyboard/keycodes.cpp"
    "${SRC_DIR}/keyboard/keyboard_listener.cpp"
    "${SRC_DIR}/keyboard/hotkey_blocking.cpp"
    "${SRC_DIR}/macro/macro_engine.cpp"
    "${SRC_DIR}/macro/rule_sources.cpp"
    "${SRC_DIR}/macro/macro_engine.h"
    "${SRC_DIR}/macro/macro_config.h"
    "${SRC_DIR}/mem/cpu_affinity_manager.cpp"
    "${SRC_DIR}/mem/gpu_image.cpp"
    "${SRC_DIR}/mem/gpu_resource_manager.cpp"
    "${SRC_DIR}/model_crypto/model_crypto.cpp"
    "${SRC_DIR}/auth/auth_client.cpp"
    "${SRC_DIR}/auth/auth_state.cpp"
    "${SRC_DIR}/modules/makcu/src/makcu.cpp"
    "${SRC_DIR}/modules/makcu/src/serialport.cpp"
    "${SRC_DIR}/mouse/AimbotTarget.cpp"
    # ★ 瞄准控制链已整条删除(2026-09-17): aim_pid / boss_aim / aim_tracker /
    #   aim_scale / aim_path / anchor_filter / auto_stop / trigger_scope /
    #   autotune_* 与 runtime/mouse_thread_loop.cpp 全部移除。
    #   本程序现在只做【采集 → 推理】, 检测结果供预览窗显示。
    "${SRC_DIR}/mouse/Makcu.cpp"
    "${SRC_DIR}/mouse/MakcuNew.cpp"
    # KMBox Net (以太网 UDP) 后端 + 驱动抽象层 (2026-09-15 恢复/新增)。
    # 抽象层的形状移植自 AimMagic 的驱动工厂 FUN_140040ff0 (见 mouse_driver.h)。
    "${SRC_DIR}/mouse/kmboxNetConnection.cpp"
    "${SRC_DIR}/mouse/kmbox_net/kmboxNet.cpp"
    "${SRC_DIR}/mouse/mouse_driver.cpp"
    "${SRC_DIR}/mouse/dhzbox_driver.cpp"
    "${SRC_DIR}/mouse/ferrum_driver.cpp"
    "${SRC_DIR}/mouse/cat_driver.cpp"
    "${SRC_DIR}/mouse/cpbox_driver.cpp"
    "${SRC_DIR}/mouse/windows_driver.cpp"
    "${SRC_DIR}/mouse/windows_driver.h"
    "${SRC_DIR}/mouse/mouse.cpp"
    "${SRC_DIR}/overlay/app_log.cpp"
    "${SRC_DIR}/overlay/preview_window.cpp"
    "${SRC_DIR}/runtime/active_hotkey.cpp"
    # 移植后的纯计算控制层。
    "${SRC_DIR}/control/types.cpp"
    "${SRC_DIR}/control/anchor.cpp"
    "${SRC_DIR}/control/recovered_pid.cpp"
    "${SRC_DIR}/control/recovered_tracker.cpp"
    "${SRC_DIR}/control/recovered_aim_controller.cpp"
    "${SRC_DIR}/runtime/aim_loop.cpp"
    "${SRC_DIR}/runtime/aim_path_config.h"
    # aim_loop_config.cpp 是【纯映射】, 不依赖 OpenCV/Windows ⇒ 逻辑测试也编它。
    "${SRC_DIR}/runtime/aim_loop_config.cpp"
    "${SRC_DIR}/runtime/aim_telemetry.cpp"
    "${SRC_DIR}/runtime/cuda_availability.cpp"
    "${SRC_DIR}/runtime/config_snapshot.cpp"
    "${SRC_DIR}/runtime/config_snapshot.h"
    "${SRC_DIR}/runtime/inference_session.cpp"
    "${SRC_DIR}/runtime/latency_probe.h"
    "${SRC_DIR}/runtime/live_tune.cpp"
    "${SRC_DIR}/runtime/live_tune.h"
    "${SRC_DIR}/runtime/sched_boost.cpp"
    "${SRC_DIR}/runtime/sched_boost.h"
    "${SRC_DIR}/scr/file_picker.cpp"
    "${SRC_DIR}/scr/other_tools.cpp"
    "${SRC_DIR}/tensorrt/nvinf.cpp"
    # Qt UI sources
    "${QT_UI_DIR}/MainWindow.cpp"
    "${QT_UI_DIR}/MainWindow.h"
    "${QT_UI_DIR}/remote/LanTuningServer.cpp"
    "${QT_UI_DIR}/remote/LanTuningServer.h"
    "${QT_UI_DIR}/remote/tuning.qrc"
    "${QT_UI_DIR}/pages/LanTuningPage.cpp"
    "${QT_UI_DIR}/pages/LanTuningPage.h"
    "${QT_UI_DIR}/config/ConfigManager.cpp"
    "${QT_UI_DIR}/config/ConfigManager.h"
    "${QT_UI_DIR}/config/config_profiles.cpp"
    "${QT_UI_DIR}/config/config_profiles.h"
    "${QT_UI_DIR}/config/config_bridge.cpp"
    "${QT_UI_DIR}/config/config_bridge.h"
    "${QT_UI_DIR}/pages/AiModelPage.cpp"
    "${QT_UI_DIR}/pages/AiModelPage.h"
    # ★★ 瞄准设置页 (2026-09-17 第三轮续重建): 热键管理 + 通用控制器层参数。
    #   它同时是 config.hotkeys[] 的唯一界面写入者。
    "${QT_UI_DIR}/pages/AimSettingsPage.cpp"
    "${QT_UI_DIR}/widgets/HotkeyActivationWidget.cpp"
    "${QT_UI_DIR}/widgets/HotkeyActivationWidget.h"
    "${QT_UI_DIR}/widgets/TriggerWorkflowEditor.cpp"
    "${QT_UI_DIR}/widgets/TriggerWorkflowEditor.h"
    "${QT_UI_DIR}/widgets/TriggerTargetEditor.cpp"
    "${QT_UI_DIR}/widgets/TriggerTargetEditor.h"
    "${QT_UI_DIR}/pages/AimSettingsPage.h"
    "${QT_UI_DIR}/pages/AutoCapturePage.cpp"
    "${QT_UI_DIR}/pages/AutoCapturePage.h"
    "${QT_UI_DIR}/pages/CapturePage.cpp"
    "${QT_UI_DIR}/pages/CapturePage.h"
    "${QT_UI_DIR}/pages/CrosshairPage.cpp"
    "${QT_UI_DIR}/pages/CrosshairPage.h"
    "${QT_UI_DIR}/pages/AimpointRecoilPage.cpp"
    "${QT_UI_DIR}/pages/AimpointRecoilPage.h"
    "${QT_UI_DIR}/pages/AutoFlashPage.cpp"
    "${QT_UI_DIR}/pages/AutoFlashPage.h"
    "${QT_UI_DIR}/pages/MacroPage.cpp"
    "${QT_UI_DIR}/pages/MacroPage.h"
    "${QT_UI_DIR}/widgets/MacroRuleEditor.cpp"
    "${QT_UI_DIR}/widgets/MacroRuleEditor.h"
    "${QT_UI_DIR}/widgets/MacroTriggerCard.cpp"
    "${QT_UI_DIR}/widgets/MacroTriggerCard.h"
    "${QT_UI_DIR}/widgets/MacroConditionList.cpp"
    "${QT_UI_DIR}/widgets/MacroConditionList.h"
    "${QT_UI_DIR}/widgets/MacroActionTimeline.cpp"
    "${QT_UI_DIR}/widgets/MacroActionTimeline.h"
    "${QT_UI_DIR}/widgets/MacroSummary.h"
    "${QT_UI_DIR}/widgets/MacroUiCommon.h"
    "${QT_UI_DIR}/pages/HeadBodyFusionPage.cpp"
    "${QT_UI_DIR}/pages/HeadBodyFusionPage.h"
    "${QT_UI_DIR}/pages/DebugPage.cpp"
    "${QT_UI_DIR}/pages/DebugPage.h"
    "${QT_UI_DIR}/pages/HardwarePage.cpp"
    "${QT_UI_DIR}/pages/HardwarePage.h"
    "${QT_UI_DIR}/pages/LogPage.cpp"
    "${QT_UI_DIR}/pages/LogPage.h"
    "${QT_UI_DIR}/pages/ModelToolsPage.cpp"
    "${QT_UI_DIR}/pages/ModelToolsPage.h"
    "${QT_UI_DIR}/pages/OverviewPage.cpp"
    "${QT_UI_DIR}/pages/OverviewPage.h"
    "${QT_UI_DIR}/pages/SessionPage.cpp"
    "${QT_UI_DIR}/pages/SessionPage.h"
    "${QT_UI_DIR}/pages/StatsPage.cpp"
    "${QT_UI_DIR}/pages/StatsPage.h"
    "${QT_UI_DIR}/pages/TargetPage.cpp"
    "${QT_UI_DIR}/pages/TargetPage.h"
    "${QT_UI_DIR}/widgets/CardWidget.cpp"
    "${QT_UI_DIR}/widgets/CardWidget.h"
    "${QT_UI_DIR}/widgets/CurveCanvas.cpp"
    "${QT_UI_DIR}/widgets/CurveCanvas.h"
    "${QT_UI_DIR}/widgets/NeuralCurveTrainer.cpp"
    "${QT_UI_DIR}/widgets/NeuralCurveTrainer.h"
    "${QT_UI_DIR}/widgets/FormKit.cpp"
    "${QT_UI_DIR}/widgets/FormKit.h"
    "${QT_UI_DIR}/widgets/IconFont.cpp"
    "${QT_UI_DIR}/widgets/IconFont.h"
    "${QT_UI_DIR}/widgets/LoginDialog.cpp"
    "${QT_UI_DIR}/widgets/LoginDialog.h"
    "${QT_UI_DIR}/widgets/MetricCard.cpp"
    "${QT_UI_DIR}/widgets/MetricCard.h"
    "${QT_UI_DIR}/widgets/SideNav.cpp"
    "${QT_UI_DIR}/widgets/SideNav.h"
    "${QT_UI_DIR}/widgets/StatusBar.cpp"
    "${QT_UI_DIR}/widgets/StatusBar.h"
    "${QT_UI_DIR}/widgets/StatusPill.cpp"
    "${QT_UI_DIR}/widgets/StatusPill.h"
    "${QT_UI_DIR}/widgets/TelemetryChart.cpp"
    "${QT_UI_DIR}/widgets/TelemetryChart.h"
    "${QT_UI_DIR}/widgets/ToggleSwitch.cpp"
    "${QT_UI_DIR}/widgets/ToggleSwitch.h"
    "${QT_UI_DIR}/widgets/TopNavBar.cpp"
    "${QT_UI_DIR}/widgets/TopNavBar.h"
)

add_library(serial_embedded STATIC
    "${SRC_DIR}/modules/serial/src/serial.cc"
    "${SRC_DIR}/modules/serial/src/impl/win.cc"
    "${SRC_DIR}/modules/serial/src/impl/list_ports/list_ports_win.cc")

target_include_directories(serial_embedded PUBLIC "${SRC_DIR}/modules/serial/include")
target_link_libraries(serial_embedded PUBLIC setupapi)

add_executable(ai WIN32 ${APP_SOURCES})
target_sources(ai PRIVATE "${QT_UI_DIR}/style/theme.qrc")

# Optional official CPBox runtime. The SDK remains outside the source tree;
# set CPBOX_SDK_ROOT to the extracted cpbox_sdk_final directory to stage its DLL.
set(CPBOX_SDK_ROOT "" CACHE PATH "Root of the official CPBox SDK")
if(CPBOX_SDK_ROOT)
    if(CMAKE_SIZEOF_VOID_P EQUAL 8)
        set(CPBOX_ARCH "x64")
    else()
        set(CPBOX_ARCH "x86")
    endif()
    set(CPBOX_DLL "${CPBOX_SDK_ROOT}/bin/${CPBOX_ARCH}/cpbox_mouse_dll.dll")
    require_file("${CPBOX_DLL}" "CPBox SDK runtime DLL")
    copy_runtime(ai "${CPBOX_DLL}")
endif()

# Optional official Magewell SDK. The SDK is installed separately; its files are
# deliberately not copied into the source tree or packaged with the application.
set(MAGEWELL_SDK_ROOT "" CACHE PATH "Root of the Magewell Windows Capture SDK")
if(MAGEWELL_SDK_ROOT)
    find_path(MAGEWELL_SDK_INCLUDE_DIR LibMWCapture/MWCapture.h
        PATHS "${MAGEWELL_SDK_ROOT}/SDKv3/Include" "${MAGEWELL_SDK_ROOT}/Include"
        NO_DEFAULT_PATH)
    find_library(MAGEWELL_SDK_LIBRARY NAMES LibMWCapture
        PATHS "${MAGEWELL_SDK_ROOT}/SDKv3/Lib/x64" "${MAGEWELL_SDK_ROOT}/Lib/x64"
        NO_DEFAULT_PATH)
    if(NOT MAGEWELL_SDK_INCLUDE_DIR OR NOT MAGEWELL_SDK_LIBRARY)
        message(FATAL_ERROR "Magewell SDK headers or x64 import library not found under MAGEWELL_SDK_ROOT")
    endif()
    target_include_directories(ai PRIVATE "${MAGEWELL_SDK_INCLUDE_DIR}")
    target_link_libraries(ai PRIVATE "${MAGEWELL_SDK_LIBRARY}")
    target_compile_definitions(ai PRIVATE APOTHEOSIS_HAS_MAGEWELL_SDK)
    if(MSVC)
        target_link_options(ai PRIVATE "/DELAYLOAD:LibMWCapture.dll")
    endif()
endif()

# Native code for the deployed Turing/Ampere GPUs; compute_86 PTX provides
# forward JIT compatibility. TensorRT/driver compatibility is still required.
set_target_properties(ai PROPERTIES
    OUTPUT_NAME "Apotheosis"
    CUDA_STANDARD 17
    CUDA_STANDARD_REQUIRED ON
    # 运行机使用的 CMP 40HX/TU106 是 sm_75，开发机 RTX 3060 是 sm_86。
    # 同时保留 compute_86 PTX，供更新架构的驱动进行前向 JIT，避免再次出现
    # “解码/产帧 FPS 正常，但 CUDA 转色核无法启动而画面全黑”。
    CUDA_ARCHITECTURES "75-real;86-real;86-virtual")

foreach(target serial_embedded ai)
    if(MSVC)
        set_target_properties(${target} PROPERTIES MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
    endif()
endforeach()

target_compile_definitions(ai PRIVATE UNICODE _UNICODE _WINDOWS NOMINMAX WIN32_LEAN_AND_MEAN USE_CUDA)
if(MSVC)
    target_compile_options(ai PRIVATE
        $<$<COMPILE_LANGUAGE:C,CXX>:/utf-8 /Zc:char8_t->
        $<$<COMPILE_LANGUAGE:CXX>:/W3>
        $<$<COMPILE_LANGUAGE:CXX>:/sdl>
        $<$<COMPILE_LANGUAGE:CXX>:/wd4005>
        $<$<COMPILE_LANGUAGE:CXX>:/wd4996>
        $<$<COMPILE_LANGUAGE:CXX>:/permissive->
        $<$<COMPILE_LANGUAGE:CXX>:/EHsc>)
    target_link_libraries(ai PRIVATE delayimp)
    target_link_options(ai PRIVATE "/ENTRY:mainCRTStartup" "/DELAYLOAD:nvinfer_10.dll" "/DELAYLOAD:nvonnxparser_10.dll")
endif()

target_include_directories(ai PRIVATE
    "${CMAKE_SOURCE_DIR}/third_party/ndi/include"
    "${CMAKE_SOURCE_DIR}"
    "${SRC_DIR}"
    "${SRC_DIR}/auth"
    "${SRC_DIR}/capture"
    "${SRC_DIR}/config"
    "${SRC_DIR}/crosshair"
    "${SRC_DIR}/detector"
    "${SRC_DIR}/include"
    "${SRC_DIR}/keyboard"
    "${SRC_DIR}/mem"
    "${SRC_DIR}/mouse"
    "${SRC_DIR}/overlay"
    "${SRC_DIR}/runtime"
    "${SRC_DIR}/scr"
    "${SRC_DIR}/tensorrt"
    "${SRC_DIR}/modules/makcu/include"
    "${SRC_DIR}/modules/serial/include"
    "${SRC_DIR}/modules/stb"
    "${SRC_DIR}/modules/npcap-sdk/Include"
    "${SRC_DIR}/modules/nv-codec-headers"
    "${QT_UI_DIR}"
    "${OPENCV_INCLUDE_DIR}"
    "${ONNXRUNTIME_DIR}/build/native/include"
    "${CPPWINRT_INCLUDE_DIR}"
    "${TENSORRT_ROOT}/include")

if(EXISTS "${CUDNN_ROOT}/include/12.9/cudnn.h")
    target_include_directories(ai PRIVATE "${CUDNN_ROOT}/include/12.9")
elseif(EXISTS "${CUDNN_ROOT}/include/cudnn.h")
    target_include_directories(ai PRIVATE "${CUDNN_ROOT}/include")
endif()

target_link_directories(ai PRIVATE "${TENSORRT_ROOT}/lib")
target_link_libraries(ai PRIVATE
    serial_embedded
    Qt6::Widgets
    Qt6::Network
    "${OPENCV_LIB}"
    "${ONNXRUNTIME_DIR}/runtimes/win-x64/native/onnxruntime.lib"
    CUDA::cuda_driver
    CUDA::cudart
    CUDA::nvjpeg
    CUDA::nppc
    CUDA::nppicc
    CUDA::nppidei
    "${SRC_DIR}/modules/npcap-sdk/Lib/x64/wpcap.lib"
    "${SRC_DIR}/modules/npcap-sdk/Lib/x64/Packet.lib"
    nvinfer_10
    nvonnxparser_10
    bcrypt
    winhttp
    crypt32
    # KMBox Net 走 UDP (mouse/kmbox_net/kmboxNet.cpp 用 winsock)。
    ws2_32
    d3d11
    dxgi
    d2d1
    setupapi
    cfgmgr32
    strmiids
    mf
    mfplat
    mfreadwrite
    mfuuid
    ole32
    oleaut32
    winmm
    # MMCSS (AvSetMmThreadCharacteristicsW / AvSetMmThreadPriority) —— 推理线程
    # 的调度保护, 对应原神AI 的 mmcss=true。见 runtime/sched_boost.cpp。
    avrt
    WindowsApp)

copy_runtime(ai "${SRC_DIR}/assets/apotheosis_logo.png")

copy_runtime(ai "${OPENCV_DLL}")
copy_runtime(ai "${OPENCV_VC_DIR}/bin/opencv_videoio_ffmpeg4130_64.dll")
copy_runtime(ai "${ONNXRUNTIME_DIR}/runtimes/win-x64/native/onnxruntime.dll")
copy_runtime(ai "${MSVC_REDIST_DIR}/concrt140.dll")
copy_runtime(ai "${MSVC_REDIST_DIR}/msvcp140.dll")
copy_runtime(ai "${MSVC_REDIST_DIR}/msvcp140_1.dll")
copy_runtime(ai "${MSVC_REDIST_DIR}/vcruntime140.dll")
copy_runtime(ai "${MSVC_REDIST_DIR}/vcruntime140_1.dll")
copy_runtime(ai "${WINDOWS_DEBUGGERS_X64_DIR}/dbghelp.dll")
copy_runtime(ai "${CUDAToolkit_BIN_DIR}/cudart64_12.dll")
copy_runtime(ai "${CUDAToolkit_BIN_DIR}/cublas64_12.dll")
copy_runtime(ai "${CUDAToolkit_BIN_DIR}/cublasLt64_12.dll")
copy_runtime(ai "${CUDAToolkit_BIN_DIR}/cufft64_11.dll")
copy_runtime(ai "${CUDAToolkit_BIN_DIR}/nvjpeg64_12.dll")
copy_runtime(ai "${CUDAToolkit_BIN_DIR}/nppc64_12.dll")
copy_runtime(ai "${CUDAToolkit_BIN_DIR}/nppial64_12.dll")
copy_runtime(ai "${CUDAToolkit_BIN_DIR}/nppicc64_12.dll")
copy_runtime(ai "${CUDAToolkit_BIN_DIR}/nppidei64_12.dll")
copy_runtime(ai "${CUDAToolkit_BIN_DIR}/nppif64_12.dll")
copy_runtime(ai "${CUDAToolkit_BIN_DIR}/nppig64_12.dll")
copy_runtime(ai "${CUDAToolkit_BIN_DIR}/nppim64_12.dll")
copy_runtime(ai "${CUDAToolkit_BIN_DIR}/nppist64_12.dll")
copy_runtime(ai "${CUDAToolkit_BIN_DIR}/nppitc64_12.dll")
copy_runtime(ai "${TENSORRT_ROOT}/bin/nvinfer_10.dll")
copy_runtime(ai "${TENSORRT_ROOT}/bin/nvonnxparser_10.dll")

file(GLOB TENSORRT_RESOURCE_DLLS "${TENSORRT_ROOT}/bin/nvinfer_builder_resource_*.dll")
foreach(runtime_dll IN LISTS TENSORRT_RESOURCE_DLLS)
    copy_runtime(ai "${runtime_dll}")
endforeach()

add_custom_command(TARGET ai POST_BUILD
    COMMAND powershell.exe -NoProfile -ExecutionPolicy Bypass -Command "Copy-Item -Path '${CUDNN_ROOT}/bin/12.9/*.dll' -Destination '$<TARGET_FILE_DIR:ai>' -Force"
    VERBATIM)

# Deploy Qt6 runtime DLLs next to the executable
add_custom_command(TARGET ai POST_BUILD
    COMMAND "${CMAKE_PREFIX_PATH}/bin/windeployqt.exe"
            --no-translations --no-opengl-sw --no-system-d3d-compiler
            "$<TARGET_FILE:ai>"
    VERBATIM)

# Copy Qt UI assets (stylesheet, icon font) into build dir
add_custom_command(TARGET ai POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:ai>/style"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${QT_UI_DIR}/style/theme.qss"
            "$<TARGET_FILE_DIR:ai>/style/theme.qss"
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:ai>/assets"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${QT_UI_DIR}/assets/tabler-icons.ttf"
            "$<TARGET_FILE_DIR:ai>/assets/tabler-icons.ttf"
    VERBATIM)

# ── 采集链路 bench (独立 exe; 不进主程序, 也不进 logic-tests) ────────────────
# 把「MF 投递速率」与「程序消费速率」分开量, 用来定位高帧率采集掉帧到底发生在
# 读循环(MF 串行往返/每帧拷贝) 还是解码 worker/输出队列背压。
# 参数与主程序对齐(crop_enabled=true), 用法见 tests/mfcap_bench.cpp 顶部注释。
add_executable(mfcap_bench
    "${SRC_DIR}/capture/tests/mfcap_bench.cpp"
    "${SRC_DIR}/capture/mf_capture.cpp"
    "${SRC_DIR}/capture/dshow_capture.cpp"
    "${SRC_DIR}/capture/gpu_jpeg_decoder.cpp"
    "${SRC_DIR}/capture/gpu_color_ops.cu"
    "${SRC_DIR}/mem/gpu_image.cpp")

if(MSVC)
    set_target_properties(mfcap_bench PROPERTIES MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
endif()

set_target_properties(mfcap_bench PROPERTIES
    CUDA_STANDARD 17
    CUDA_STANDARD_REQUIRED ON
    CUDA_ARCHITECTURES "75-real;86-real;86-virtual")

target_compile_definitions(mfcap_bench PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN USE_CUDA)
if(MSVC)
    target_compile_options(mfcap_bench PRIVATE
        $<$<COMPILE_LANGUAGE:C,CXX>:/utf-8 /Zc:char8_t->
        $<$<COMPILE_LANGUAGE:CXX>:/W3>)
endif()

target_include_directories(mfcap_bench PRIVATE
    "${CMAKE_SOURCE_DIR}"
    "${SRC_DIR}"
    "${SRC_DIR}/capture"
    "${SRC_DIR}/mem"
    "${SRC_DIR}/runtime"
    "${OPENCV_INCLUDE_DIR}")

target_link_libraries(mfcap_bench PRIVATE
    "${OPENCV_LIB}"
    CUDA::cudart
    CUDA::nvjpeg
    CUDA::nppc
    CUDA::nppicc
    CUDA::nppidei
    mf
    mfplat
    mfreadwrite
    mfuuid
    ole32
    oleaut32
    strmiids)

copy_runtime(mfcap_bench "${OPENCV_DLL}")
copy_runtime(mfcap_bench "${CUDAToolkit_BIN_DIR}/cudart64_12.dll")
copy_runtime(mfcap_bench "${CUDAToolkit_BIN_DIR}/nvjpeg64_12.dll")
copy_runtime(mfcap_bench "${CUDAToolkit_BIN_DIR}/nppc64_12.dll")
copy_runtime(mfcap_bench "${CUDAToolkit_BIN_DIR}/nppial64_12.dll")
copy_runtime(mfcap_bench "${CUDAToolkit_BIN_DIR}/nppicc64_12.dll")
copy_runtime(mfcap_bench "${CUDAToolkit_BIN_DIR}/nppidei64_12.dll")
copy_runtime(mfcap_bench "${CUDAToolkit_BIN_DIR}/nppig64_12.dll")
copy_runtime(mfcap_bench "${MSVC_REDIST_DIR}/msvcp140.dll")
copy_runtime(mfcap_bench "${MSVC_REDIST_DIR}/vcruntime140.dll")
copy_runtime(mfcap_bench "${MSVC_REDIST_DIR}/vcruntime140_1.dll")

# Direct KS probe uses the installed UVC driver and bypasses MF/DirectShow.
add_subdirectory(tools/ndi_sender)

# Kept separate from the application until same-mode measurements show a gain.
add_executable(ks_capture_probe "${CMAKE_SOURCE_DIR}/tools/ks_capture_probe.cpp")
target_link_libraries(ks_capture_probe PRIVATE setupapi ksuser)
if(MSVC)
    target_compile_options(ks_capture_probe PRIVATE /utf-8 /W4)
endif()

# Standalone Media Foundation mode and first-frame diagnostic for field testing.
add_executable(capture_card_dump "${SRC_DIR}/capture/tests/capture_card_dump.cpp")
target_link_libraries(capture_card_dump PRIVATE mf mfplat mfreadwrite mfuuid ole32)
if(MSVC)
    target_compile_options(capture_card_dump PRIVATE /utf-8 /W3)
endif()

add_executable(dshow_card_dump
    "${SRC_DIR}/capture/tests/dshow_card_dump.cpp"
    "${SRC_DIR}/capture/dshow_capture.cpp"
    "${SRC_DIR}/capture/gpu_color_ops.cu"
    "${SRC_DIR}/mem/gpu_image.cpp")
target_include_directories(dshow_card_dump PRIVATE "${SRC_DIR}" "${OPENCV_INCLUDE_DIR}")
target_link_libraries(dshow_card_dump PRIVATE "${OPENCV_LIB}" CUDA::cudart strmiids ole32 oleaut32)
if(MSVC)
    target_compile_options(dshow_card_dump PRIVATE
        $<$<COMPILE_LANGUAGE:CXX>:/utf-8>
        $<$<COMPILE_LANGUAGE:CXX>:/Zc:char8_t->
        $<$<COMPILE_LANGUAGE:CXX>:/W3>)
endif()
copy_runtime(dshow_card_dump "${OPENCV_DLL}")
copy_runtime(dshow_card_dump "${CUDAToolkit_BIN_DIR}/cudart64_12.dll")

# ── nvJPEG 解码路径微基准 (独立 exe) ─────────────────────────────────────────
# 用一张真实采集帧比较 split+ROI / nvjpegDecode / batched(DEFAULT|GPU_HYBRID|
# HARDWARE) 的每帧主机开销。本机只有 4 核, host 时间是要害指标。
add_executable(nvjpeg_bench "${SRC_DIR}/capture/tests/nvjpeg_bench.cpp")
# 源码含中文注释/字符串: 必须 /utf-8, 否则 MSVC 按 GBK 解析, 尾字节 0x5C 会吃掉
# 后面的转义符, 报出一堆莫名其妙的 "printf 重定义" / "缺少类型说明符"。
if(MSVC)
    target_compile_options(nvjpeg_bench PRIVATE
        $<$<COMPILE_LANGUAGE:C,CXX>:/utf-8 /Zc:char8_t->
        $<$<COMPILE_LANGUAGE:CXX>:/W3>)
endif()
target_include_directories(nvjpeg_bench PRIVATE "${SRC_DIR}/capture" "${OPENCV_INCLUDE_DIR}")
target_link_libraries(nvjpeg_bench PRIVATE CUDA::cudart CUDA::nvjpeg "${OPENCV_LIB}")
copy_runtime(nvjpeg_bench "${OPENCV_DLL}")
copy_runtime(nvjpeg_bench "${CUDAToolkit_BIN_DIR}/cudart64_12.dll")
copy_runtime(nvjpeg_bench "${CUDAToolkit_BIN_DIR}/nvjpeg64_12.dll")
copy_runtime(nvjpeg_bench "${MSVC_REDIST_DIR}/msvcp140.dll")
copy_runtime(nvjpeg_bench "${MSVC_REDIST_DIR}/vcruntime140.dll")
copy_runtime(nvjpeg_bench "${MSVC_REDIST_DIR}/vcruntime140_1.dll")

# ── 采集卡原生媒体类型枚举 (独立 exe) ────────────────────────────────────────
# 列出卡在每种格式下真实声明的分辨率/帧率, 用来判断"降 MJPG 画质换解码速度"
# 到底有没有可选的模式。不写死设备名 —— 按序号枚举。
add_executable(mjpg_modes_dump "${SRC_DIR}/capture/tests/mjpg_modes_dump.cpp")
if(MSVC)
    target_compile_options(mjpg_modes_dump PRIVATE
        $<$<COMPILE_LANGUAGE:C,CXX>:/utf-8 /Zc:char8_t->
        $<$<COMPILE_LANGUAGE:CXX>:/W3>)
endif()
target_link_libraries(mjpg_modes_dump PRIVATE mfplat mfreadwrite mfuuid mf)
copy_runtime(mjpg_modes_dump "${MSVC_REDIST_DIR}/msvcp140.dll")
copy_runtime(mjpg_modes_dump "${MSVC_REDIST_DIR}/vcruntime140.dll")
copy_runtime(mjpg_modes_dump "${MSVC_REDIST_DIR}/vcruntime140_1.dll")

# ── NV12 高帧率真伪探针 (独立 exe) ───────────────────────────────────────────
# 判定卡是否真的在 240fps 出【内容不同】的帧, 还是 120fps 出帧被驱动复制成 240。
# 三条独立判据: 到达率 / 相邻帧内容差异 / 设备时间戳间隔。
add_executable(nv12_truth_probe "${SRC_DIR}/capture/tests/nv12_truth_probe.cpp")
if(MSVC)
    target_compile_options(nv12_truth_probe PRIVATE
        $<$<COMPILE_LANGUAGE:C,CXX>:/utf-8 /Zc:char8_t->
        $<$<COMPILE_LANGUAGE:CXX>:/W3>)
endif()
target_link_libraries(nv12_truth_probe PRIVATE mfplat mfreadwrite mfuuid mf)
copy_runtime(nv12_truth_probe "${MSVC_REDIST_DIR}/msvcp140.dll")
copy_runtime(nv12_truth_probe "${MSVC_REDIST_DIR}/vcruntime140.dll")
copy_runtime(nv12_truth_probe "${MSVC_REDIST_DIR}/vcruntime140_1.dll")

# ── NV12 原始帧落盘 (独立 exe) ───────────────────────────────────────────────
# 把连续若干帧原样写盘, 用外部工具比较文件差异, 不依赖自制哈希/队列逻辑。
add_executable(nv12_dump_frames "${SRC_DIR}/capture/tests/nv12_dump_frames.cpp")
if(MSVC)
    target_compile_options(nv12_dump_frames PRIVATE
        $<$<COMPILE_LANGUAGE:C,CXX>:/utf-8 /Zc:char8_t->
        $<$<COMPILE_LANGUAGE:CXX>:/W3>)
endif()
target_link_libraries(nv12_dump_frames PRIVATE mfplat mfreadwrite mfuuid mf)
copy_runtime(nv12_dump_frames "${MSVC_REDIST_DIR}/msvcp140.dll")
copy_runtime(nv12_dump_frames "${MSVC_REDIST_DIR}/vcruntime140.dll")
copy_runtime(nv12_dump_frames "${MSVC_REDIST_DIR}/vcruntime140_1.dll")

# ── MJPG 帧体积探针 (独立 exe) ───────────────────────────────────────────────
# 量采集卡当前实际吐出的 JPEG 体积。解码耗时随体积线性增长, 所以"解码能不能
# 扛住 240fps"必须先知道真实体积 (无信号小帧会给出误导性的乐观结论)。
add_executable(mjpg_size_probe "${SRC_DIR}/capture/tests/mjpg_size_probe.cpp")
if(MSVC)
    target_compile_options(mjpg_size_probe PRIVATE
        $<$<COMPILE_LANGUAGE:C,CXX>:/utf-8 /Zc:char8_t->
        $<$<COMPILE_LANGUAGE:CXX>:/W3>)
endif()
target_link_libraries(mjpg_size_probe PRIVATE mfplat mfreadwrite mfuuid mf)
copy_runtime(mjpg_size_probe "${MSVC_REDIST_DIR}/msvcp140.dll")
copy_runtime(mjpg_size_probe "${MSVC_REDIST_DIR}/vcruntime140.dll")
copy_runtime(mjpg_size_probe "${MSVC_REDIST_DIR}/vcruntime140_1.dll")

# ── MJPEG 解码耗时 vs 体积 多点扫描 (独立 exe) ───────────────────────────────
# 用同一张真实帧重编码出多个体积档位, 多点回归判断"固定开销"是否真实存在。
# 两个样本点拟合出的截距不是证据。
add_executable(mjpg_sweep "${SRC_DIR}/capture/tests/mjpg_sweep.cpp")
if(MSVC)
    target_compile_options(mjpg_sweep PRIVATE
        $<$<COMPILE_LANGUAGE:C,CXX>:/utf-8 /Zc:char8_t->
        $<$<COMPILE_LANGUAGE:CXX>:/W3>)
endif()
target_include_directories(mjpg_sweep PRIVATE "${SRC_DIR}/capture" "${OPENCV_INCLUDE_DIR}")
target_link_libraries(mjpg_sweep PRIVATE CUDA::cudart CUDA::nvjpeg "${OPENCV_LIB}")
copy_runtime(mjpg_sweep "${OPENCV_DLL}")
copy_runtime(mjpg_sweep "${CUDAToolkit_BIN_DIR}/cudart64_12.dll")
copy_runtime(mjpg_sweep "${CUDAToolkit_BIN_DIR}/nvjpeg64_12.dll")
copy_runtime(mjpg_sweep "${MSVC_REDIST_DIR}/msvcp140.dll")
copy_runtime(mjpg_sweep "${MSVC_REDIST_DIR}/vcruntime140.dll")
copy_runtime(mjpg_sweep "${MSVC_REDIST_DIR}/vcruntime140_1.dll")

# ── CPU 亲和性修复验证 (独立 exe) ────────────────────────────────────────────
# 直接调用 CPUAffinityManager 并用 GetProcessAffinityMask 读回真实生效的核,
# 确认"请求 N 个核"确实拿到 N 个 (旧实现 4 核机器上请求 4 只得 2)。
add_executable(affinity_check
    "${SRC_DIR}/mem/tests/affinity_check.cpp"
    "${SRC_DIR}/mem/cpu_affinity_manager.cpp")
if(MSVC)
    target_compile_options(affinity_check PRIVATE
        $<$<COMPILE_LANGUAGE:C,CXX>:/utf-8 /Zc:char8_t->
        $<$<COMPILE_LANGUAGE:CXX>:/W3>)
endif()
target_include_directories(affinity_check PRIVATE "${SRC_DIR}/mem")
target_link_libraries(affinity_check PRIVATE mfplat mfreadwrite mfuuid mf)
copy_runtime(affinity_check "${MSVC_REDIST_DIR}/msvcp140.dll")
copy_runtime(affinity_check "${MSVC_REDIST_DIR}/vcruntime140.dll")
copy_runtime(affinity_check "${MSVC_REDIST_DIR}/vcruntime140_1.dll")

# ── 解码 worker 池吞吐基准 (独立 exe) ────────────────────────────────────────
# 扫 (worker 数 × 核数), 给出各组合实际吞吐, 对照 240fps 预算决定 DECODE_WORKERS。
add_executable(decode_workers_bench "${SRC_DIR}/capture/tests/decode_workers_bench.cpp")
if(MSVC)
    target_compile_options(decode_workers_bench PRIVATE
        $<$<COMPILE_LANGUAGE:C,CXX>:/utf-8 /Zc:char8_t->
        $<$<COMPILE_LANGUAGE:CXX>:/W3>)
endif()
target_link_libraries(decode_workers_bench PRIVATE CUDA::cudart CUDA::nvjpeg)
copy_runtime(decode_workers_bench "${CUDAToolkit_BIN_DIR}/cudart64_12.dll")
copy_runtime(decode_workers_bench "${CUDAToolkit_BIN_DIR}/nvjpeg64_12.dll")
copy_runtime(decode_workers_bench "${MSVC_REDIST_DIR}/msvcp140.dll")
copy_runtime(decode_workers_bench "${MSVC_REDIST_DIR}/vcruntime140.dll")
copy_runtime(decode_workers_bench "${MSVC_REDIST_DIR}/vcruntime140_1.dll")

# ── 自旋 vs 阻塞等待 CPU 代价 (独立 exe) ─────────────────────────────────────
# 量化 use_spin_wait_sync 在 4 核机器上占用多少核, 以及改阻塞同步能省回多少。
add_executable(spin_vs_block_bench "${SRC_DIR}/detector/tests/spin_wait_bench.cpp")

if(MSVC)
    target_compile_options(spin_vs_block_bench PRIVATE
        $<$<COMPILE_LANGUAGE:C,CXX>:/utf-8 /Zc:char8_t->
        $<$<COMPILE_LANGUAGE:CXX>:/W3>)
endif()
target_link_libraries(spin_vs_block_bench PRIVATE CUDA::cudart)
copy_runtime(spin_vs_block_bench "${CUDAToolkit_BIN_DIR}/cudart64_12.dll")
copy_runtime(spin_vs_block_bench "${MSVC_REDIST_DIR}/msvcp140.dll")
copy_runtime(spin_vs_block_bench "${MSVC_REDIST_DIR}/vcruntime140.dll")
copy_runtime(spin_vs_block_bench "${MSVC_REDIST_DIR}/vcruntime140_1.dll")

add_executable(crosshair_gpu_test
    "${CMAKE_SOURCE_DIR}/tests/crosshair_gpu_test.cpp"
    "${SRC_DIR}/capture/gpu_color_ops.cu")
set_target_properties(crosshair_gpu_test PROPERTIES
    CUDA_STANDARD 17 CUDA_STANDARD_REQUIRED ON
    CUDA_ARCHITECTURES "75-real;86-real;86-virtual")
target_include_directories(crosshair_gpu_test PRIVATE "${SRC_DIR}")
target_link_libraries(crosshair_gpu_test PRIVATE CUDA::cudart)
copy_runtime(crosshair_gpu_test "${CUDAToolkit_BIN_DIR}/cudart64_12.dll")

message(STATUS "Apotheosis portable build (Qt6 UI)")
message(STATUS "OpenCV: ${OPENCV_VC_DIR}")
message(STATUS "ONNX Runtime: ${ONNXRUNTIME_DIR}")
message(STATUS "TensorRT: ${TENSORRT_ROOT}")
message(STATUS "cuDNN: ${CUDNN_ROOT}")
message(STATUS "CUDA: ${CUDAToolkit_BIN_DIR}")

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
