#include "media/jpeg_encoder.hpp"

extern "C" {
#include <libavutil/imgutils.h>
}

#include <algorithm>
#include <memory>
#include <stdexcept>
#include <string>

#include "media/ff_util.hpp"

namespace hkmedia {
namespace {

struct FrameDestroy {
    void operator()(AVFrame* frame) const noexcept { av_frame_free(&frame); }
};
struct PacketDestroy {
    void operator()(AVPacket* packet) const noexcept { av_packet_free(&packet); }
};

void check_ffmpeg(int result, const char* step) {
    if (result < 0)
        throw std::runtime_error(std::string(step) + ": " + ff_error(result));
}

// FFmpeg 保留了 YUVJ 系列兼容格式但标记为废弃；全量程行为改在 SwsContext
// 中配置。编码边界保留旧格式（部分 MJPEG 编码器只认 YUVJ420P）。
AVPixelFormat swscale_compatible_format(AVPixelFormat format) {
    switch (format) {
        case AV_PIX_FMT_YUVJ420P: return AV_PIX_FMT_YUV420P;
        case AV_PIX_FMT_YUVJ422P: return AV_PIX_FMT_YUV422P;
        case AV_PIX_FMT_YUVJ444P: return AV_PIX_FMT_YUV444P;
        case AV_PIX_FMT_YUVJ440P: return AV_PIX_FMT_YUV440P;
        default: return format;
    }
}

uint8_t expand_limited_sample(uint8_t value, int minimum, int span) {
    const int expanded = ((static_cast<int>(value) - minimum) * 255 + span / 2) / span;
    return static_cast<uint8_t>(std::clamp(expanded, 0, 255));
}

void expand_limited_yuv_to_full_range(AVFrame* frame, AVPixelFormat format) {
    int chroma_width = 0, chroma_height = 0;
    if (format == AV_PIX_FMT_YUV420P) {
        chroma_width = (frame->width + 1) / 2;
        chroma_height = (frame->height + 1) / 2;
    } else if (format == AV_PIX_FMT_YUV444P) {
        chroma_width = frame->width;
        chroma_height = frame->height;
    } else {
        throw std::runtime_error("无法展开该 JPEG 像素格式的色彩范围");
    }
    for (int y = 0; y < frame->height; ++y) {
        auto* row = frame->data[0] + y * frame->linesize[0];
        for (int x = 0; x < frame->width; ++x)
            row[x] = expand_limited_sample(row[x], 16, 219);
    }
    for (int plane = 1; plane <= 2; ++plane) {
        for (int y = 0; y < chroma_height; ++y) {
            auto* row = frame->data[plane] + y * frame->linesize[plane];
            for (int x = 0; x < chroma_width; ++x)
                row[x] = expand_limited_sample(row[x], 16, 224);
        }
    }
}

int qscale_for_quality(int quality) {
    switch (quality) {
        case 0: return 2;
        case 2: return 5;
        default: return 3;
    }
}

} // namespace

JpegEncoder::~JpegEncoder() {
    sws_freeContext(sws_);
    avcodec_free_context(&encoder_);
}

void JpegEncoder::ensure_encoder(int width, int height, int quality) {
    if (encoder_ && width_ == width && height_ == height && quality_ == quality) return;
    if (width <= 0 || height <= 0)
        throw std::runtime_error("解码帧尺寸无效，无法编码 JPEG");
    sws_freeContext(sws_);
    sws_ = nullptr;
    avcodec_free_context(&encoder_);

    const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_MJPEG);
    if (!codec) throw std::runtime_error("FFmpeg 无可用的 MJPEG 编码器");
    encoder_ = avcodec_alloc_context3(codec);
    if (!encoder_) throw std::bad_alloc();
    encoder_->width = width;
    encoder_->height = height;
    encoder_->time_base = AVRational{1, 25};
    // 部分 MJPEG 编码器拒绝 YUV420P；在编码边界保留旧全量程格式，
    // swscale 侧使用其未废弃别名并把目标范围显式设为 full。
    encoder_->pix_fmt = (width % 2 == 0 && height % 2 == 0) ? AV_PIX_FMT_YUVJ420P
                                                            : AV_PIX_FMT_YUVJ444P;
    encoder_->color_range = AVCOL_RANGE_JPEG;
    encoder_->flags |= AV_CODEC_FLAG_QSCALE;
    encoder_->global_quality = FF_QP2LAMBDA * qscale_for_quality(quality);
    check_ffmpeg(avcodec_open2(encoder_, codec, nullptr), "打开 MJPEG 编码器");
    width_ = width;
    height_ = height;
    quality_ = quality;
}

