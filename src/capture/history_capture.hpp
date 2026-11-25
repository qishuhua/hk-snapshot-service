#pragma once
// 历史时间点截图：
//   NET_DVR_PlayBackByTime_V40 按时间拉回放流 → SDK ES 回调（裸码流 + 帧级 OSD 元数据）
//   → FFmpeg 软件全速解码 → 按 PTS 把解码输出与 SDK OSD 配对
//   → 解码到目标时间后选择已观察到的最近帧，编码 JPEG 返回。
//
// 回调全速接收并入队，解码线程全速消费，不限制队列容量。

#include <HCNetSDK.h>

#include <atomic>
#include <cstdint>
#include <list>
#include <mutex>
#include <string>

#include "model/capture_types.hpp"
#include "config/config.hpp"
#include "sdk/es_queue.hpp"
#include "sdk/sdk_codec.hpp"
#include "sdk/sdk_env.hpp"

namespace hkmedia {

class HistoryCapture {
public:
    HistoryCapture(LoginSession& session, const CaptureRequest& req, const Config& cfg);

    // 阻塞直到命中目标帧 / 超时 / 失败。线程安全前提：一个 HistoryCapture
    // 实例同一时间只被一个线程 run()。
    CaptureResult run();

private:
    // ---- SDK 交互 ----
    SdkVideoCodec query_codec();
    void open_playback();
    // 周期性回放控制：进度(100/200)、保活心跳、EOF 静默判定、首帧等待超时。
    void control_tick(int64_t now);

    // 运行在 HCNetSDK 内部线程：任何异常不允许逃逸回 SDK 的 C 栈。
    static void CALLBACK es_callback(LONG play_handle, NET_DVR_PACKET_INFO_EX* info,
                                     void* opaque) noexcept;

    // 回调线程报告失败（空包等）；解码线程经 take_failure() 取走。
    void report_failure(const std::string& reason);
    bool take_failure(std::string& reason);

    // ---- 解码侧（仅消费线程访问，无锁） ----
    struct OsdEntry {
        int64_t pts;
        int64_t osd_ms;
        NvrTime osd;
        uint32_t frame_no;
    };
    // 发送时登记 pts→OSD，输出时按 best_effort_timestamp 取走；
    // B 帧重排导致输出顺序 ≠ 回调顺序，不能按到达顺序配对。
    void register_stamp(const EsPacket& packet);
    bool take_stamp(int64_t pts, OsdEntry& out);

    LoginSession& session_;
    const CaptureRequest& req_;
    const Config& cfg_;
    const std::string log_name_;
    const std::string request_id_;

    LONG play_handle_ = -1;
    EsQueue queue_;

    // 回调线程 ↔ 消费线程共享状态（全部原子/互斥）
    std::atomic<bool> done_{false};        // 已命中目标帧或放弃，回调直接返回
    std::atomic<bool> callback_failed_{false};
    std::mutex failure_mu_;
    std::string failure_reason_;
    std::atomic<int64_t> last_rx_ms_{0};
    std::atomic<uint64_t> rx_frames_{0};
    std::atomic<uint32_t> last_frame_rate_code_{0};

    // 仅消费线程访问
    bool eof_seen_ = false;
    DWORD last_progress_ = 0xffffffff;
    int64_t next_keepalive_ms_ = 0;
    uint64_t decoded_frames_ = 0;
    std::list<OsdEntry> pending_stamps_;   // 有界（kMaxPendingStamps）
    static constexpr size_t kMaxPendingStamps = 4096;
};

} // namespace hkmedia
