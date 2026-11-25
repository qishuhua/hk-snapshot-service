#pragma once

#include <cstdint>

namespace hkmedia {

// 只比较查询前 5 秒、后 3 秒内的帧；等距时选择时间较早的帧。
class NearestOsd {
public:
    explicit NearestOsd(int64_t query_ms) : query_ms_(query_ms) {}

    bool consider(int64_t osd_ms) {
        if (osd_ms < query_ms_ - 5000 || osd_ms > query_ms_ + 3000) return false;
        const int64_t distance = osd_ms >= query_ms_ ? osd_ms - query_ms_ : query_ms_ - osd_ms;
        if (found_ && (distance > distance_ || (distance == distance_ && osd_ms >= osd_ms_)))
            return false;
        found_ = true;
        distance_ = distance;
        osd_ms_ = osd_ms;
        return true;
    }

    // 到达目标时间后，返回已观察到的最近帧，不再等待整个回放窗口。
    bool ready(int64_t osd_ms) const noexcept { return found_ && osd_ms >= query_ms_; }
    bool found() const noexcept { return found_; }
    int64_t distance_ms() const noexcept { return distance_; }

private:
    int64_t query_ms_;
    int64_t osd_ms_ = 0;
    int64_t distance_ = 0;
    bool found_ = false;
};

} // 命名空间 hkmedia