std::vector<uint8_t> JpegEncoder::encode(const AVFrame* source, int quality) {
    if (!source) throw std::runtime_error("待编码帧为空");
    // 本服务为纯软解：硬件帧（NVDEC 等）带 hw_frames_ctx，直接拒绝。
    if (source->format == AV_PIX_FMT_CUDA || source->hw_frames_ctx)
        throw std::runtime_error("JpegEncoder 只接受软件帧（本服务为纯软解）");
    if (source->width <= 0 || source->height <= 0)
        throw std::runtime_error("解码帧尺寸无效，无法编码 JPEG");
    ensure_encoder(source->width, source->height, quality);

    std::unique_ptr<AVFrame, FrameDestroy> yuv(av_frame_alloc());
    std::unique_ptr<AVPacket, PacketDestroy> packet(av_packet_alloc());
    if (!yuv || !packet) throw std::bad_alloc();

    const auto encoder_pixel_format = encoder_->pix_fmt;
    const auto sws_output_format = swscale_compatible_format(encoder_pixel_format);
    yuv->format = sws_output_format;
    yuv->width = source->width;
    yuv->height = source->height;
    yuv->color_range = AVCOL_RANGE_JPEG;
    yuv->quality = encoder_->global_quality;
    check_ffmpeg(av_frame_get_buffer(yuv.get(), 32), "分配 JPEG 图像缓冲");
    check_ffmpeg(av_frame_make_writable(yuv.get()), "JPEG 图像缓冲可写");

    const auto source_format = static_cast<AVPixelFormat>(source->format);
    const auto sws_source_format = swscale_compatible_format(source_format);
    const bool source_is_full_range = source->color_range == AVCOL_RANGE_JPEG ||
        (source->color_range == AVCOL_RANGE_UNSPECIFIED &&
         source_format != sws_source_format);
    if (sws_source_format == sws_output_format) {
        // YUVJ 与其非 J 别名平面布局一致，直接拷贝；
        // 源不是全量程时把 limited 样本展开为 full。
        const uint8_t* source_data[4] = {
            source->data[0], source->data[1], source->data[2], source->data[3]
        };
        av_image_copy(yuv->data, yuv->linesize, source_data, source->linesize,
                      sws_output_format, source->width, source->height);
        if (!source_is_full_range)
            expand_limited_yuv_to_full_range(yuv.get(), sws_output_format);
    } else {
        sws_ = sws_getCachedContext(
            sws_, source->width, source->height, source_format,
            source->width, source->height, sws_output_format,
            SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (!sws_) throw std::runtime_error("sws_getCachedContext: 不支持的源像素格式");
        const int* coefficients = sws_getCoefficients(SWS_CS_DEFAULT);
        const int color_range_result = sws_setColorspaceDetails(
            sws_, coefficients, source_is_full_range ? 1 : 0,
            coefficients, 1, 0, 1 << 16, 1 << 16);
        const int scaled = sws_scale(sws_, source->data, source->linesize, 0,
                                     source->height, yuv->data, yuv->linesize);
        if (scaled != source->height)
            throw std::runtime_error("sws_scale 输出不完整");
        if (color_range_result < 0)
            expand_limited_yuv_to_full_range(yuv.get(), sws_output_format);
    }

    // swscale 已把全量程 YUV 写入非废弃别名格式；提交时改回编码器声明的旧格式。
    yuv->format = encoder_pixel_format;
    check_ffmpeg(avcodec_send_frame(encoder_, yuv.get()), "提交 JPEG 图像");
    check_ffmpeg(avcodec_receive_packet(encoder_, packet.get()), "编码 JPEG 图像");

    return std::vector<uint8_t>(packet->data, packet->data + packet->size);
}

} // namespace hkmedia
