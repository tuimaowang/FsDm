#pragma once
// 找图、OCR 和 YOLO 共用同一份 CPU 帧快照，避免同一帧被重复读回和复制。
#include "ZhaoTu.h"
#include <memory>
#include <vector>

namespace fsdm::zhenhuancun {
// pixels 持有不可变的完整 BGRA 快照；调用期间即使缓存更新，当前读者仍可安全使用。
struct Snapshot {
    FsZhen info{};
    std::shared_ptr<const std::vector<unsigned char>> pixels;
    // true 表示本次直接复用了缓存中的像素，没有再次从采集端读回。
    bool reused{};
};

// 先查询帧元数据；若绑定、帧号和尺寸未变则复用已有像素，否则从采集端读回新帧。
// 返回值为 FsJieguo；成功时 snapshot 同时持有帧信息和像素数据。
int Read(Snapshot& snapshot);
// 统一控制同帧分析结果缓存；关闭复用时各分析模块仍可复用采集像素。
void SetResultReuse(bool enabled) noexcept;
bool ResultReuseEnabled() noexcept;
}
