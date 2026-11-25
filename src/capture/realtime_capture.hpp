#pragma once
// 实时截图：NET_DVR_CaptureJPEGPicture_NEW 让设备/前置通道直接输出 JPEG，
// 不经过本地解码，延迟最低。适合“当前画面”截图。

#include "model/capture_types.hpp"
#include "config/config.hpp"

namespace hkmedia {

// 每个请求独立登录 → 抓图 → 注销。捕获缓冲从 4MB 起步，SDK 返回
// NET_DVR_NOENOUGH_BUF(43) 时逐次加倍（最高 64MB）重试。
CaptureResult realtime_capture(const CaptureRequest& req, const Config& cfg);

} // namespace hkmedia
