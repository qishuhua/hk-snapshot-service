#include "common/request_context.hpp"

#include <condition_variable>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace {

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

} // 匿名命名空间

int main() {
    using namespace hkmedia;
    require(current_request_id() == "-", "请求外上下文应为空");
    set_request_context("http-request");
    const std::string callback_id = current_request_id();
    {
        const std::string nested_id = "nested-request";
        RequestLogScope nested(nested_id);
        require(current_request_id() == nested_id, "临时上下文应覆盖当前请求");
    }
    require(current_request_id() == callback_id, "临时上下文结束后应恢复原请求");

    std::mutex mutex;
    std::condition_variable condition;
    int ready = 0;
    bool a_ok = false, b_ok = false;
    const auto worker = [&](const std::string& own_id, bool& ok) {
        set_request_context(own_id);
        {
            std::unique_lock<std::mutex> lock(mutex);
            ++ready;
            condition.notify_all();
            condition.wait(lock, [&] { return ready == 2; });
        }
        ok = current_request_id() == own_id;
        {
            RequestLogScope callback_scope(callback_id);
            ok = ok && current_request_id() == callback_id;
        }
        ok = ok && current_request_id() == own_id;
        clear_request_context();
        ok = ok && current_request_id() == "-";
    };
    std::thread first([&] { worker("worker-a", a_ok); });
    std::thread second([&] { worker("worker-b", b_ok); });
    first.join();
    second.join();
    require(a_ok && b_ok, "工作线程和回调上下文必须相互隔离并正确恢复");
    require(current_request_id() == callback_id, "子线程不能修改 HTTP 线程上下文");
    clear_request_context();
    require(current_request_id() == "-", "请求结束后应清除上下文");
    std::cout << "请求上下文回归测试通过\n";
}
