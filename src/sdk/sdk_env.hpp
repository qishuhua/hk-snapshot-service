#pragma once
// HCNetSDK 进程级生命周期 + 单请求登录会话 RAII。

#include <HCNetSDK.h>

#include <cstring>
#include <mutex>
#include <stdexcept>
#include <string>

#include "common/app_log.hpp"
#include "common/error.hpp"
#include "config/config.hpp"

namespace hkmedia {

// NET_DVR_Init / NET_DVR_Cleanup 是进程全局的：main 启动时 ensure() 一次，
// HTTP 服务停止（所有请求均已结束）后 shutdown() 一次。
class SdkRuntime {
public:
    SdkRuntime() = delete;

    static void ensure(const Config& cfg) {
        std::call_once(init_once_, [&] {
            if (!NET_DVR_Init())
                throw std::runtime_error("NET_DVR_Init 初始化失败");
            NET_DVR_SetConnectTime(static_cast<DWORD>(cfg.connect_timeout_ms), 1);
            NET_DVR_SetRecvTimeOut(static_cast<DWORD>(cfg.connect_timeout_ms));
            if (cfg.sdk_log) {
                // SDK 自身日志写到工作目录 sdk_log/（容器内需保证可写）。
                NET_DVR_SetLogToFile(3, const_cast<char*>("sdk_log"), TRUE);
            }
            log_info("sdk", "HCNetSDK 已初始化 version=" +
                     std::to_string(NET_DVR_GetSDKVersion()) +
                     " connect_timeout_ms=" + std::to_string(cfg.connect_timeout_ms));
        });
    }

    static void shutdown() {
        std::call_once(cleanup_once_, [] {
            NET_DVR_Cleanup();
            log_info("sdk", "HCNetSDK 已清理");
        });
    }

private:
    inline static std::once_flag init_once_;
    inline static std::once_flag cleanup_once_;
};

// 单请求登录会话：构造登录、析构注销。不可复制。
// 参数直接传连接四元组和日志标签，避免底层依赖业务请求类型。
class LoginSession {
public:
    LoginSession(const std::string& host, int port, const std::string& username,
                 const std::string& password, const std::string& endpoint_label) {
        const int64_t started_ms = steady_ms();
        log_info("sdk", "login.start endpoint=" + endpoint_label + " username=" + username +
                           " async=0 api=NET_DVR_Login_V40");
        NET_DVR_USER_LOGIN_INFO li{};
        NET_DVR_DEVICEINFO_V40 info{};
        copy_fixed(li.sDeviceAddress, host, "host");
        copy_fixed(li.sUserName, username, "username");
        copy_fixed(li.sPassword, password, "password");
        li.wPort = static_cast<WORD>(port);
        li.bUseAsynLogin = FALSE;
        user_id_ = NET_DVR_Login_V40(&li, &info);
        if (user_id_ < 0) {
            const DWORD err = NET_DVR_GetLastError();
            // 1 = 用户名密码错误，2 = 权限不足 → 归为认证失败；其余为设备/网络故障。
            const int status = (err == NET_DVR_PASSWORD_ERROR || err == NET_DVR_NOENOUGHPRI)
                                   ? 401 : 502;
            log_error("sdk", "login.failed endpoint=" + endpoint_label + " sdk_error=" +
                                std::to_string(err) + " elapsed_ms=" +
                                std::to_string(steady_ms() - started_ms));
            throw CaptureError(status, status == 401 ? "login_rejected" : "login_failed",
                               "登录设备失败 " + endpoint_label +
                                   "，SDK 错误码 " + std::to_string(err));
        }
        try {
            const auto& device = info.struDeviceV30;
            log_info("sdk", "login.complete endpoint=" + endpoint_label + " user_id=" +
                                std::to_string(user_id_) + " analog_channels=" +
                                std::to_string(device.byChanNum) + " analog_start=" +
                                std::to_string(device.byStartChan) + " digital_channels=" +
                                std::to_string(static_cast<unsigned>(device.byIPChanNum) +
                                               (static_cast<unsigned>(device.byHighDChanNum) << 8)) +
                                " digital_start=" + std::to_string(device.byStartDChan) +
                                " elapsed_ms=" + std::to_string(steady_ms() - started_ms));
        } catch (...) {
            NET_DVR_Logout(user_id_);
            user_id_ = -1;
            throw;
        }
    }

    ~LoginSession() noexcept {
        if (user_id_ < 0) return;
        const int64_t started_ms = steady_ms();
        const LONG user_id = user_id_;
        user_id_ = -1;
        const bool ok = NET_DVR_Logout(user_id);
        const DWORD error = ok ? 0 : NET_DVR_GetLastError();
        try {
            const std::string message = "logout.complete user_id=" + std::to_string(user_id) +
                " success=" + std::to_string(ok) + " sdk_error=" + std::to_string(error) +
                " elapsed_ms=" + std::to_string(steady_ms() - started_ms);
            if (ok) log_info("sdk", message);
            else log_warn("sdk", message);
        } catch (...) {
        }
    }

    LoginSession(const LoginSession&) = delete;
    LoginSession& operator=(const LoginSession&) = delete;

    LONG user_id() const noexcept { return user_id_; }

private:
    // SDK 头文件中 sDeviceAddress[129] / sUserName[NAMELEN=64] / sPassword[PASSWDLEN=64]，
    // 由模板参数推导，无需硬编码长度。
    template <size_t N>
    static void copy_fixed(char (&dest)[N], const std::string& src, const char* field) {
        if (src.size() >= N)
            throw CaptureError(400, "invalid_param",
                               std::string(field) + " 长度超过 SDK 限制(" + std::to_string(N - 1) + ")");
        std::memcpy(dest, src.data(), src.size());
        dest[src.size()] = '\0';
    }

    LONG user_id_ = -1;
};

} // namespace hkmedia
