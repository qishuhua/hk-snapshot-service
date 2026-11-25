#pragma once
// AVFrame（软件帧）→ 内存 JPEG：处理 limited→full 色彩范围展开与
// YUVJ 兼容格式，配置 MJPEG 编码器参数，输出完整 JPEG 文件字节。

#include <cstdint>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
#include <libswscale/swscale.h>
}

namespace hkmedia {

class JpegEncoder {
public:
    // quality 档位沿用海康语义：0 最好 / 1 较好 / 2 一般，
    // 映射到 MJPEG 的 qscale 2 / 3 / 5（默认档 q=3，观感与体积平衡）。
    // 返回完整 JPEG 文件字节。线程不安全：每个解码线程独立持有。
    std::vector<uint8_t> encode(const AVFrame* frame, int quality);

    ~JpegEncoder();

    JpegEncoder() = default;
    JpegEncoder(const JpegEncoder&) = delete;
    JpegEncoder& operator=(const JpegEncoder&) = delete;

private:
    void ensure_encoder(int width, int height, int quality);

    AVCodecContext* encoder_ = nullptr;
    SwsContext* sws_ = nullptr;
    int width_ = 0, height_ = 0, quality_ = -1;
};

} // namespace hkmedia
