#include "config/config.hpp"

namespace hkmedia {

Config Config::from_env() {
    Config cfg;
    cfg.listen_addr = detail::env_str("HKMEDIA_LISTEN_ADDR", cfg.listen_addr);
    cfg.listen_port = static_cast<int>(detail::env_int("HKMEDIA_PORT", cfg.listen_port));
    cfg.max_concurrent = static_cast<int>(
        detail::env_int("HKMEDIA_MAX_CONCURRENT", cfg.max_concurrent));
    cfg.default_timeout_ms = static_cast<int>(
        detail::env_int("HKMEDIA_DEFAULT_TIMEOUT_MS", cfg.default_timeout_ms));
    cfg.jpeg_quality = static_cast<int>(detail::env_int("HKMEDIA_JPEG_QUALITY", cfg.jpeg_quality));
    cfg.picture_size = static_cast<int>(detail::env_int("HKMEDIA_PICTURE_SIZE", cfg.picture_size));
    cfg.connect_timeout_ms = static_cast<int>(
        detail::env_int("HKMEDIA_CONNECT_TIMEOUT_MS", cfg.connect_timeout_ms));
    cfg.eof_quiet_ms = static_cast<int>(detail::env_int("HKMEDIA_EOF_QUIET_MS", cfg.eof_quiet_ms));
    cfg.playback_start_idle_ms = static_cast<int>(
        detail::env_int("HKMEDIA_PLAYBACK_START_IDLE_MS", cfg.playback_start_idle_ms));
    cfg.log_level = detail::env_str("HKMEDIA_LOG_LEVEL", cfg.log_level);
    cfg.sdk_log = detail::env_int("HKMEDIA_SDK_LOG", 0) != 0;

    // 修正非法配置。
    if (cfg.listen_port <= 0 || cfg.listen_port > 65535) cfg.listen_port = kDefaultListenPort;
    if (cfg.max_concurrent < 1) cfg.max_concurrent = 1;
    if (cfg.default_timeout_ms < 1000) cfg.default_timeout_ms = 1000;
    if (cfg.default_timeout_ms > 60000) cfg.default_timeout_ms = 60000;
    if (cfg.jpeg_quality < 0 || cfg.jpeg_quality > 2) cfg.jpeg_quality = 1;
    if (cfg.picture_size < 0 || cfg.picture_size > 255) cfg.picture_size = 255;
    return cfg;
}

} // namespace hkmedia
