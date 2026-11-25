#pragma once
// 请求上下文在 HTTP 工作线程中保存，并可临时传递给 SDK 回调线程。

#include <string>
#include <utility>

namespace hkmedia {

inline thread_local std::string thread_request_id;
inline thread_local const std::string* active_request_id = nullptr;

inline const std::string& current_request_id() noexcept {
    static const std::string empty_id = "-";
    return active_request_id ? *active_request_id : empty_id;
}

inline void set_request_context(std::string request_id) {
    thread_request_id = std::move(request_id);
    active_request_id = &thread_request_id;
}

inline void clear_request_context() noexcept {
    active_request_id = nullptr;
    thread_request_id.clear();
}

class RequestLogScope {
public:
    explicit RequestLogScope(const std::string& request_id) noexcept
        : previous_(active_request_id) {
        active_request_id = &request_id;
    }
    ~RequestLogScope() { active_request_id = previous_; }
    RequestLogScope(const RequestLogScope&) = delete;
    RequestLogScope& operator=(const RequestLogScope&) = delete;

private:
    const std::string* previous_;
};

} // 命名空间 hkmedia
