find_package(Threads REQUIRED)

# 界面几何测试需要 Qt。用 QUIET + 可选: 逻辑测试的卖点是任何平台都能单独编

# 找不到就跳过这一类测试, 其余照常。
find_package(Qt6 QUIET COMPONENTS Widgets)

set(APP_SOURCE_ROOT "${CMAKE_CURRENT_LIST_DIR}/../Apotheosis")
function(add_logic_test target source)
    # 模板化的源文件列表(通过 ARGN 传入额外源文件), 源文件里带 main 函数的是 ${source}
    add_executable(${target} "${source}" ${ARGN})
    target_include_directories(${target} PRIVATE "${APP_SOURCE_ROOT}")
    target_compile_features(${target} PRIVATE cxx_std_20)
    target_link_libraries(${target} PRIVATE Threads::Threads)
    set_target_properties(${target} PROPERTIES AUTOMOC OFF)
    if(MSVC)
        target_compile_options(${target} PRIVATE /utf-8 /Zc:char8_t-)
    else()
        target_compile_options(${target} PRIVATE -fno-char8_t)
    endif()
    add_test(NAME ${target} COMMAND ${target})
    set_tests_properties(${target} PROPERTIES TIMEOUT 20)
endfunction()

# ══════════════════════════════════════════════════════════════════════════════
#  ★ 2026-09-17: 瞄准控制链已整条删除, 本文件随之大幅收缩。
#
#  被删掉的测试(连同它们的被测源文件一起 —— 那些源文件已不存在):
#    aim_pid_test / aim_tracker_test / aim_eventsync_test / aim_pid_am_test /
#    aim_path_test / trigger_scope_test / aim_pid_quantum_test / auto_stop_test /
#    inflight_comp_test / dt_smooth_test / aim_scale_test / aim_acceptance_test /
#    autotune_test / autotune_reload_test / autotune_layout_test
#
#  这些回归钉的是 PID 死区、在途补偿、尺度调度、提前量与调参 agent —— 全是
#  已被删除的模块。它们的断言对象不存在了, 留着只会编不过。
#
#  现在剩下的都是【采集 / 推理 / 运行期数据面】这一侧的回归。
# ══════════════════════════════════════════════════════════════════════════════

add_logic_test(latency_probe_test "${APP_SOURCE_ROOT}/runtime/tests/latency_probe_test.cpp")
add_logic_test(frame_crosshair_test "${CMAKE_CURRENT_LIST_DIR}/frame_crosshair_test.cpp")
add_logic_test(am_centroid_test "${CMAKE_CURRENT_LIST_DIR}/am_centroid_test.cpp")
add_logic_test(latest_control_worker_test "${CMAKE_CURRENT_LIST_DIR}/latest_control_worker_test.cpp")
add_logic_test(am_trigger_test "${CMAKE_CURRENT_LIST_DIR}/am_trigger_test.cpp")
add_logic_test(button_priority_test "${CMAKE_CURRENT_LIST_DIR}/button_priority_test.cpp")
add_logic_test(ferrum_protocol_test "${CMAKE_CURRENT_LIST_DIR}/ferrum_protocol_test.cpp")
add_logic_test(macro_rule_test "${CMAKE_CURRENT_LIST_DIR}/macro_rule_test.cpp")
if(WIN32 AND OPENCV_LIB AND TARGET CUDA::cudart)
    add_logic_test(macro_sources_test "${CMAKE_CURRENT_LIST_DIR}/macro_sources_test.cpp"
        "${APP_SOURCE_ROOT}/macro/rule_sources.cpp" "${APP_SOURCE_ROOT}/mem/gpu_image.cpp")
    target_include_directories(macro_sources_test PRIVATE "${OPENCV_INCLUDE_DIR}")
    target_link_libraries(macro_sources_test PRIVATE "${OPENCV_LIB}" CUDA::cudart windowsapp winmm user32)
    target_compile_definitions(macro_sources_test PRIVATE NOMINMAX)
    copy_runtime(macro_sources_test "${OPENCV_DLL}")
    copy_runtime(macro_sources_test "${CUDAToolkit_BIN_DIR}/cudart64_12.dll")
