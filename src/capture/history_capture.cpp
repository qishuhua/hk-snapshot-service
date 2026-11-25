#include "capture/history_capture.hpp"
#include "capture/nearest_osd.hpp"

#include <algorithm>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

#include "common/app_log.hpp"
#include "common/error.hpp"
#include "sdk/channel_query.hpp"
#include "sdk/es_log.hpp"
#include "media/ff_util.hpp"
#include "media/jpeg_encoder.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/log.h>
#include <libavutil/pixfmt.h>
}

namespace hkmedia {

namespace {

// NET_DVR_PACKET_INFO_EX.dwPacketType：0=文件头 1=I 2=B 3=P 10=音频 11=私有数据。
constexpr DWORD kPacketTypeI = 1, kPacketTypeB = 2, kPacketTypeP = 3;

std::string packet_type_name(uint32_t type) {
    switch (type) {
        case kPacketTypeI: return "I";
        case kPacketTypeB: return "B";
        case kPacketTypeP: return "P";
        default: return "unknown";
    }
}

void check_ffmpeg(int ret, const std::string& what) {
    if (ret < 0) throw std::runtime_error(what + ": " + ff_error(ret));
}

} // namespace

HistoryCapture::HistoryCapture(LoginSession& session, const CaptureRequest& req,
                               const Config& cfg)
    : session_(session),
      req_(req),
      cfg_(cfg),
      log_name_("history/" + req.endpoint()),
      request_id_(current_request_id()) {}

// ---------------------------------------------------------------------------
// SDK 交互
// ---------------------------------------------------------------------------

SdkVideoCodec HistoryCapture::query_codec() {
    // 编码类型只信 SDK 元数据（channel_query.hpp：多码流查询 + V30 回退）。
    return query_channel_codec(session_.user_id(), req_.channel, req_.stream_type,
                               log_name_).codec;
}

void HistoryCapture::open_playback() {
    NET_DVR_VOD_PARA vod{};
    vod.dwSize = sizeof(vod);
    vod.struIDInfo.dwSize = sizeof(vod.struIDInfo);
    vod.struIDInfo.dwChannel = static_cast<DWORD>(req_.channel);
    // 请求和 SDK 回放时间均为秒精度。
    const NvrTime begin_time = ms_to_nvr_time(req_.query_ms);
    const NvrTime end_time = ms_to_nvr_time(req_.query_ms + 3000);
    vod.struBeginTime.dwYear = begin_time.year;
    vod.struBeginTime.dwMonth = begin_time.month;
    vod.struBeginTime.dwDay = begin_time.day;
    vod.struBeginTime.dwHour = begin_time.hour;
    vod.struBeginTime.dwMinute = begin_time.minute;
    vod.struBeginTime.dwSecond = begin_time.second;
    vod.struEndTime.dwYear = end_time.year;
    vod.struEndTime.dwMonth = end_time.month;
    vod.struEndTime.dwDay = end_time.day;
    vod.struEndTime.dwHour = end_time.hour;
    vod.struEndTime.dwMinute = end_time.minute;
    vod.struEndTime.dwSecond = end_time.second;
    vod.byStreamType = static_cast<BYTE>(req_.stream_type);
    vod.byDrawFrame = 0;  // 非抽帧模式：需要全部帧以精确匹配 OSD
    vod.hWnd = 0;         // Linux SDK 下 HWND 为整型，0 = 不渲染

    const int64_t opened_ms = steady_ms();
    log_info(log_name_, "playback.open.start user_id=" + std::to_string(session_.user_id()) +
                          " begin=" + begin_time.text() + " end=" + end_time.text() +
                          " stream_type=" + std::to_string(req_.stream_type) + " draw_frame=0");
    play_handle_ = NET_DVR_PlayBackByTime_V40(session_.user_id(), &vod);
    if (play_handle_ < 0) {
        const DWORD err = NET_DVR_GetLastError();
        // 33 = NET_DVR_NOSPECFILE（该时段没有录像）；7/8/10 为网络类错误。
        const bool network = err == NET_DVR_NETWORK_FAIL_CONNECT ||
                             err == NET_DVR_NETWORK_SEND_ERROR ||
                             err == NET_DVR_NETWORK_RECV_TIMEOUT;
        if (network)
            throw CaptureError(502, "playback_open_failed",
                               "启动回放失败（网络） " + req_.endpoint() +
                                   "，SDK 错误码 " + std::to_string(err));
        throw CaptureError(404, "no_recording",
                           "该时间段没有可回放的录像 " + req_.endpoint() + " @ " +
                               req_.query_time.text() + "，SDK 错误码 " +
                               std::to_string(err));
    }
    log_info(log_name_, "playback.open.complete play_handle=" + std::to_string(play_handle_) +
                          " elapsed_ms=" + std::to_string(steady_ms() - opened_ms));
    if (!NET_DVR_SetPlayBackESCallBack(play_handle_, &HistoryCapture::es_callback, this))
        throw CaptureError(502, "playback_open_failed",
                           "SetPlayBackESCallBack 失败，SDK 错误码 " +
                               std::to_string(NET_DVR_GetLastError()));
    // 注册回调后必须显式 PLAYSTART 设备才开始推流；
    // V40 版本要求携带 4 字节文件偏移，0 = 从头开始。
    DWORD start_offset = 0;
    log_info(log_name_, "playback.start play_handle=" + std::to_string(play_handle_) +
                          " es_callback=registered start_offset=0");
    if (!NET_DVR_PlayBackControl_V40(play_handle_, NET_DVR_PLAYSTART,
                                     &start_offset, sizeof(start_offset),
                                     nullptr, nullptr))
        throw CaptureError(502, "playback_open_failed",
                           "PLAYSTART 失败，SDK 错误码 " +
                               std::to_string(NET_DVR_GetLastError()));
    log_info(log_name_, "playback.started play_handle=" + std::to_string(play_handle_));
}

void HistoryCapture::control_tick(int64_t now) {
    // 进度：按时间回放 0=进行中 100=结束 200=错误。
    DWORD progress = 0;
    DWORD length = sizeof(progress);
    if (NET_DVR_PlayBackControl_V40(play_handle_, NET_DVR_PLAYGETPOS, nullptr, 0,
                                    &progress, &length) &&
        length == sizeof(progress)) {
        if (progress != last_progress_) {
            log_info(log_name_, "playback.progress value=" + std::to_string(progress) +
                                  " received_frames=" + std::to_string(rx_frames_.load()));
            last_progress_ = progress;
        }
        if (progress == 200)
            throw CaptureError(502, "playback_error",
                               "NVR 回放进度上报错误(200) " + req_.endpoint());
        if (progress == 100) eof_seen_ = true;
    }
    if (now >= next_keepalive_ms_) {
        // SDK 建议：回调阻塞期间每 2 秒发送一次心跳保活。
        NET_DVR_PlayBackControl_V40(play_handle_, NET_DVR_KEEPALIVE, nullptr, 0,
                                    nullptr, nullptr);
        next_keepalive_ms_ = now + 2000;
    }
    if (eof_seen_ && now - last_rx_ms_.load(std::memory_order_relaxed) > cfg_.eof_quiet_ms) {
        // 进度 100% 且链路静默：剩余数据已到齐，转入收尾。
        queue_.close();
        return;
    }
    if (rx_frames_.load(std::memory_order_relaxed) == 0 && eof_seen_)
        queue_.close();  // 设备明确无数据可发，立即收尾
}

// ---------------------------------------------------------------------------
// ES 回调（HCNetSDK 线程）
// ---------------------------------------------------------------------------

void HistoryCapture::report_failure(const std::string& reason) {
    if (!callback_failed_.exchange(true)) {
        std::lock_guard<std::mutex> lock(failure_mu_);
        failure_reason_ = reason;
    }
    queue_.close();  // 唤醒消费线程尽快收尾
    log_error(log_name_, reason);
}

bool HistoryCapture::take_failure(std::string& reason) {
    if (!callback_failed_.load()) return false;
    std::lock_guard<std::mutex> lock(failure_mu_);
    reason = failure_reason_;
    return true;
}

void CALLBACK HistoryCapture::es_callback(LONG play_handle, NET_DVR_PACKET_INFO_EX* info,
                                          void* opaque) noexcept {
    auto* self = static_cast<HistoryCapture*>(opaque);
    if (!self || !info || self->done_.load(std::memory_order_relaxed)) return;
    RequestLogScope request_scope(self->request_id_);
    const DWORD type = info->dwPacketType;
    if (type != kPacketTypeI && type != kPacketTypeB && type != kPacketTypeP) return;
    try {
        if (!info->pPacketBuffer || info->dwPacketSize == 0) {
            self->report_failure("SDK 返回了空的 ES 视频帧");
            return;
        }
        EsPacket packet;
        packet.bytes.assign(info->pPacketBuffer,
                            info->pPacketBuffer + info->dwPacketSize);
        packet.packet_type = type;
        packet.packet_mode = info->dwPacketMode;
        packet.width = info->wWidth;
        packet.height = info->wHeight;
        packet.pts = (static_cast<uint64_t>(info->dwTimeStampHigh) << 32) |
                     static_cast<uint64_t>(info->dwTimeStamp);
        packet.osd.year = info->dwYear;
        packet.osd.month = info->dwMonth;
        packet.osd.day = info->dwDay;
        packet.osd.hour = info->dwHour;
        packet.osd.minute = info->dwMinute;
        packet.osd.second = info->dwSecond;
        packet.osd.millisecond = info->dwMillisecond;
        packet.osd_ms = nvr_time_to_ms(packet.osd);
        packet.frame_no = info->dwFrameNum;
        log_es_packet(self->log_name_, play_handle, packet, self->rx_frames_.load() == 0);
        const PushStatus pushed = self->queue_.try_push(std::move(packet));
        if (pushed == PushStatus::Closed) return;  // 消费端已收尾
        self->rx_frames_.fetch_add(1, std::memory_order_relaxed);
        self->last_frame_rate_code_.store(info->dwFrameRate, std::memory_order_relaxed);
        self->last_rx_ms_.store(steady_ms(), std::memory_order_relaxed);
    } catch (const std::exception& e) {
        try {
            self->report_failure(std::string("ES 回调异常: ") + e.what());
        } catch (...) {
        }
    } catch (...) {
        try {
            self->report_failure("ES 回调异常: 未知");
        } catch (...) {
        }
    }
}

// ---------------------------------------------------------------------------
// 解码侧
// ---------------------------------------------------------------------------

void HistoryCapture::register_stamp(const EsPacket& packet) {
    pending_stamps_.push_back(
        {static_cast<int64_t>(packet.pts), packet.osd_ms, packet.osd, packet.frame_no});
    while (pending_stamps_.size() > kMaxPendingStamps) pending_stamps_.pop_front();
}

bool HistoryCapture::take_stamp(int64_t pts, OsdEntry& out) {
    // B 帧重排窗口有限，正常命中都在队首附近；限制扫描范围防止病态 O(n²)。
    size_t scanned = 0;
    for (auto it = pending_stamps_.begin(); it != pending_stamps_.end() && scanned < 256;
         ++it, ++scanned) {
        if (it->pts == pts) {
            out = *it;
            pending_stamps_.erase(it);
            return true;
        }
    }
    return false;
}

CaptureResult HistoryCapture::run() {
    const int64_t started_ms = steady_ms();
    const int64_t deadline_ms = started_ms + req_.timeout_ms;
    next_keepalive_ms_ = started_ms + 2000;
    last_rx_ms_.store(started_ms, std::memory_order_relaxed);
    JpegEncoder jpeg;

    // 1) 编码类型：只信 SDK 元数据，绝不从 NAL 猜测。
    const SdkVideoCodec codec = query_codec();

    // 2) 启动按时间回放。RAII 保证异常/命中路径都会先停回放再注销登录。
    struct PlaybackGuard {
        HistoryCapture& self;
        void stop() noexcept {
            self.done_.store(true, std::memory_order_relaxed);
            if (self.play_handle_ < 0) return;
            const LONG handle = self.play_handle_;
            self.play_handle_ = -1;
            const bool ok = NET_DVR_StopPlayBack(handle);
            const DWORD error = ok ? 0 : NET_DVR_GetLastError();
            try {
                const std::string message = "playback.stop play_handle=" + std::to_string(handle) +
                    " success=" + std::to_string(ok) + " sdk_error=" + std::to_string(error);
                if (ok) log_info(self.log_name_, message);
                else log_warn(self.log_name_, message);
            } catch (...) {
            }
        }
        ~PlaybackGuard() { stop(); }
    } playback_guard{*this};
    open_playback();

    // 3) FFmpeg 软件解码器：线程数自动（全速），pkt_timebase 未定标（仅做
    //    packet→frame 的 PTS 配对，不用于时间换算）。
    const AVCodecID codec_id =
        codec == SdkVideoCodec::HEVC ? AV_CODEC_ID_HEVC : AV_CODEC_ID_H264;
    const AVCodec* decoder_codec = avcodec_find_decoder(codec_id);
    if (!decoder_codec)
        throw CaptureError(501, "unsupported_codec",
                           std::string("FFmpeg 无 ") + sdk_codec_name(codec) + " 解码器");
    struct CodecContextDestroy {
        void operator()(AVCodecContext* ctx) const noexcept { avcodec_free_context(&ctx); }
    };
    std::unique_ptr<AVCodecContext, CodecContextDestroy> decoder(
        avcodec_alloc_context3(decoder_codec));
    if (!decoder) throw std::bad_alloc();
    decoder->pkt_timebase = AVRational{1, 1};
    decoder->thread_count = 0;  // 0 = 按 CPU 自动，全速解码
    check_ffmpeg(avcodec_open2(decoder.get(), decoder_codec, nullptr),
                 "avcodec_open2(" + std::string(sdk_codec_name(codec)) + ")");
    log_info(log_name_, "decoder.ready codec=" + std::string(sdk_codec_name(codec)) +
                          " thread_count=" + std::to_string(decoder->thread_count));

    struct FrameDestroy {
        void operator()(AVFrame* frame) const noexcept { av_frame_free(&frame); }
    };
    struct PacketDestroy {
        void operator()(AVPacket* packet) const noexcept { av_packet_free(&packet); }
    };
    std::unique_ptr<AVFrame, FrameDestroy> frame(av_frame_alloc());
    std::unique_ptr<AVFrame, FrameDestroy> best_frame(av_frame_alloc());
    std::unique_ptr<AVPacket, PacketDestroy> packet(av_packet_alloc());
    if (!frame || !best_frame || !packet) throw std::bad_alloc();

    CaptureResult result;
    result.history = true;
    bool matched = false;
    NearestOsd nearest(req_.query_ms);
    uint64_t unmatched_frames = 0;   // 输出帧未能配对到 SDK OSD 的数量（诊断用）

    // 输出帧 → OSD 配对 + 命中判定。
    const auto consume_frame = [&](const AVFrame& f) -> bool {
        OsdEntry stamp;
        const bool stamped = take_stamp(f.best_effort_timestamp, stamp);
        if (!stamped) {
            if (++unmatched_frames == 64)
                log_warn(log_name_,
                         "已有 64 个解码输出帧无法配对 SDK OSD（PTS 索引失配），"
                         "目标帧判定可能退化为超时/404");
            return false;  // 未配对的输出帧无法判时，跳过
        }
        if (nearest.consider(stamp.osd_ms)) {
            av_frame_unref(best_frame.get());
            check_ffmpeg(av_frame_ref(best_frame.get(), &f), "保存最近帧");
            result.matched_osd = stamp.osd;
            result.frame_no = stamp.frame_no;
            result.width = static_cast<uint32_t>(f.width);
            result.height = static_cast<uint32_t>(f.height);
        }
        return nearest.ready(stamp.osd_ms);  // 观察到目标时间或之后的帧后立即收尾。
    };

    // send + 登记配对信息 + 排空输出。返回是否命中。
    const auto send_packet = [&](const EsPacket& in) -> bool {
        check_ffmpeg(av_new_packet(packet.get(), static_cast<int>(in.bytes.size())),
                     "av_new_packet");
        std::memcpy(packet->data, in.bytes.data(), in.bytes.size());
        packet->pts = in.pts <= static_cast<uint64_t>(INT64_MAX)
                          ? static_cast<int64_t>(in.pts) : AV_NOPTS_VALUE;
        packet->dts = AV_NOPTS_VALUE;
        if (in.packet_type == kPacketTypeI) packet->flags |= AV_PKT_FLAG_KEY;
        int ret = avcodec_send_packet(decoder.get(), packet.get());
        if (ret == AVERROR(EAGAIN)) {
            for (;;) {
                const int r = avcodec_receive_frame(decoder.get(), frame.get());
                if (r == AVERROR(EAGAIN) || r == AVERROR_EOF) break;
                check_ffmpeg(r, "avcodec_receive_frame");
                ++decoded_frames_;
                if (consume_frame(*frame)) { av_frame_unref(frame.get()); return true; }
                av_frame_unref(frame.get());
            }
            ret = avcodec_send_packet(decoder.get(), packet.get());
        }
        av_packet_unref(packet.get());
        if (ret < 0)
            throw CaptureError(502, "decode_failed",
                               std::string("解码失败(") + sdk_codec_name(codec) + "): " +
                                   ff_error(ret) + " frame_no=" +
                                   std::to_string(in.frame_no) + " type=" +
                                   packet_type_name(in.packet_type) +
                                   "（历史录像编码可能与设备当前配置不一致）");
        register_stamp(in);
        for (;;) {
            const int r = avcodec_receive_frame(decoder.get(), frame.get());
            if (r == AVERROR(EAGAIN) || r == AVERROR_EOF) break;
            check_ffmpeg(r, "avcodec_receive_frame");
            ++decoded_frames_;
            if (consume_frame(*frame)) { av_frame_unref(frame.get()); return true; }
            av_frame_unref(frame.get());
        }
        return false;
    };

    // 冲刷解码器（流结束/队列关闭后的残帧，B 帧重排缓冲里可能还有目标帧）。
    const auto flush_decoder = [&]() -> bool {
        int ret = avcodec_send_packet(decoder.get(), nullptr);
        if (ret == AVERROR(EAGAIN)) {
            // 输出未排空：先排空再进入 EOF 冲刷。
            for (;;) {
                const int r = avcodec_receive_frame(decoder.get(), frame.get());
                if (r == AVERROR(EAGAIN) || r == AVERROR_EOF) break;
                check_ffmpeg(r, "avcodec_receive_frame");
                ++decoded_frames_;
                if (consume_frame(*frame)) { av_frame_unref(frame.get()); return true; }
                av_frame_unref(frame.get());
            }
            ret = avcodec_send_packet(decoder.get(), nullptr);
        }
        if (ret < 0 && ret != AVERROR_EOF) return false;
        for (;;) {
            const int r = avcodec_receive_frame(decoder.get(), frame.get());
            if (r == AVERROR(EAGAIN) || r == AVERROR_EOF) break;
            if (r < 0) break;
            ++decoded_frames_;
            if (consume_frame(*frame)) { av_frame_unref(frame.get()); return true; }
            av_frame_unref(frame.get());
        }
        return false;
    };

    EsPacket in;
    while (true) {
        const int64_t now = steady_ms();
        if (now >= deadline_ms)
            throw CaptureError(504, "capture_timeout",
                               "截图超时（未在 " + std::to_string(req_.timeout_ms) +
                                   "ms 内命中目标帧） " + req_.endpoint() + " @ " +
                                   req_.query_time.text());
        std::string failure;
        if (take_failure(failure))
            throw CaptureError(500, "internal_error", failure);

        const int64_t wait_until = std::min<int64_t>(deadline_ms, now + 200);
        const PopStatus popped = queue_.pop_wait(in, wait_until);
        if (popped == PopStatus::Timeout) {
            control_tick(steady_ms());
            // 首帧等待兜底：回放已启动但迟迟没有任何数据。
            if (rx_frames_.load(std::memory_order_relaxed) == 0 &&
                steady_ms() - started_ms > cfg_.playback_start_idle_ms)
                throw CaptureError(504, "playback_no_data",
                                   "回放启动后未收到任何视频数据 " + req_.endpoint() +
                                       "（该时段可能无录像）");
            continue;
        }
        if (popped == PopStatus::Closed) {
            // 队列排空后冲刷重排残帧，再确定窗口内最近的帧。
            flush_decoder();
            if (nearest.found()) { matched = true; break; }
            throw CaptureError(404, "frame_not_found",
                               "回放流已结束，查询前 5 秒至后 3 秒内没有可匹配的帧 " +
                                   req_.endpoint() + " @ " + req_.query_time.text() +
                                   "（received=" +
                                   std::to_string(rx_frames_.load()) + "）");
        }

        if (in.packet_mode == 1)
            throw CaptureError(502, "unsupported_stream",
                               "SDK ES 包为 FU-A 分片模式（packet_mode=1），"
                               "需要重组后才能送入 libavcodec，当前不支持");
        if (in.bytes.size() > static_cast<size_t>(INT32_MAX))
            throw CaptureError(502, "unsupported_stream", "ES 包过大");

        if (send_packet(in)) {
            done_.store(true, std::memory_order_relaxed);
            matched = true;
            break;
        }
        in = EsPacket{};
    }

    if (!matched)  // 逻辑上不可达（所有退出路径要么命中要么抛异常），防御保留
        throw CaptureError(500, "internal_error", "历史截图未命中即退出");

    // 命中后先停止设备回放，再进行本地 JPEG 编码。
    playback_guard.stop();
    result.jpeg = jpeg.encode(best_frame.get(), req_.jpeg_quality);
    log_info(log_name_, "nearest_frame distance_ms=" + std::to_string(nearest.distance_ms()));
    result.received_frames = rx_frames_.load();
    result.decoded_frames = decoded_frames_;
    result.elapsed_ms = steady_ms() - started_ms;
    log_info(log_name_, "历史截图完成 " + req_.endpoint() + " @ " +
                 req_.query_time.text() + " → OSD " + result.matched_osd.text() +
                 " frame=" + std::to_string(result.frame_no) +
                 " " + std::to_string(result.width) + "x" + std::to_string(result.height) +
                 " rx=" + std::to_string(result.received_frames) +
                 " decoded=" + std::to_string(result.decoded_frames) +
                 " jpeg=" + std::to_string(result.jpeg.size()) + "B" +
                 " elapsed=" + std::to_string(result.elapsed_ms) + "ms");
    return result;
}

} // namespace hkmedia
