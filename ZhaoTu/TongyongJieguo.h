#pragma once
// 在 DLL 内构建并管理通用结果；对外仅返回 Jiekou.h 中的固定布局。
// 每次完成后深拷贝所有字段，后续调用和生成线程退出均不改变已经交给调用方的结果。
#include "../Gonggong/Jiekou.h"
#include <string>
#include <vector>
#include <cstddef>

namespace fsdm::jieguo {
struct Item {
    uint32_t fields{};
    int32_t id{-1}, x{-1}, y{-1};
    uint32_t color{};
    std::string text, file;
};
const FsTongyongJieguo* Save(const char* function, int status, int detail,
    const std::vector<Item>& items = {}, const std::string& fullText = {}) noexcept;
// 精确释放只比较登记的地址，不解引用调用方传入的地址。
bool FreeOne(const FsTongyongJieguo* result) noexcept;
void FreeAll() noexcept;
size_t Outstanding() noexcept;
class WorkbenchResult final {
public:
    explicit WorkbenchResult(const FsTongyongJieguo* result) noexcept : result_(result) {}
    ~WorkbenchResult() { FreeOne(result_); }
    WorkbenchResult(const WorkbenchResult&) = delete;
    WorkbenchResult& operator=(const WorkbenchResult&) = delete;
private:
    const FsTongyongJieguo* result_;
};
}
