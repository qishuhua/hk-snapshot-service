#pragma once
// 编码类型选择：严格取自 HCNetSDK 的 byVideoEncType 通道压缩参数元数据，
// 绝不从 H.264/H.265 NAL 字节猜测。

#include <cstdint>

namespace hkmedia {

enum class SdkVideoCodec : uint8_t { Unknown, H264, HEVC };

struct SdkCodecChoice {
    SdkVideoCodec codec;
    bool is_vendor_private;
};

inline SdkCodecChoice sdk_codec_from_encoding(uint32_t by_video_enc_type) {
    switch (by_video_enc_type) {
        case 0:  return {SdkVideoCodec::H264, true};   // 海康私有 264
        case 1:  return {SdkVideoCodec::H264, false};  // 标准 H.264
        case 10: return {SdkVideoCodec::HEVC, false};  // 标准 H.265
        default: return {SdkVideoCodec::Unknown, false};
    }
}

inline const char* sdk_codec_name(SdkVideoCodec codec) {
    switch (codec) {
        case SdkVideoCodec::H264: return "h264";
        case SdkVideoCodec::HEVC: return "hevc";
        default:                  return "unknown";
    }
}

} // namespace hkmedia
