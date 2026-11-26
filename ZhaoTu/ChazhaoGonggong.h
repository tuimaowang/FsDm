#pragma once
// CPU、GPU 共用同一份 TSPlug 比较样本。方向、透明色和文件版本由公共模板层准备，
// GPU 仅改变比较发生的位置，不另写一套颜色解析或相似度规则。
#include <windows.h>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>
namespace fsdm::chazhao {
inline std::atomic<size_t> templateCacheLimit{256ull*1024*1024};
struct Color { int r{}, g{}, b{}; };
struct TemplateData {
    UINT width{}, height{};
    struct Sample { size_t offset; Color color; };
    std::vector<Sample> samples;
    // 从模板缓存释放后禁止再驻留 GPU 缓存；已有查找仍可安全使用不可变的像素样本。
    mutable std::atomic_bool retired{false};
};
// tolerance 的低三字节为 RGB 容差，位 24 表示对整个 RGB 容差盒取反。
struct Point {
    int32_t x{}, y{};
    uint32_t color{}, tolerance{};
};
inline uint32_t Pack(Color c) {
    return (uint32_t(c.r) << 16) | (uint32_t(c.g) << 8) | uint32_t(c.b);
}
}
