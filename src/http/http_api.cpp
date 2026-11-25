#include "http/http_api.hpp"

#include <httplib.h>
#include <nlohmann/json.hpp>

#include <atomic>
#include <condition_variable>
#include <csignal>
#include <mutex>
#include <signal.h>
#include <string>
#include <vector>
#include <unordered_map>

#include "common/app_log.hpp"
#include "common/error.hpp"
#include "model/capture_types.hpp"
#include "capture/history_capture.hpp"
#include "capture/realtime_capture.hpp"
#include "sdk/sdk_env.hpp"
#include "common/time_util.hpp"
#include "http/request_log.hpp"

namespace hkmedia {

using json = nlohmann::json;

namespace {

// 按设备地址和端口分别限制截图并发，空闲设备不保留计数。
class DeviceGate {
public:
    explicit DeviceGate(int limit) : limit_(limit) {}

    bool try_acquire(const std::string& key, int& available) {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = counts_.find(key);
        if (it != counts_.end() && it->second >= limit_) {
            available = 0;
            return false;
        }
        if (it == counts_.end()) it = counts_.emplace(key, 0).first;
        available = limit_ - ++it->second;
        return true;
    }

    void release(const std::string& key) {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = counts_.find(key);
        if (it != counts_.end() && --it->second == 0) counts_.erase(it);
    }

private:
    int limit_;
    std::mutex mu_;
    std::unordered_map<std::string, int> counts_;
};

json error_body(const std::string& code, const std::string& message) {
    return json{{"error", {{"code", code}, {"message", message}}}};
}

void send_error(httplib::Response& res, int status, const std::string& code,
                const std::string& message) {
    log_http_event("request.error", {{"status", status}, {"code", code}, {"message", message}});
    res.status = status;
    res.set_header("Content-Type", "application/json; charset=utf-8");
    res.body = error_body(code, message).dump();
}

// 读取字符串字段：必填，类型错误/缺失返回 false。
bool require_string(const json& body, const char* key, std::string& out) {
    const auto it = body.find(key);
    if (it == body.end() || it->is_null()) return false;
    if (!it->is_string()) return false;
    out = it->get<std::string>();
    if (out.empty()) return false;
    return true;
}

bool optional_int(const json& body, const char* key, int& out) {
    const auto it = body.find(key);
    if (it == body.end() || it->is_null()) return true;
    if (!it->is_number_integer()) return false;
    out = it->get<int>();
    return true;
}

// 解析并校验请求体。非法输入抛 CaptureError(400)。
CaptureRequest parse_request(const json& body, const Config& cfg) {
    if (!body.is_object())
        throw CaptureError(400, "invalid_param", "请求体必须是 JSON 对象");
    CaptureRequest req;
    if (!require_string(body, "host", req.host))
        throw CaptureError(400, "invalid_param", "缺少必填字段 host（非空字符串）");
    if (!require_string(body, "username", req.username))
        throw CaptureError(400, "invalid_param", "缺少必填字段 username");
    if (!require_string(body, "password", req.password))
        throw CaptureError(400, "invalid_param", "缺少必填字段 password");

    int port = 8000;
    if (!optional_int(body, "port", port) || port <= 0 || port > 65535)
        throw CaptureError(400, "invalid_param", "port 取值非法（1-65535）");
    req.port = port;

    int channel = 1;
    if (!optional_int(body, "channel", channel) || channel < 1 || channel > 1024)
        throw CaptureError(400, "invalid_param", "channel 取值非法（1-1024，IP 通道按设备实际编号）");
    req.channel = channel;

    int stream_type = 0;
    if (!optional_int(body, "stream_type", stream_type) || stream_type < 0 || stream_type > 2)
        throw CaptureError(400, "invalid_param", "stream_type 取值非法（0 主码流 1 子码流 2 三码流）");
    req.stream_type = stream_type;

    int quality = cfg.jpeg_quality;
    if (!optional_int(body, "jpeg_quality", quality) || quality < 0 || quality > 2)
        throw CaptureError(400, "invalid_param", "jpeg_quality 取值非法（0 最好 1 较好 2 一般）");
    req.jpeg_quality = quality;

    int picture_size = cfg.picture_size;
    if (!optional_int(body, "picture_size", picture_size) || picture_size < 0 || picture_size > 255)
        throw CaptureError(400, "invalid_param", "picture_size 取值非法（0-255，255=跟随码流）");
    req.picture_size = picture_size;

    int timeout_ms = cfg.default_timeout_ms;
    if (!optional_int(body, "timeout_ms", timeout_ms) || timeout_ms < 1000 || timeout_ms > 60000)
        throw CaptureError(400, "invalid_param", "timeout_ms 取值非法（1000-60000）");
    req.timeout_ms = timeout_ms;

    // time：缺省/空 = 实时截图；否则为 NVR 本地时间（历史截图）。
    std::string time_text;
    const auto it = body.find("time");
    if (it != body.end() && !it->is_null()) {
        if (!it->is_string() || it->get<std::string>().empty())
            throw CaptureError(400, "invalid_param", "time 必须是非空字符串（YYYY-MM-DD HH:MM:SS）或 null");
        time_text = it->get<std::string>();
    }
    if (!time_text.empty()) {
        try {
            req.query_time = parse_nvr_time(time_text);
        } catch (const std::exception& e) {
            throw CaptureError(400, "invalid_param", std::string("time 非法: ") + e.what());
        }
        req.query_ms = nvr_time_to_ms(req.query_time);
        req.history = true;
    }
    return req;
}

void send_jpeg(httplib::Response& res, const CaptureResult& result) {
    res.status = 200;
    res.set_header("Content-Type", "image/jpeg");
    res.set_header("X-Capture-Mode", result.history ? "history" : "realtime");
    if (result.history) {
        res.set_header("X-Capture-Osd-Time", result.matched_osd.iso_text());
        res.set_header("X-Capture-Frame-No", std::to_string(result.frame_no));
    }
    if (result.width > 0)
        res.set_header("X-Capture-Resolution",
                       std::to_string(result.width) + "x" + std::to_string(result.height));
    res.set_header("X-Capture-Elapsed-Ms", std::to_string(result.elapsed_ms));
    res.body.assign(reinterpret_cast<const char*>(result.jpeg.data()), result.jpeg.size());
    log_http_event("capture.result", {
        {"mode", result.history ? "history" : "realtime"}, {"jpeg_bytes", result.jpeg.size()},
        {"width", result.width}, {"height", result.height},
        {"matched_osd", result.history ? json(result.matched_osd.iso_text()) : json(nullptr)},
        {"frame_no", result.frame_no}, {"received_frames", result.received_frames},
        {"decoded_frames", result.decoded_frames}, {"elapsed_ms", result.elapsed_ms}});
}

std::atomic<httplib::Server*>* global_server() {
    static std::atomic<httplib::Server*> server{nullptr};
    return &server;
}

} // namespace

int run_server(const Config& cfg) {
    httplib::Server svr;
    install_request_logging(svr);
    if (!svr.set_mount_point("/", "./web"))
        log_warn("http", "网页目录不可用：./web");
    DeviceGate gate(cfg.max_concurrent);

    svr.set_payload_max_length(64 * 1024);  // 请求体只含连接参数
    svr.set_read_timeout(15, 0);
    // HTTP 线程池由所有设备共享；设备并发计数相互独立。
    svr.new_task_queue = [&cfg] {
        return new httplib::ThreadPool(cfg.max_concurrent * 2 + 4);
    };

    svr.Get("/healthz", [](const httplib::Request&, httplib::Response& res) {
        res.status = 200;
        res.set_content("ok", "text/plain");
    });

    const auto snapshot_handler = [&cfg, &gate](const httplib::Request& http_req, httplib::Response& res) {
        log_http_request_body(http_req);
        const int64_t started = steady_ms();
        const auto finish_log = [&](const std::string& mode, const std::string& outcome,
                                    int status) {
            log_http_event("request.outcome", {{"mode", mode}, {"outcome", outcome},
                {"status", status}, {"handler_ms", steady_ms() - started}});
        };

        const json body = json::parse(http_req.body, nullptr, /*allow_exceptions=*/false);
        if (body.is_discarded()) {
            send_error(res, 400, "invalid_json", "请求体不是合法 JSON");
            finish_log("-", "bad_json", 400);
            return;
        }

        try {
            const CaptureRequest req = parse_request(body, cfg);
            const std::string device_key = req.host + ":" + std::to_string(req.port);
            int available = 0;
            if (!gate.try_acquire(device_key, available)) {
                log_http_event("request.rejected", {{"reason", "concurrency_limit"},
                    {"device", device_key}, {"max_concurrent", cfg.max_concurrent},
                    {"available_permits", available}});
                send_error(res, 429, "too_many_requests",
                           "设备并发截图已达上限 " + std::to_string(cfg.max_concurrent));
                finish_log(req.history ? "history" : "realtime", "busy", 429);
                return;
            }
            struct GateGuard {
                DeviceGate& gate;
                const std::string& key;
                ~GateGuard() { gate.release(key); }
            } gate_guard{gate, device_key};
            log_http_event("request.admitted", {{"device", device_key},
                {"max_concurrent", cfg.max_concurrent}, {"available_permits", available}});
            log_http_event("capture.params", {
                {"host", req.host}, {"port", req.port}, {"username", req.username},
                {"password", req.password}, {"channel", req.channel}, {"stream_type", req.stream_type},
                {"mode", req.history ? "history" : "realtime"},
                {"time", req.history ? json(req.query_time.iso_text()) : json(nullptr)},
                {"timeout_ms", req.timeout_ms},
                {"jpeg_quality", req.jpeg_quality}, {"picture_size", req.picture_size},
                {"connect_timeout_ms", cfg.connect_timeout_ms},
                {"playback_start_idle_ms", cfg.playback_start_idle_ms}, {"eof_quiet_ms", cfg.eof_quiet_ms}});
            CaptureResult result;
            if (req.history) {
                LoginSession session(req.host, req.port, req.username, req.password,
                                     req.endpoint());
                HistoryCapture capture(session, req, cfg);
                result = capture.run();
            } else {
                result = realtime_capture(req, cfg);
            }
            send_jpeg(res, result);
            finish_log(req.history ? "history" : "realtime", "ok", 200);
        } catch (const CaptureError& e) {
            send_error(res, e.http_status, e.code, e.what());
            finish_log("?", e.code, e.http_status);
        } catch (const std::exception& e) {
            log_error("http", std::string("未预期异常: ") + e.what());
            send_error(res, 500, "internal_error", "服务内部错误");
            finish_log("?", "internal_error", 500);
        }
    };
    svr.Post("/snapshots", snapshot_handler);
    svr.Post("/api/snapshots", snapshot_handler);
    svr.Post("/api/v1/capture", snapshot_handler);

    // SIGINT/SIGTERM → 优雅停止（等待已接连接处理完）。
    struct sigaction sa{};
    sa.sa_handler = [](int) {
        if (httplib::Server* server = global_server()->load()) server->stop();
    };
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);

    log_info("http", "监听 " + cfg.listen_addr + ":" + std::to_string(cfg.listen_port) +
               " max_concurrent_per_device=" + std::to_string(cfg.max_concurrent));
    global_server()->store(&svr);
    const bool listening = svr.listen(cfg.listen_addr, cfg.listen_port);
    global_server()->store(nullptr);

    if (!listening) {
        log_error("http", "HTTP 服务启动/监听失败 " + cfg.listen_addr + ":" +
                     std::to_string(cfg.listen_port));
        return 1;
    }
    log_info("http", "HTTP 服务已停止");
    return 0;
}

} // namespace hkmedia
