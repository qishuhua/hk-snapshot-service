#pragma once
// HTTP API 层：cpp-httplib 同步服务。

#include "config/config.hpp"

namespace hkmedia {

// 阻塞运行 HTTP 服务（SIGINT/SIGTERM 时优雅停止），返回进程退出码。
int run_server(const Config& cfg);

} // namespace hkmedia
