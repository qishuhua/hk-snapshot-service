#pragma once
// HTTP 请求的输入、处理阶段和输出日志。

#include <string>
#include <nlohmann/json_fwd.hpp>

namespace httplib {
class Server;
struct Request;
}

namespace hkmedia {

void install_request_logging(httplib::Server& server);
void log_http_request_body(const httplib::Request& request);
void log_http_event(const std::string& event, const nlohmann::json& details);

} // 命名空间 hkmedia
