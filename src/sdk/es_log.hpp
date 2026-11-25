#pragma once
// ES 帧排障信息：首帧默认记录，其余帧在 debug 级别记录元数据。

#include <cstdint>
#include <string>

#include "common/app_log.hpp"
#include "sdk/es_queue.hpp"

namespace hkmedia {

inline void log_es_packet(const std::string& component, int64_t play_handle,
                          const EsPacket& packet, bool first_frame) {
    if (!first_frame && !spdlog::default_logger_raw()->should_log(spdlog::level::debug)) return;
    const std::string message = std::string(first_frame ? "es.first_frame" :
        (packet.packet_type == 1 ? "es.key_frame" : "es.frame")) +
        " play_handle=" + std::to_string(play_handle) + " frame_no=" + std::to_string(packet.frame_no) +
        " packet_type=" + std::to_string(packet.packet_type) +
        " packet_mode=" + std::to_string(packet.packet_mode) +
        " sdk_pts=" + std::to_string(packet.pts) + " osd=" + packet.osd.iso_text() +
        " bytes=" + std::to_string(packet.bytes.size()) + " width=" + std::to_string(packet.width) +
        " height=" + std::to_string(packet.height);
    if (first_frame) log_info(component, message);
    else log_debug(component, message);
}

} // 命名空间 hkmedia
