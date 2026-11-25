#include "http/request_log.hpp"

#include <httplib.h>
#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>

#include "common/app_log.hpp"

namespace hkmedia {
namespace {

using json = nlohmann::json;
std::atomic<uint64_t> request_sequence{0};
thread_local int64_t request_started_ms = 0;
thread_local bool request_body_logged = false;

std::string dump_log_json(const json& value) {
    // 非法 UTF-8 用替代字符保留日志，JSON 转义保证原始请求不会破坏单行格式。
    return value.dump(-1, ' ', false, json::error_handler_t::replace);
}

json log_headers(const httplib::Headers& headers) {
    json values = json::array();
    for (const auto& header : headers)
        values.push_back({{"name", header.first}, {"value", header.second}});
    return values;
}

void begin_request(const httplib::Request& request, httplib::Response& response) {
    if (response.has_header("X-Request-Id")) {
        set_request_context(response.get_header_value("X-Request-Id"));
        return;
    }
    const auto now_us = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    const std::string request_id = std::to_string(now_us) + "-" +
        std::to_string(request_sequence.fetch_add(1, std::memory_order_relaxed));
    set_request_context(request_id);
    request_started_ms = steady_ms();
    request_body_logged = false;
    response.set_header("X-Request-Id", request_id);
    log_http_event("request.start", {
        {"method", request.method}, {"path", request.path}, {"target", request.target},
        {"version", request.version}, {"remote_addr", request.remote_addr},
        {"remote_port", request.remote_port}, {"headers", log_headers(request.headers)},
        {"declared_body_bytes", request.get_header_value("Content-Length")}});
}

} // 匿名命名空间

void log_http_event(const std::string& event, const nlohmann::json& details) {
    log_info("http", event + " " + dump_log_json(details));
}

void log_http_request_body(const httplib::Request& request) {
    if (request_body_logged) return;
    log_http_event("request.body", {
        {"body_bytes", request.body.size()}, {"body", request.body},
        {"declared_body_bytes", request.get_header_value("Content-Length")}});
    request_body_logged = true;
}

void install_request_logging(httplib::Server& server) {
    server.set_pre_routing_handler([](const httplib::Request& request, httplib::Response& response) {
        // 此时框架尚未读完 POST 请求体，原始内容由业务入口或响应钩子补记。
        begin_request(request, response);
        return httplib::Server::HandlerResponse::Unhandled;
    });
    server.set_post_routing_handler([](const httplib::Request& request, httplib::Response& response) {
        // 同时覆盖 healthz、404、请求体过大等未进入业务处理函数的响应。
        begin_request(request, response);
        log_http_request_body(request);
        const auto content_type = response.get_header_value("Content-Type");
        json output = {
            {"status", response.status}, {"headers", log_headers(response.headers)},
            {"content_type", content_type}, {"body_bytes", response.body.size()},
            {"processing_ms", steady_ms() - request_started_ms}};
        if (content_type.find("application/json") != std::string::npos ||
            content_type.find("text/") == 0 || content_type.empty())
            output["body"] = response.body;
        log_http_event("response.ready", output);
    });
    server.set_logger([](const httplib::Request& request, const httplib::Response& response) {
        const std::string message = "request.complete " + dump_log_json({
            {"method", request.method}, {"target", request.target}, {"status", response.status},
            {"body_bytes", response.body.size()}, {"elapsed_ms", steady_ms() - request_started_ms}});
        if (response.status >= 500) log_error("http", message);
        else if (response.status >= 400) log_warn("http", message);
        else log_info("http", message);
        clear_request_context();
        request_started_ms = 0;
        request_body_logged = false;
    });
}

} // 命名空间 hkmedia
