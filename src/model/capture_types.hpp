#pragma once
// 截图请求与结果的数据模型，不依赖 HTTP、SDK 或 FFmpeg。

#include <cstdint>
#include <string>
#include <vector>

#include "common/time_util.hpp"

namespace hkmedia {

// POST /api/v1/capture 的请求（已解析、已校验）。
struct CaptureRequest {
    std::string host;
    int port = 8000;
    std::string username;
    std::string password;
    int channel = 1;          // NVR 通道号（IP 通道按设备实际编号，通常从 33 起）
    int stream_type = 0;      // 历史回放码流类型：0 主 1 子 2 三码流
    bool history = false;     // false = 实时截图；true = 历史时间点截图
    NvrTime query_time;       // history=true 时的目标时间（NVR 本地时间）
    int64_t query_ms = 0;     // nvr_time_to_ms(query_time)
    int timeout_ms = 15000;   // 单次截图整体截止时间
    int jpeg_quality = 1;     // 0 最好 / 1 较好 / 2 一般
    int picture_size = 255;   // 实时抓图分辨率档，0xff = 跟随当前码流

    // 日志用的设备标识（不含密码）
    std::string endpoint() const {
        return host + ":" + std::to_string(port) + "/ch" + std::to_string(channel);
    }
};

struct CaptureResult {
    std::vector<uint8_t> jpeg;
    bool history = false;
    NvrTime matched_osd;        // 历史模式：匹配帧的 SDK OSD 时间
    uint32_t frame_no = 0;      // 历史模式：匹配帧的 SDK 帧号
    uint32_t width = 0, height = 0;
    uint64_t received_frames = 0;   // SDK 回调收到的视频帧数
    uint64_t decoded_frames = 0;    // FFmpeg 实际解码输出的帧数
    int64_t elapsed_ms = 0;
};

} // namespace hkmedia
