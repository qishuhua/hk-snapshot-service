#include "common/app_log.hpp"

#include <spdlog/sinks/ostream_sink.h>

#include <sstream>
#include <stdexcept>

namespace {

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void check_output(std::ostringstream& output, int line, const char* level) {
    const std::string expected = std::string("\"") + __FILE__ + ":" +
        std::to_string(line) + "\"|-|" + level +
        "|-|main|-|[request_id=log-test] [sdk] 日志示例打印 {value}";
    require(output.str().find(expected) != std::string::npos,
            "日志必须包含调用处文件、行号、函数、请求 ID 和原始消息");
    output.str("");
    output.clear();
}

} // 匿名命名空间

int main() {
    std::ostringstream output;
    auto sink = std::make_shared<spdlog::sinks::ostream_sink_mt>(output);
    sink->set_pattern(LOGGER_FORMAT);
    auto logger = std::make_shared<spdlog::logger>("log-test", sink);
    spdlog::set_default_logger(logger);
    logger->set_level(spdlog::level::debug);
    hkmedia::set_request_context("log-test");

    // 使用命名空间限定和非限定两种调用方式，确保宏兼容存量代码。
    int line = __LINE__ + 1;
    hkmedia::log_debug("sdk", "日志示例打印 {value}");
    check_output(output, line, "debug");
    using namespace hkmedia;
    line = __LINE__ + 1;
    log_info("sdk", "日志示例打印 {value}");
    check_output(output, line, "info");
    line = __LINE__ + 1;
    log_warn("sdk", "日志示例打印 {value}");
    check_output(output, line, "warning");
    line = __LINE__ + 1;
    log_error("sdk", "日志示例打印 {value}");
    check_output(output, line, "error");

    // 默认编译级别下仍可在运行时启用 debug，也必须遵守运行时过滤。
    logger->set_level(spdlog::level::info);
    log_debug("sdk", "不应输出");
    require(output.str().empty(), "info 级别应过滤 debug 日志");
    clear_request_context();
    log_info("sdk", "请求外日志");
    require(output.str().find("[request_id=-]") != std::string::npos,
            "请求外日志应保留默认请求标识");
    std::cout << "日志源码位置回归测试通过\n";
}
