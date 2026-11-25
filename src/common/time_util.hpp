#pragma once
// NVR 本地时间的解析、格式化与单调毫秒折算。
//
// 关键约定：查询时间与回放 OSD 时间都是「NVR 设备本地时间」，本服务不做任何
// 时区换算（ OSD 字段本身就是设备侧日历值）。比较两个时间必须折算为同一单调
// 轴，直接用 days_from_civil（Howard Hinnant 算法）把 y/m/d 折为自 1970-01-01
// 的天数，跨日/跨月/闰年均单调可靠。

#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <string>

namespace hkmedia {

struct NvrTime {
    uint32_t year = 0, month = 0, day = 0;
    uint32_t hour = 0, minute = 0, second = 0, millisecond = 0;

    bool valid() const noexcept {
        return year >= 2000 && year <= 2100 && month >= 1 && month <= 12 &&
               day >= 1 && day <= 31 && hour <= 23 && minute <= 59 &&
               second <= 59 && millisecond <= 999;
    }

    // "YYYY-MM-DD HH:MM:SS.mmm"
    std::string text() const {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%04u-%02u-%02u %02u:%02u:%02u.%03u",
                      year, month, day, hour, minute, second, millisecond);
        return buf;
    }

    // "YYYY-MM-DDTHH:MM:SS.mmm"（HTTP 头安全，无空格）
    std::string iso_text() const {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%04u-%02u-%02dT%02u:%02u:%02u.%03u",
                      year, month, day, hour, minute, second, millisecond);
        return buf;
    }
};

inline int64_t days_from_civil(int64_t y, unsigned m, unsigned d) {
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);            // [0, 399]
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;  // [0, 365]
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;           // [0, 146096]
    return era * 146097 + static_cast<int64_t>(doe) - 719468;
}

inline int64_t nvr_time_to_ms(const NvrTime& t) {
    return days_from_civil(t.year, t.month, t.day) * 86400'000LL +
           static_cast<int64_t>(t.hour) * 3600'000LL +
           static_cast<int64_t>(t.minute) * 60'000LL +
           static_cast<int64_t>(t.second) * 1000LL + t.millisecond;
}

// days_from_civil 的逆运算：自 1970-01-01 的天数 → y/m/d。
inline void civil_from_days(int64_t days, uint32_t& y, uint32_t& m, uint32_t& d) {
    const int64_t z = days + 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = static_cast<unsigned>(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const int64_t yy = static_cast<int64_t>(yoe) + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    d = doy - (153 * mp + 2) / 5 + 1;
    m = mp + (mp < 10 ? 3 : -9);
    y = static_cast<uint32_t>(yy + (m <= 2));
}

// 单调毫秒 → 日历字段（毫秒字段折算到天以内）。
inline NvrTime ms_to_nvr_time(int64_t ms) {
    const int64_t days = ms / 86400'000LL;
    const int64_t rest = ms % 86400'000LL;
    NvrTime t;
    civil_from_days(days, t.year, t.month, t.day);
    t.hour = static_cast<uint32_t>(rest / 3600'000);
    t.minute = static_cast<uint32_t>(rest / 60'000 % 60);
    t.second = static_cast<uint32_t>(rest / 1000 % 60);
    t.millisecond = static_cast<uint32_t>(rest % 1000);
    return t;
}

// 解析 NVR 本地时间文本，支持：
//   "YYYY-MM-DD HH:MM:SS" 秒级
//   "YYYY-MM-DDTHH:MM:SS" 支持 T 分隔和 Z 后缀
// 解析失败抛 std::runtime_error（由 HTTP 层映射为 400）。
inline NvrTime parse_nvr_time(const std::string& input) {
    unsigned y = 0, mo = 0, d = 0, h = 0, mi = 0, sec = 0;
    char sep = 0;
    int consumed = 0;
    const int n = std::sscanf(input.c_str(), "%4u-%2u-%2u%c%2u:%2u:%2u%n",
                              &y, &mo, &d, &sep, &h, &mi, &sec, &consumed);
    if (n != 7 || (sep != ' ' && sep != 'T' && sep != 't'))
        throw std::runtime_error("时间格式不正确: '" + input +
                                 "'（应为 YYYY-MM-DD HH:MM:SS）");
    const char* rest = input.c_str() + consumed;
    if (*rest == 'Z' || *rest == 'z') ++rest;
    if (*rest != '\0')
        throw std::runtime_error("时间含有无法识别的尾部字符: '" + input + "'");
    NvrTime t;
    t.year = y; t.month = mo; t.day = d;
    t.hour = h; t.minute = mi; t.second = sec;
    if (!t.valid())
        throw std::runtime_error("时间超出合理范围: '" + input + "'");
    return t;
}

} // namespace hkmedia
