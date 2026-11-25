#include "sdk/es_queue.hpp"

#include <chrono>
#include <stdexcept>
#include <utility>

int main() {
    hkmedia::EsQueue queue;
    for (int i = 0; i < 33; ++i) {
        hkmedia::EsPacket packet;
        packet.bytes.resize(1024 * 1024);
        packet.frame_no = i;
        if (queue.try_push(std::move(packet)) != hkmedia::PushStatus::Ok)
            throw std::runtime_error("码流入队失败");
    }
    queue.close();
    for (int i = 0; i < 33; ++i) {
        hkmedia::EsPacket packet;
        if (queue.pop_wait(packet, 0) != hkmedia::PopStatus::Ok || packet.frame_no != i)
            throw std::runtime_error("码流出队顺序错误");
    }
    hkmedia::EsPacket packet;
    if (queue.pop_wait(packet, 0) != hkmedia::PopStatus::Closed ||
        queue.try_push(std::move(packet)) != hkmedia::PushStatus::Closed)
        throw std::runtime_error("关闭队列行为错误");
}
