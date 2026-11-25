#include "capture/nearest_osd.hpp"

#include <stdexcept>

void require(bool ok) {
    if (!ok) throw std::runtime_error("最近 OSD 帧选择失败");
}

int main() {
    hkmedia::NearestOsd early(10000);
    require(!early.ready(10000));
    require(early.consider(9980) && !early.ready(9980));
    require(!early.consider(10030) && early.ready(10030));
    hkmedia::NearestOsd exact(10000);
    require(exact.consider(10000) && exact.ready(10000));
    hkmedia::NearestOsd choice(10000);
    require(!choice.consider(4999));
    require(choice.consider(5000));
    require(choice.consider(11000));
    require(choice.consider(9000));  // 等距时选择较早的帧。
    require(!choice.consider(11000));
    require(!choice.consider(13001));
    require(choice.consider(9950));
    require(!choice.consider(9800));  // OSD 回退时继续比较所有候选帧。
    require(choice.found() && choice.distance_ms() == 50);
    hkmedia::NearestOsd empty(10000);
    require(!empty.consider(4999) && !empty.consider(13001) && !empty.found());
    hkmedia::NearestOsd future(10000);
    require(future.consider(13000) && future.distance_ms() == 3000);
}
