#pragma once
// FFmpeg 小工具：错误字符串化。

#include <string>

extern "C" {
#include <libavutil/error.h>
}

namespace hkmedia {

inline std::string ff_error(int err) {
    char msg[AV_ERROR_MAX_STRING_SIZE]{};
    av_strerror(err, msg, sizeof(msg));
    return msg;
}

} // namespace hkmedia
