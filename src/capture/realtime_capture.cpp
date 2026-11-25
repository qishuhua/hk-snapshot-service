#include "capture/realtime_capture.hpp"

#include <HCNetSDK.h>

#include <string>
#include <vector>

#include "common/app_log.hpp"
#include "common/error.hpp"
#include "sdk/sdk_env.hpp"

namespace hkmedia {

namespace {
constexpr size_t kInitialJpegBuffer = 4 * 1024 * 1024;
constexpr size_t kMaxJpegBuffer = 64 * 1024 * 1024;
} // namespace

CaptureResult realtime_capture(const CaptureRequest& req, const Config& cfg) {
    (void)cfg;
    const int64_t started = steady_ms();
    LoginSession session(req.host, req.port, req.username, req.password, req.endpoint());

    NET_DVR_JPEGPARA para{};
    para.wPicSize = static_cast<WORD>(req.picture_size);  // 0xff = 跟随当前码流分辨率
    para.wPicQuality = static_cast<WORD>(req.jpeg_quality);

    size_t capacity = kInitialJpegBuffer;
    std::vector<char> buffer;
    while (capacity <= kMaxJpegBuffer) {
        buffer.assign(capacity, 0);
        DWORD returned = 0;
        const int64_t capture_started_ms = steady_ms();
        log_info("capture", "jpeg.capture.start user_id=" + std::to_string(session.user_id()) +
            " channel=" + std::to_string(req.channel) + " picture_size=" + std::to_string(req.picture_size) +
            " jpeg_quality=" + std::to_string(req.jpeg_quality) + " buffer_bytes=" + std::to_string(capacity));
        if (NET_DVR_CaptureJPEGPicture_NEW(session.user_id(),
                                           static_cast<LONG>(req.channel), &para,
                                           buffer.data(),
                                           static_cast<DWORD>(capacity), &returned)) {
            log_info("capture", "jpeg.capture.complete returned_bytes=" + std::to_string(returned) +
                " elapsed_ms=" + std::to_string(steady_ms() - capture_started_ms));
            if (returned == 0)
                throw CaptureError(502, "sdk_capture_failed",
                                   "设备返回了空图片 " + req.endpoint());
            CaptureResult result;
            result.jpeg.assign(buffer.begin(), buffer.begin() + returned);
            result.elapsed_ms = steady_ms() - started;
            log_info("capture", "实时截图完成 " + req.endpoint() +
                     " jpeg=" + std::to_string(result.jpeg.size()) + "B" +
                     " elapsed=" + std::to_string(result.elapsed_ms) + "ms");
            return result;
        }
        const DWORD err = NET_DVR_GetLastError();
        log_warn("capture", "jpeg.capture.failed sdk_error=" + std::to_string(err) +
            " buffer_bytes=" + std::to_string(capacity) +
            " elapsed_ms=" + std::to_string(steady_ms() - capture_started_ms));
        if (err == NET_DVR_NOENOUGH_BUF) {  // 43：缓冲太小
            capacity *= 2;
            continue;
        }
        // 常见失败：通道不支持远端抓图、NVR 对 IP 通道的能力差异、录像计划外等。
        throw CaptureError(502, "sdk_capture_failed",
                           "实时抓图失败 " + req.endpoint() +
                           "，SDK 错误码 " + std::to_string(err) +
                           "（部分设备通道不支持远端 JPEG 抓图，可改用历史模式 time=当前时间）");
    }
    throw CaptureError(502, "sdk_capture_failed",
                       "JPEG 捕获缓冲已扩至 64MB 仍不足 " + req.endpoint());
}

} // namespace hkmedia
