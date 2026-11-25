#pragma once
// ES 包队列：HCNetSDK 回调线程（生产者）→ 解码线程（消费者）。
//
// 设计约束：
//  - 生产者不等待消费：队列不限制容量，关闭后拒绝新数据。
//  - 消费者按截止时间等待：pop_wait 在超时/关闭时返回，让主循环有机会执行
//    回放控制（进度查询、心跳、整体超时判断）。

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <utility>
#include <vector>

#include "common/app_log.hpp"
#include "common/time_util.hpp"

namespace hkmedia {

struct EsPacket {
    std::vector<uint8_t> bytes;  // 裸 ES 帧负载
    uint64_t pts = 0;            // (dwTimeStampHigh << 32) | dwTimeStamp，未定标
    int64_t osd_ms = 0;          // nvr_time_to_ms(OSD 日历字段)
    NvrTime osd;                 // 原始 OSD 日历字段
    uint32_t frame_no = 0;
    uint32_t packet_type = 0;    // 1=I 2=B 3=P
    uint32_t packet_mode = 0;    // 0=裸 ES 1=FU-A 分片（本服务不支持，报错）
    uint32_t width = 0, height = 0;
};

enum class PushStatus { Ok, Closed };
enum class PopStatus { Ok, Timeout, Closed };

class EsQueue {
public:
    PushStatus try_push(EsPacket&& packet) {
        {
            std::lock_guard<std::mutex> lock(mu_);
            if (closed_) return PushStatus::Closed;
            packets_.push_back(std::move(packet));
        }
        changed_.notify_all();
        return PushStatus::Ok;
    }

    // 等待到 deadline（steady 毫秒）。返回 Timeout 表示本次等待没等到数据，
    // Closed 表示队列已关闭且缓冲已排空。
    PopStatus pop_wait(EsPacket& out, int64_t deadline_ms) {
        std::unique_lock<std::mutex> lock(mu_);
        const bool signaled = changed_.wait_until(
            lock, std::chrono::steady_clock::time_point(
                      std::chrono::milliseconds(deadline_ms)),
            [&] { return closed_ || !packets_.empty(); });
        if (packets_.empty()) return signaled ? PopStatus::Closed : PopStatus::Timeout;
        out = std::move(packets_.front());
        packets_.pop_front();
        return PopStatus::Ok;
    }

    void close() {
        {
            std::lock_guard<std::mutex> lock(mu_);
            closed_ = true;
        }
        changed_.notify_all();
    }

private:
    mutable std::mutex mu_;
    std::condition_variable changed_;
    std::deque<EsPacket> packets_;
    bool closed_ = false;
};

} // namespace hkmedia
