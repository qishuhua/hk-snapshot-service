#pragma once
// 通道编码类型查询（截图使用）。
//
// 优先 NET_DVR_GET_MULTI_STREAM_COMPRESSIONCFG（官方推荐：按码流类型精确查询，
// 支持三码流与扩展分辨率索引），失败回退 NET_DVR_GET_COMPRESSCFG_V30 老接口
// （全设备兼容，但只有主录像/事件录像/网传三组参数）。
// 编码类型只信 SDK 的 byVideoEncType 元数据，绝不从 NAL 字节猜测。

#include <HCNetSDK.h>

#include <stdexcept>
#include <string>

#include "common/app_log.hpp"
#include "common/error.hpp"
#include "sdk/sdk_codec.hpp"

namespace hkmedia {

// 返回值：{codec: 仅 H264/HEVC 或抛错, is_vendor_private: 海康私有 264 标记}
// 失败抛 CaptureError；不支持的非 H.264/H.265 编码抛 501。
struct ChannelCodec {
    SdkVideoCodec codec;
    bool is_vendor_private;
};

// VOD byStreamType（0-主 1-子 2-三码流）→ MULTI_STREAM 查询 dwStreamType
// （0-主 1-子 2-事件 3-码流3）语义错位，不能直接透传。
inline DWORD multi_stream_cond_type(int stream_type) {
    if (stream_type == 1) return 1;
    if (stream_type == 2) return 3;
    return 0;
}

inline ChannelCodec query_channel_codec(LONG user_id, int channel, int stream_type,
                                        const std::string& log_name) {
    NET_DVR_COMPRESSION_INFO_V30 settings{};
    DWORD cond_type = multi_stream_cond_type(stream_type);
    const int64_t started_ms = steady_ms();
    log_info(log_name, "codec.query.start user_id=" + std::to_string(user_id) +
                          " channel=" + std::to_string(channel) +
                          " stream_type=" + std::to_string(stream_type) +
                          " multi_stream_type=" + std::to_string(cond_type));
    bool by_multi_stream = false;
    {
        NET_DVR_MULTI_STREAM_COMPRESSIONCFG_COND cond{};
        cond.dwSize = sizeof(cond);
        cond.struStreamInfo.dwSize = sizeof(cond.struStreamInfo);
        cond.struStreamInfo.dwChannel = static_cast<DWORD>(channel);
        cond.dwStreamType = cond_type;
        NET_DVR_MULTI_STREAM_COMPRESSIONCFG result{};
        result.dwSize = sizeof(result);
        DWORD status = 0xffffffff;
        const bool ok = NET_DVR_GetDeviceConfig(user_id, NET_DVR_GET_MULTI_STREAM_COMPRESSIONCFG, 1,
                                    &cond, sizeof(cond), &status, &result,
                                    sizeof(result));
        const DWORD error = ok ? 0 : NET_DVR_GetLastError();
        if (ok && status <= 1) {
            settings = result.struStreamPara;
            by_multi_stream = true;
        } else {
            log_warn(log_name, "codec.query.fallback method=multi_stream success=" +
                                   std::to_string(ok) + " status=" + std::to_string(status) +
                                   " sdk_error=" + std::to_string(error) + " fallback=V30");
        }
    }
    if (!by_multi_stream) {
        NET_DVR_COMPRESSIONCFG_V30 cfg{};
        cfg.dwSize = sizeof(cfg);
        DWORD returned = 0;
        if (!NET_DVR_GetDVRConfig(user_id, NET_DVR_GET_COMPRESSCFG_V30,
                                  static_cast<LONG>(channel), &cfg, sizeof(cfg),
                                  &returned))
            throw CaptureError(502, "codec_query_failed",
                               "查询通道压缩参数失败 ch" + std::to_string(channel) +
                                   "，SDK 错误码 " +
                                   std::to_string(NET_DVR_GetLastError()));
        settings = stream_type == 1 ? cfg.struNetPara : cfg.struNormHighRecordPara;
        cond_type = 0;
    }
    const uint32_t enc_type = settings.byVideoEncType;
    const auto choice = sdk_codec_from_encoding(enc_type);
    if (choice.codec == SdkVideoCodec::Unknown)
        throw CaptureError(501, "unsupported_codec",
                           "通道 " + std::to_string(channel) + " 编码类型 byVideoEncType=" +
                               std::to_string(enc_type) + " 不支持（仅 H.264/H.265）");
    if (choice.is_vendor_private)
        log_warn(log_name, "byVideoEncType=0 为海康私有 H.264，已按标准 H.264 处理；"
                           "若封装/解码异常可能是私有码流格式不受 FFmpeg 支持");
    // SDK 压缩配置反映设备当前状态，历史录像可能使用其他编码格式。
    log_info(log_name, "codec.query.complete codec=" + std::string(sdk_codec_name(choice.codec)) +
                            " byVideoEncType=" + std::to_string(enc_type) + " source=" +
                            (by_multi_stream
                                 ? "multi_stream(" + std::to_string(cond_type) + ")"
                                 : "V30") + " elapsed_ms=" + std::to_string(steady_ms() - started_ms));
    return {choice.codec, choice.is_vendor_private};
}

} // namespace hkmedia
