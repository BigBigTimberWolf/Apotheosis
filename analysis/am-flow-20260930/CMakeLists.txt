cmake_minimum_required(VERSION 3.20)
project(am_flow_audit LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(APP "${CMAKE_CURRENT_LIST_DIR}/../../Apotheosis")
add_executable(flow_probe flow_probe.cpp "${APP}/control/recovered_pid.cpp"
 "${APP}/control/types.cpp" "${APP}/control/anchor.cpp"
 "${APP}/control/recovered_tracker.cpp" "${APP}/control/recovered_aim_controller.cpp")
target_include_directories(flow_probe PRIVATE "${APP}")
if(MSVC)
 target_compile_options(flow_probe PRIVATE /utf-8)
endif()