endif()
if(Qt6Widgets_FOUND)
    add_executable(macro_rule_ui_test "${CMAKE_CURRENT_LIST_DIR}/macro_rule_ui_test.cpp"
        "${CMAKE_SOURCE_DIR}/qt_ui/widgets/MacroRuleEditor.cpp"
        "${CMAKE_SOURCE_DIR}/qt_ui/widgets/FormKit.cpp"
        "${CMAKE_SOURCE_DIR}/qt_ui/widgets/ToggleSwitch.cpp"
        "${CMAKE_SOURCE_DIR}/qt_ui/widgets/ToggleSwitch.h")
    target_include_directories(macro_rule_ui_test PRIVATE "${APP_SOURCE_ROOT}" "${CMAKE_SOURCE_DIR}/qt_ui")
    target_link_libraries(macro_rule_ui_test PRIVATE Qt6::Widgets)
    target_compile_features(macro_rule_ui_test PRIVATE cxx_std_20)
    set_target_properties(macro_rule_ui_test PROPERTIES AUTOMOC ON)
    target_compile_options(macro_rule_ui_test PRIVATE /utf-8 /Zc:char8_t-)
    get_target_property(_macro_qt_core Qt6::Core IMPORTED_LOCATION_RELEASE)
    get_filename_component(_macro_qt_bin "${_macro_qt_core}" DIRECTORY)
    add_test(NAME macro_rule_ui_test COMMAND macro_rule_ui_test)
    set_tests_properties(macro_rule_ui_test PROPERTIES TIMEOUT 20 ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${_macro_qt_bin}")
endif()
add_logic_test(ndi_latest_slot_test "${CMAKE_CURRENT_LIST_DIR}/ndi_latest_slot_test.cpp")
add_logic_test(capture_card_caps_test "${APP_SOURCE_ROOT}/capture/tests/capture_card_caps_test.cpp")
if(WIN32 AND OPENCV_LIB AND CUDAToolkit_FOUND)
    add_executable(ndi_capture_test
        "${CMAKE_CURRENT_LIST_DIR}/ndi_capture_test.cpp"
        "${APP_SOURCE_ROOT}/capture/ndi_capture.cpp"
        "${APP_SOURCE_ROOT}/ndi/ndi_runtime.cpp")
    target_include_directories(ndi_capture_test PRIVATE
        "${APP_SOURCE_ROOT}" "${OPENCV_INCLUDE_DIR}" "${CMAKE_CURRENT_LIST_DIR}/../third_party/ndi/include")
    target_link_libraries(ndi_capture_test PRIVATE "${OPENCV_LIB}" CUDA::cudart)
    target_compile_features(ndi_capture_test PRIVATE cxx_std_20)
    target_compile_definitions(ndi_capture_test PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    if(MSVC)
        target_compile_options(ndi_capture_test PRIVATE /utf-8 /Zc:char8_t-)
    endif()
    copy_runtime(ndi_capture_test "${OPENCV_DLL}")
    copy_runtime(ndi_capture_test "${CUDAToolkit_BIN_DIR}/cudart64_12.dll")
    add_dependencies(ndi_capture_test ndi_sender)
    add_test(NAME ndi_capture_test COMMAND ndi_capture_test --sender "$<TARGET_FILE:ndi_sender>")
    set_tests_properties(ndi_capture_test PROPERTIES TIMEOUT 40 SKIP_RETURN_CODE 77)
    add_executable(network_capture_test
        "${CMAKE_CURRENT_LIST_DIR}/network_capture_test.cpp"
        "${APP_SOURCE_ROOT}/capture/stream_capture.cpp")
    target_include_directories(network_capture_test PRIVATE
        "${APP_SOURCE_ROOT}" "${OPENCV_INCLUDE_DIR}")
    target_link_libraries(network_capture_test PRIVATE
        "${OPENCV_LIB}" CUDA::cudart ws2_32)
    target_compile_features(network_capture_test PRIVATE cxx_std_20)
    if(MSVC)
        target_compile_options(network_capture_test PRIVATE /utf-8 /Zc:char8_t-)
    endif()
    copy_runtime(network_capture_test "${OPENCV_DLL}")
    copy_runtime(network_capture_test "${OPENCV_VC_DIR}/bin/opencv_videoio_ffmpeg4130_64.dll")
    add_test(NAME network_capture_test COMMAND network_capture_test)
    set_tests_properties(network_capture_test PROPERTIES TIMEOUT 25)
endif()
add_logic_test(device_frame_age_test "${CMAKE_CURRENT_LIST_DIR}/device_frame_age_test.cpp")
add_logic_test(interruptible_slot_test "${CMAKE_CURRENT_LIST_DIR}/interruptible_slot_test.cpp")
add_logic_test(gpu_ready_event_test "${CMAKE_CURRENT_LIST_DIR}/gpu_ready_event_test.cpp")
# This test verifies event ownership with an instrumented CUDA API, not GPU work.
target_include_directories(gpu_ready_event_test BEFORE PRIVATE "${CMAKE_CURRENT_LIST_DIR}/stubs")
add_logic_test(raw_frame_layout_test "${CMAKE_CURRENT_LIST_DIR}/raw_frame_layout_test.cpp")
if(OPENCV_LIB)
    add_executable(crosshair_exact_hsv_test
        "${CMAKE_CURRENT_LIST_DIR}/crosshair_exact_hsv_test.cpp"
        "${APP_SOURCE_ROOT}/crosshair/crosshair_detector.cpp")
    target_include_directories(crosshair_exact_hsv_test PRIVATE
        "${APP_SOURCE_ROOT}" "${OPENCV_INCLUDE_DIR}")
    target_link_libraries(crosshair_exact_hsv_test PRIVATE "${OPENCV_LIB}")
    target_compile_features(crosshair_exact_hsv_test PRIVATE cxx_std_20)
    if(MSVC)
        target_compile_options(crosshair_exact_hsv_test PRIVATE /utf-8 /Zc:char8_t-)
    endif()
    if(WIN32)
        copy_runtime(crosshair_exact_hsv_test "${OPENCV_DLL}")
    endif()
    add_test(NAME crosshair_exact_hsv_test COMMAND crosshair_exact_hsv_test)
endif()
add_logic_test(weapon_switch31_test "${CMAKE_CURRENT_LIST_DIR}/weapon_switch31_test.cpp")
add_logic_test(switch31_shot_gate_test "${CMAKE_CURRENT_LIST_DIR}/switch31_shot_gate_test.cpp")
if(WIN32)
    add_logic_test(dhzbox_driver_test "${CMAKE_CURRENT_LIST_DIR}/dhzbox_driver_test.cpp"
        "${APP_SOURCE_ROOT}/mouse/dhzbox_driver.cpp")
    target_link_libraries(dhzbox_driver_test PRIVATE ws2_32)
endif()
add_logic_test(neural_curve_test "${CMAKE_CURRENT_LIST_DIR}/neural_curve_test.cpp")
add_logic_test(model_output_contract_test "${CMAKE_CURRENT_LIST_DIR}/model_output_contract_test.cpp")

if(TENSORRT_ROOT AND EXISTS "${TENSORRT_ROOT}/include/NvInfer.h")
    add_logic_test(trt_build_monitor_test "${CMAKE_CURRENT_LIST_DIR}/trt_build_monitor_test.cpp")
    target_include_directories(trt_build_monitor_test PRIVATE "${TENSORRT_ROOT}/include")
endif()

if(Qt6Widgets_FOUND)
    add_executable(neural_curve_ui_smoke
        "${CMAKE_CURRENT_LIST_DIR}/neural_curve_ui_smoke.cpp"
        "${CMAKE_SOURCE_DIR}/qt_ui/widgets/NeuralCurveTrainer.cpp"
        "${CMAKE_SOURCE_DIR}/qt_ui/widgets/NeuralCurveTrainer.h"
        "${CMAKE_SOURCE_DIR}/qt_ui/widgets/CurveCanvas.cpp"
        "${CMAKE_SOURCE_DIR}/qt_ui/widgets/CurveCanvas.h")
    target_include_directories(neural_curve_ui_smoke PRIVATE
        "${APP_SOURCE_ROOT}" "${CMAKE_SOURCE_DIR}/qt_ui")
    target_compile_features(neural_curve_ui_smoke PRIVATE cxx_std_20)
    target_link_libraries(neural_curve_ui_smoke PRIVATE Qt6::Widgets Threads::Threads)
    set_target_properties(neural_curve_ui_smoke PROPERTIES AUTOMOC ON)
    if(MSVC)
        target_compile_options(neural_curve_ui_smoke PRIVATE /utf-8 /Zc:char8_t-)
    else()
        target_compile_options(neural_curve_ui_smoke PRIVATE -fno-char8_t)
    endif()
    add_test(NAME neural_curve_ui_smoke COMMAND neural_curve_ui_smoke)
    # ★ 测试 exe 旁边没有 Qt6*.dll。Qt 运行时不在 DLL 搜索路径上时, 进程会在
    #   main() 之前以 STATUS_DLL_NOT_FOUND (0xC0000135) 退出; ctest 只会看到
    #   "超时 20s", 看起来像测试逻辑挂了 —— 实际是根本没跑起来。
    #   所以把 Qt 的 bin 目录前置进测试进程的 PATH。
    set(_neural_qt_core "")
    foreach(_qt_loc_prop IMPORTED_LOCATION_RELEASE IMPORTED_LOCATION_NOCONFIG IMPORTED_LOCATION)
        if(NOT _neural_qt_core)
            get_target_property(_qt_try Qt6::Core ${_qt_loc_prop})
            if(_qt_try AND NOT _qt_try MATCHES "-NOTFOUND$")
                set(_neural_qt_core "${_qt_try}")
            endif()
        endif()
    endforeach()
    if(_neural_qt_core)
        get_filename_component(_neural_qt_bin "${_neural_qt_core}" DIRECTORY)
        set_tests_properties(neural_curve_ui_smoke PROPERTIES
            TIMEOUT 20 ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
            ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${_neural_qt_bin}")
    else()
        set_tests_properties(neural_curve_ui_smoke PROPERTIES
            TIMEOUT 20 ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    endif()

    add_executable(input_method_config_test
        "${CMAKE_CURRENT_LIST_DIR}/input_method_config_test.cpp"
        "${CMAKE_SOURCE_DIR}/qt_ui/config/ConfigManager.cpp"
        "${CMAKE_SOURCE_DIR}/qt_ui/config/ConfigManager.h")
    target_include_directories(input_method_config_test PRIVATE
        "${APP_SOURCE_ROOT}" "${APP_SOURCE_ROOT}/config" "${CMAKE_SOURCE_DIR}/qt_ui")
    target_compile_features(input_method_config_test PRIVATE cxx_std_20)
    target_link_libraries(input_method_config_test PRIVATE Qt6::Core)
    set_target_properties(input_method_config_test PROPERTIES AUTOMOC ON)
    if(MSVC)
        target_compile_options(input_method_config_test PRIVATE /utf-8 /Zc:char8_t-)
    endif()
    add_test(NAME input_method_config_test COMMAND input_method_config_test)
    if(_neural_qt_core)
        set_tests_properties(input_method_config_test PROPERTIES
            ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${_neural_qt_bin}")
    endif()

    add_executable(trigger_workflow_ui_test
        "${CMAKE_CURRENT_LIST_DIR}/trigger_workflow_ui_test.cpp"
        "${CMAKE_SOURCE_DIR}/qt_ui/widgets/TriggerWorkflowEditor.cpp"
        "${CMAKE_SOURCE_DIR}/qt_ui/widgets/TriggerTargetEditor.cpp"
        "${CMAKE_SOURCE_DIR}/qt_ui/widgets/FormKit.cpp"
        "${CMAKE_SOURCE_DIR}/qt_ui/widgets/ToggleSwitch.cpp"
        "${CMAKE_SOURCE_DIR}/qt_ui/widgets/ToggleSwitch.h")
    target_include_directories(trigger_workflow_ui_test PRIVATE
        "${APP_SOURCE_ROOT}" "${APP_SOURCE_ROOT}/config" "${CMAKE_SOURCE_DIR}/qt_ui")
    target_link_libraries(trigger_workflow_ui_test PRIVATE Qt6::Widgets)
    target_compile_features(trigger_workflow_ui_test PRIVATE cxx_std_20)
    set_target_properties(trigger_workflow_ui_test PROPERTIES AUTOMOC ON)
    if(MSVC)
        target_compile_options(trigger_workflow_ui_test PRIVATE /utf-8 /Zc:char8_t-)
    endif()
    add_test(NAME trigger_workflow_ui_test COMMAND trigger_workflow_ui_test)
    if(_neural_qt_core)
        set_tests_properties(trigger_workflow_ui_test PROPERTIES TIMEOUT 20
            ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
            ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${_neural_qt_bin}")
    else()
        set_tests_properties(trigger_workflow_ui_test PROPERTIES TIMEOUT 20
            ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    endif()

    add_executable(overview_performance_button_test
        "${CMAKE_CURRENT_LIST_DIR}/overview_performance_button_test.cpp"
        "${CMAKE_SOURCE_DIR}/qt_ui/pages/OverviewPage.cpp"
        "${CMAKE_SOURCE_DIR}/qt_ui/widgets/MetricCard.cpp"
        "${CMAKE_SOURCE_DIR}/qt_ui/widgets/TelemetryChart.cpp"
        "${CMAKE_SOURCE_DIR}/qt_ui/widgets/IconFont.cpp")
    target_include_directories(overview_performance_button_test PRIVATE
        "${CMAKE_SOURCE_DIR}/qt_ui")
    target_link_libraries(overview_performance_button_test PRIVATE Qt6::Widgets)
    target_compile_features(overview_performance_button_test PRIVATE cxx_std_20)
    set_target_properties(overview_performance_button_test PROPERTIES AUTOMOC ON)
    if(MSVC)
        target_compile_options(overview_performance_button_test PRIVATE /utf-8 /Zc:char8_t-)
    endif()
    add_test(NAME overview_performance_button_test COMMAND overview_performance_button_test)
    if(_neural_qt_core)
        set_tests_properties(overview_performance_button_test PROPERTIES TIMEOUT 20
            ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
            ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${_neural_qt_bin}")
    else()
        set_tests_properties(overview_performance_button_test PROPERTIES TIMEOUT 20
            ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    endif()
endif()

# ── 移植控制器的数值与状态回归 ────────────────────────────────────────────────
add_logic_test(recovered_controller_test "${CMAKE_CURRENT_LIST_DIR}/recovered_controller_test.cpp"
    "${APP_SOURCE_ROOT}/runtime/aim_loop_config.cpp"
    "${APP_SOURCE_ROOT}/control/types.cpp"
    "${APP_SOURCE_ROOT}/control/anchor.cpp"
    "${APP_SOURCE_ROOT}/control/recovered_pid.cpp"
    "${APP_SOURCE_ROOT}/control/recovered_tracker.cpp"
    "${APP_SOURCE_ROOT}/control/recovered_aim_controller.cpp")
add_logic_test(trigger_snap_return_test "${CMAKE_CURRENT_LIST_DIR}/trigger_snap_return_test.cpp"
    "${APP_SOURCE_ROOT}/control/types.cpp")
add_logic_test(motion_feedback_window_test "${CMAKE_CURRENT_LIST_DIR}/motion_feedback_window_test.cpp")
add_logic_test(aimpoint_prediction_test "${CMAKE_CURRENT_LIST_DIR}/aimpoint_prediction_test.cpp"
    "${APP_SOURCE_ROOT}/control/types.cpp"
    "${APP_SOURCE_ROOT}/control/anchor.cpp"
    "${APP_SOURCE_ROOT}/control/recovered_pid.cpp"
    "${APP_SOURCE_ROOT}/control/recovered_tracker.cpp"
    "${APP_SOURCE_ROOT}/control/recovered_aim_controller.cpp")

add_logic_test(hotkey_selection_test "${CMAKE_CURRENT_LIST_DIR}/hotkey_selection_test.cpp")
if(WIN32)
    add_logic_test(kmbox_net_monitor_test "${CMAKE_CURRENT_LIST_DIR}/kmbox_net_monitor_test.cpp"
        "${APP_SOURCE_ROOT}/mouse/kmbox_net/kmboxNet.cpp")
    target_include_directories(kmbox_net_monitor_test PRIVATE "${APP_SOURCE_ROOT}/mouse")
    target_compile_definitions(kmbox_net_monitor_test PRIVATE NOMINMAX)
    target_link_libraries(kmbox_net_monitor_test PRIVATE ws2_32)
endif()
if(Qt6Widgets_FOUND)
    add_logic_test(hotkey_activation_ui_test "${CMAKE_CURRENT_LIST_DIR}/hotkey_activation_ui_test.cpp"
        "${CMAKE_SOURCE_DIR}/qt_ui/widgets/HotkeyActivationWidget.cpp"
        "${CMAKE_SOURCE_DIR}/qt_ui/widgets/HotkeyActivationWidget.h")
    target_include_directories(hotkey_activation_ui_test PRIVATE "${CMAKE_SOURCE_DIR}/qt_ui")
    target_link_libraries(hotkey_activation_ui_test PRIVATE Qt6::Widgets)
    set_target_properties(hotkey_activation_ui_test PROPERTIES AUTOMOC ON)
    set_tests_properties(hotkey_activation_ui_test PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    if(_neural_qt_bin)
        set_tests_properties(hotkey_activation_ui_test PROPERTIES
            ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${_neural_qt_bin}")
    endif()
endif()

# ── ★★ 自动扳机 + 轨迹曲线回归 (2026-09-17 恢复) ─────────────────────────────
#
# ★ 这两个后端是【从已删除状态重建】的: 原来的扳机 FSM 住在 1536 行的
#   runtime/mouse_thread_loop.cpp 里, 轨迹整形住在 mouse/aim_path.h —— 两者
#   都在"只留采集+推理"那轮被删。现在按用户要求加回来。
#
# ★★ 它们【零依赖】(不引 OpenCV / Windows / Qt), 所以任何平台都能编 ——
#    这正是把它们从那个巨文件里拆成独立头文件的理由之一。
#    留在巨文件里就是零覆盖: "编过"都不算, 更别说"跑对"。
#
# ★ 钉的核心不变量:
#   触发侧 —— 零延迟直通、延迟计时中途离开要重来、连点/长按的相位区别、
#             转火冷却只在"换目标且不在命中区"时生效、reset 必须归还左键。
#   轨迹侧 —— 直线模式逐位透传、【四种模式只旋转不缩放】、WindMouse 门控旁路、
#             同一 target_id 轨迹可复现、reset 后等价于全新驱动器。
add_logic_test(trigger_path_test "${CMAKE_CURRENT_LIST_DIR}/trigger_path_test.cpp")
add_logic_test(color_lab_test "${CMAKE_CURRENT_LIST_DIR}/color_lab_test.cpp")
add_logic_test(trigger_target_test "${CMAKE_CURRENT_LIST_DIR}/trigger_target_test.cpp"
    "${APP_SOURCE_ROOT}/control/types.cpp")
add_logic_test(auto_stop_timing_test "${CMAKE_CURRENT_LIST_DIR}/auto_stop_timing_test.cpp")

# ── 配置解析 (2026-09-14) ─────────────────────────────────────────────────────
# 需要 config.cpp (解析/夹取) 与 SimpleIni。
# ★ NOMINMAX 是必须的: config.cpp 与 SimpleIni.h 都会用到 std::min/max, 而
#   windows.h 的 min/max 宏会把它们撕成语法错误。主程序是通过 Apotheosis.h 里
#   的定义拿到的, 逻辑测试没有那一层, 所以要显式加。
if(EXISTS "${APP_SOURCE_ROOT}/modules/SimpleIni.h")
add_logic_test(config_migration_test "${CMAKE_CURRENT_LIST_DIR}/config_migration_test.cpp"
    "${APP_SOURCE_ROOT}/config/config.cpp")
target_compile_definitions(config_migration_test PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

# ── 硬件驱动抽象单元测试 (2026-09-15) ─────────────────────────────────────────
#
# 验证 AimMagic 驱动工厂形状的抽象层契约:
#   三家后端名称 / 能力位中文串查 / 状态串理由 / 不认识的名称拒绝 /
#   各包装器在空指针下的安全降级。
#
# ★ 独立可执行文件: 链真实的 mouse_driver.cpp + 依赖库(Makcu / serial / ws2_32),
#   不需要 Qt 或推理运行时。
#
# ★★ 这是【唯一一个在 macOS 上不编译】的逻辑测试(WIN32 守卫)。
#    驱动层没有跨平台替身, 所以本机跑 ctest 时它不在列表里 —— 那是覆盖缺口,
#    不是"它通过了"。
if(WIN32)
add_executable(mouse_driver_test "${CMAKE_CURRENT_LIST_DIR}/mouse_driver_test.cpp"
    "${APP_SOURCE_ROOT}/mouse/mouse_driver.cpp"
    "${APP_SOURCE_ROOT}/mouse/dhzbox_driver.cpp"
    "${APP_SOURCE_ROOT}/mouse/ferrum_driver.cpp"
    "${APP_SOURCE_ROOT}/mouse/cat_driver.cpp"
    "${APP_SOURCE_ROOT}/mouse/windows_driver.cpp"
    "${APP_SOURCE_ROOT}/mouse/Makcu.cpp"
    "${APP_SOURCE_ROOT}/mouse/MakcuNew.cpp"
    "${APP_SOURCE_ROOT}/mouse/kmboxNetConnection.cpp"
    "${APP_SOURCE_ROOT}/mouse/kmbox_net/kmboxNet.cpp"
    "${APP_SOURCE_ROOT}/modules/makcu/src/makcu.cpp"
    "${APP_SOURCE_ROOT}/modules/makcu/src/serialport.cpp"
    "${APP_SOURCE_ROOT}/modules/serial/src/serial.cc"
    "${APP_SOURCE_ROOT}/modules/serial/src/impl/win.cc"
    "${APP_SOURCE_ROOT}/modules/serial/src/impl/list_ports/list_ports_win.cc")
target_include_directories(mouse_driver_test PRIVATE
    "${CMAKE_SOURCE_DIR}"
    "${APP_SOURCE_ROOT}"
    "${APP_SOURCE_ROOT}/config"
    "${APP_SOURCE_ROOT}/include"
    "${APP_SOURCE_ROOT}/mouse"
    "${APP_SOURCE_ROOT}/modules/makcu/include"
    "${APP_SOURCE_ROOT}/modules/serial/include")
target_compile_features(mouse_driver_test PRIVATE cxx_std_20)
target_compile_definitions(mouse_driver_test PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(mouse_driver_test PRIVATE ws2_32 setupapi bcrypt)
if(MSVC)
    target_compile_options(mouse_driver_test PRIVATE /utf-8 /Zc:char8_t-)
endif()
add_test(NAME mouse_driver_test COMMAND mouse_driver_test)
set_tests_properties(mouse_driver_test PROPERTIES TIMEOUT 10)

add_executable(kmbox_net_loopback_test
    "${CMAKE_CURRENT_LIST_DIR}/kmbox_net_loopback_test.cpp"
    "${APP_SOURCE_ROOT}/mouse/kmbox_net/kmboxNet.cpp")
target_include_directories(kmbox_net_loopback_test PRIVATE
    "${APP_SOURCE_ROOT}" "${APP_SOURCE_ROOT}/mouse")
target_compile_features(kmbox_net_loopback_test PRIVATE cxx_std_20)
target_compile_definitions(kmbox_net_loopback_test PRIVATE NOMINMAX)
target_link_libraries(kmbox_net_loopback_test PRIVATE ws2_32)
if(MSVC)
    target_compile_options(kmbox_net_loopback_test PRIVATE /utf-8)
endif()
add_test(NAME kmbox_net_loopback_test COMMAND kmbox_net_loopback_test)
set_tests_properties(kmbox_net_loopback_test PROPERTIES TIMEOUT 10)
endif()
