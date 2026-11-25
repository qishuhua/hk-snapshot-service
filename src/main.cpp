// hk-snapshot-service：海康 NVR 截图服务（同步 HTTP API，Docker 部署）。
//
//   实时截图  NET_DVR_CaptureJPEGPicture_NEW（设备侧出图，不经本地解码）
//   历史截图  NET_DVR_PlayBackByTime_V40 回放流 → FFmpeg 软件全速解码
//             → 查询前 5 秒至后 3 秒内最接近的 OSD 帧编码 JPEG 返回
//
// 同步 API：一个 HTTP 请求阻塞返回 JPEG 或结构化 JSON 错误。

#include <cstdarg>
#include <stdexcept>
#include <string>

#include "common/app_log.hpp"
#include "config/config.hpp"
#include "http/http_api.hpp"
#include "sdk/sdk_env.hpp"

extern "C" {
#include <libavutil/log.h>
}

namespace {

void ffmpeg_log_to_logger(void*, int level, const char* fmt, va_list args) {
    if (level > av_log_get_level()) return;
    char line[1024]{};
    va_list copy;
    va_copy(copy, args);
    vsnprintf(line, sizeof(line), fmt, copy);
    va_end(copy);
    std::string text(line);
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r'))
        text.pop_back();
    if (text.empty()) return;
    if (level <= AV_LOG_ERROR) hkmedia::log_error("ffmpeg", text);
    else if (level <= AV_LOG_WARNING) hkmedia::log_warn("ffmpeg", text);
    else hkmedia::log_debug("ffmpeg", text);
}

} // namespace

int main() {
    using namespace hkmedia;
    try {
        const Config cfg = Config::from_env();
        create_logger(parse_log_level(cfg.log_level));
        av_log_set_level(
            spdlog::default_logger_raw()->should_log(spdlog::level::debug)
                ? AV_LOG_VERBOSE : AV_LOG_WARNING);
        av_log_set_callback(&ffmpeg_log_to_logger);

        log_info("main", "hk-snapshot-service 启动 " +
                 cfg.listen_addr + ":" + std::to_string(cfg.listen_port));
        SdkRuntime::ensure(cfg);  // 进程级 NET_DVR_Init（失败抛异常直接退出）
        struct RuntimeGuard {
            ~RuntimeGuard() noexcept {
                try { SdkRuntime::shutdown(); } catch (...) {}
            }
        } runtime_guard;
        return run_server(cfg);
    } catch (const std::exception& e) {
        log_error("main", std::string("启动失败: ") + e.what());
        return 1;
    }
}
