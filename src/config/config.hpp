#pragma once
// 服务配置：全部来自环境变量（容器友好），未设置时取默认值。

#include <cstddef>
#include <cstdlib>
#include <string>

namespace hkmedia {

inline constexpr int kDefaultListenPort = 8180;

struct Config {
    std::string listen_addr = "0.0.0.0";
    int listen_port = kDefaultListenPort;
    int max_concurrent = 16;           // 每台设备的截图并发请求上限
    int default_timeout_ms = 15000;    // 单请求默认超时（上限 60000）
    int jpeg_quality = 1;              // 默认 JPEG 档位 0/1/2
    int picture_size = 255;            // 实时抓图分辨率档（0xff 跟随码流）
    int connect_timeout_ms = 5000;     // NET_DVR_SetConnectTime
    int eof_quiet_ms = 2000;           // 回放进度 100% 后的静默判定时长
    int playback_start_idle_ms = 8000; // 回放启动后等待首帧的最长时间
    std::string log_level = "info";    // debug/info/warn/error
    bool sdk_log = false;              // NET_DVR_SetLogToFile（排障用，写入 ./sdk_log）

    static Config from_env();
};

namespace detail {
inline std::string env_str(const char* name, const std::string& fallback) {
    const char* value = std::getenv(name);
    return (value && *value) ? std::string(value) : fallback;
}
inline long env_int(const char* name, long fallback) {
    const char* value = std::getenv(name);
    if (!value || !*value) return fallback;
    char* end = nullptr;
    const long parsed = std::strtol(value, &end, 10);
    return (end && *end == '\0') ? parsed : fallback;
}
} // namespace detail

} // namespace hkmedia
