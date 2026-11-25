#pragma once
// 各模块共用的服务错误，统一携带 HTTP 状态和机器可读错误码。

#include <stdexcept>
#include <string>
#include <utility>

namespace hkmedia {

class CaptureError : public std::runtime_error {
public:
    CaptureError(int http_status, std::string code, const std::string& message)
        : std::runtime_error(message), http_status(http_status), code(std::move(code)) {}
    const int http_status;
    const std::string code;
};

} // 命名空间 hkmedia
