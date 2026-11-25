cmake_minimum_required(VERSION 3.16)
project(hk-snapshot-service VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT UNIX OR NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR
        "HCNetSDK 二进制仅提供 Linux x86_64 版本；请在 Docker（或 Linux）中构建运行")
endif()
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "HCNetSDK 二进制为 x86_64 版本，需要 64 位构建")
endif()

find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)
# FFmpeg：解码（libavcodec）+ 工具（libavutil）+ 像素转换（libswscale）
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET
    libavcodec libavutil libswscale)

# ---------------------------------------------------------------------------
# 依赖：仓库自带 HCNetSDK Linux 库；nlohmann/json、spdlog 与 cpp-httplib 走
# 系统包（Ubuntu: nlohmann-json3-dev / libspdlog-dev / libcpp-httplib-dev）
# ---------------------------------------------------------------------------
find_package(nlohmann_json 3.2.0 CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)
# libcpp-httplib-dev 不带 CMake config：头文件在默认包含路径，
# 校验 <httplib.h> 存在后直接按库名链接 -lcpp-httplib。
include(CheckIncludeFileCXX)
check_include_file_cxx("httplib.h" HAVE_HTTPLIB_H)
if(NOT HAVE_HTTPLIB_H)
    message(FATAL_ERROR "未找到 <httplib.h>；请安装 libcpp-httplib-dev")
endif()

set(HCNETSDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/HCNetSDK")
set(HCNETSDK_INCLUDE_DIR "${HCNETSDK_DIR}/include")
set(HCNETSDK_LIB_DIR "${HCNETSDK_DIR}/lib")
set(HCNETSDK_LIBRARY "${HCNETSDK_LIB_DIR}/libhcnetsdk.so")

if(NOT EXISTS "${HCNETSDK_INCLUDE_DIR}/HCNetSDK.h")
    message(FATAL_ERROR "缺少 ${HCNETSDK_INCLUDE_DIR}/HCNetSDK.h")
endif()
if(NOT EXISTS "${HCNETSDK_LIBRARY}")
    message(FATAL_ERROR "缺少 ${HCNETSDK_LIBRARY}")
endif()

add_executable(hk-snapshot-service
    src/main.cpp
    src/config/config.cpp
    src/http/http_api.cpp
    src/http/request_log.cpp
    src/capture/realtime_capture.cpp
    src/capture/history_capture.cpp
    src/media/jpeg_encoder.cpp
)
target_compile_options(hk-snapshot-service PRIVATE -Wall -Wextra)
target_include_directories(hk-snapshot-service PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${HCNETSDK_INCLUDE_DIR}")
target_link_libraries(hk-snapshot-service PRIVATE
    PkgConfig::FFMPEG
    nlohmann_json::nlohmann_json
    spdlog::spdlog
    cpp-httplib
    Threads::Threads
    "${HCNETSDK_LIBRARY}")
# 使用 RPATH 而非仅 LD_LIBRARY_PATH，本地直跑与容器内均可解析
# libhcnetsdk.so 及其 HCNetSDKCom 子目录组件。
target_link_options(hk-snapshot-service PRIVATE "LINKER:--disable-new-dtags")
set_target_properties(hk-snapshot-service PROPERTIES
    BUILD_RPATH "${HCNETSDK_LIB_DIR};${HCNETSDK_LIB_DIR}/HCNetSDKCom"
    INSTALL_RPATH "$ORIGIN/../lib;$ORIGIN/../lib/HCNetSDKCom")

install(TARGETS hk-snapshot-service RUNTIME DESTINATION bin)

option(HKMEDIA_BUILD_TESTS "构建不依赖设备的回归测试" OFF)
if(HKMEDIA_BUILD_TESTS)
    enable_testing()
    add_executable(nearest-osd-test tests/nearest_osd_test.cpp)
    target_include_directories(nearest-osd-test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    add_test(NAME nearest-osd COMMAND nearest-osd-test)
    add_executable(request-context-test tests/request_context_test.cpp)
    target_include_directories(request-context-test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_link_libraries(request-context-test PRIVATE Threads::Threads)
    add_test(NAME request-context COMMAND request-context-test)
    add_executable(es-queue-test tests/es_queue_test.cpp)
    target_include_directories(es-queue-test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_link_libraries(es-queue-test PRIVATE spdlog::spdlog Threads::Threads)
    add_test(NAME es-queue COMMAND es-queue-test)
    add_executable(app-log-test tests/app_log_test.cpp)
    target_include_directories(app-log-test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_link_libraries(app-log-test PRIVATE spdlog::spdlog)
    add_test(NAME app-log COMMAND app-log-test)
endif()

message(STATUS "HCNetSDK: ${HCNETSDK_LIB_DIR}")
message(STATUS "FFmpeg (system pkg-config): libavcodec/libavutil/libswscale，纯软解码")
