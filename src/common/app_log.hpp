#pragma once
// spdlog/fmt 日志：控制台彩色 + 滚动文件（100MB x 30）双 sink。
// APPNAME 默认 hk-snapshot-service；LOGDIR 不可用时回退
// /tmp/hk-snapshot-service/logs。提供两类日志接口：log_xxx(component, message)
// 带组件名；LOG_* 宏（fmt 格式串，自带文件:行号/函数名）可直接使用。

#include <cctype>
#include <chrono>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iostream>
#include <string>
#include <system_error>
#include <vector>

#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include "common/request_context.hpp"

namespace fs = std::filesystem;

inline constexpr std::size_t kLogRotateSizeBytes = 100ULL * 1024ULL * 1024ULL;
inline constexpr std::size_t kLogRotateFileCount = 30;

const std::string LOGGER_FORMAT =
    "%Y-%m-%d %H:%M:%S.%e|-|%n|-|\"%g:%#\"|-|%l|-|%!|-|%v";
const std::string LOGGER_FORMAT_COLOR =
    "%Y-%m-%d %H:%M:%S.%e|-|%n|-|\"%g:%#\"|-|%^%l%$|-|%!|-|%v";

namespace hkmedia {

// HKMEDIA_LOG_LEVEL 解析（宽容策略：大小写/数字均可）。
inline spdlog::level::level_enum parse_log_level(
    const std::string& level_str,
    spdlog::level::level_enum default_level = spdlog::level::info) {
    std::string s = level_str;
    for (auto& c : s) c = static_cast<char>(std::tolower(c));

    if (s == "trace")     return spdlog::level::trace;
    if (s == "debug")     return spdlog::level::debug;
    if (s == "info")      return spdlog::level::info;
    if (s == "warn" || s == "warning") return spdlog::level::warn;
    if (s == "err" || s == "error")    return spdlog::level::err;
    if (s == "critical")  return spdlog::level::critical;
    if (s == "off")       return spdlog::level::off;

    // 数字形式
    try {
        int n = std::stoi(s);
        if (n >= 0 && n <= 6)
            return static_cast<spdlog::level::level_enum>(n);
    } catch (...) {}

    return default_level;
}

inline void create_logger(spdlog::level::level_enum level = spdlog::level::info) {
    char* appname = std::getenv("APPNAME");
    const std::string logger_name =
        appname ? std::string(appname) : "hk-snapshot-service";
    char* log_dir = std::getenv("LOGDIR");
    fs::path log_path = log_dir ? fs::path(log_dir) : fs::path("/data/logs");
    if (spdlog::get(logger_name)) return;  // 已初始化（本服务只建一次）

    std::vector<spdlog::sink_ptr> sinks;

    auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    console_sink->set_level(level);
    console_sink->set_pattern(LOGGER_FORMAT_COLOR);
    sinks.push_back(console_sink);

    const fs::path fallback_path("/tmp/hk-snapshot-service/logs");
    for (int attempt = 0; attempt < 2; ++attempt) {
        try {
            // 目录已存在不代表可写；目录创建和日志文件打开失败均需尝试回退。
            fs::create_directories(log_path);
            const auto log_file = (log_path / (logger_name + ".log")).string();
            auto file_sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
                log_file, kLogRotateSizeBytes, kLogRotateFileCount, false);
            file_sink->set_level(level);
            file_sink->set_pattern(LOGGER_FORMAT);
            sinks.push_back(file_sink);
            break;
        } catch (const std::exception& e) {
            if (attempt == 0 && log_path != fallback_path) {
                std::cerr << "Cannot use log directory " << log_path
                          << ": " << e.what()
                          << "; fallback to " << fallback_path << std::endl;
                log_path = fallback_path;
                continue;
            }
            std::cerr << "File logging disabled: " << e.what() << std::endl;
            break;
        }
    }

    auto logger = std::make_shared<spdlog::logger>(
        logger_name, sinks.begin(), sinks.end());
    logger->set_level(level);
    logger->flush_on(spdlog::level::info);  // 请求开始与关键阶段及时刷新，便于定位异常退出位置。
    spdlog::register_logger(logger);
    spdlog::set_default_logger(logger);
}

// 存量接口由调用处的宏传入源码位置，保留请求上下文和运行时级别过滤。
// 不使用 SPDLOG_LOGGER_DEBUG，避免编译期裁剪导致 HKMEDIA_LOG_LEVEL=debug 失效。
inline void log_debug_at(spdlog::source_loc location, const std::string& c, const std::string& m) {
    spdlog::default_logger_raw()->log(location, spdlog::level::debug,
        "[request_id={}] [{}] {}", current_request_id(), c, m);
}
inline void log_info_at(spdlog::source_loc location, const std::string& c, const std::string& m) {
    spdlog::default_logger_raw()->log(location, spdlog::level::info,
        "[request_id={}] [{}] {}", current_request_id(), c, m);
}
inline void log_warn_at(spdlog::source_loc location, const std::string& c, const std::string& m) {
    spdlog::default_logger_raw()->log(location, spdlog::level::warn,
        "[request_id={}] [{}] {}", current_request_id(), c, m);
}
inline void log_error_at(spdlog::source_loc location, const std::string& c, const std::string& m) {
    spdlog::default_logger_raw()->log(location, spdlog::level::err,
        "[request_id={}] [{}] {}", current_request_id(), c, m);
}

inline int64_t steady_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}

} // namespace hkmedia

// 宏在调用处展开，兼容 log_info(...) 和 hkmedia::log_info(...) 两种写法。
#define log_debug(c, m) log_debug_at(spdlog::source_loc{__FILE__, __LINE__, SPDLOG_FUNCTION}, (c), (m))
#define log_info(c, m)  log_info_at(spdlog::source_loc{__FILE__, __LINE__, SPDLOG_FUNCTION}, (c), (m))
#define log_warn(c, m)  log_warn_at(spdlog::source_loc{__FILE__, __LINE__, SPDLOG_FUNCTION}, (c), (m))
#define log_error(c, m) log_error_at(spdlog::source_loc{__FILE__, __LINE__, SPDLOG_FUNCTION}, (c), (m))

// 新代码可用 fmt 风格 + 文件:行号/函数名：LOG_INFO("连接 {} 失败: {}", host, err);
#define LOG_TRACE(...)    SPDLOG_LOGGER_TRACE(spdlog::default_logger_raw(), __VA_ARGS__)
#define LOG_DEBUG(...)    SPDLOG_LOGGER_DEBUG(spdlog::default_logger_raw(), __VA_ARGS__)
#define LOG_INFO(...)     SPDLOG_LOGGER_INFO(spdlog::default_logger_raw(), __VA_ARGS__)
#define LOG_WARN(...)     SPDLOG_LOGGER_WARN(spdlog::default_logger_raw(), __VA_ARGS__)
#define LOG_ERROR(...)    SPDLOG_LOGGER_ERROR(spdlog::default_logger_raw(), __VA_ARGS__)
#define LOG_CRITICAL(...) SPDLOG_LOGGER_CRITICAL(spdlog::default_logger_raw(), __VA_ARGS__)
