// 登记独立字符串数组，供工作台按地址回收，或由外部调用全量释放接口。
// 锁保护登记和删除，不保护调用方读取裸指针。
// 调用方必须在释放前停止使用相应结果，尤其不能让全量释放与结果读取并发。
#include "ZifuchuanNeicun.h"
#include "ZhaoTu.h"
#include "TongyongJieguo.h"
#include <array>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <unordered_map>

namespace fsdm::zifuchuan {
namespace {
constexpr std::array names{
    "GetColor", "MatchPicName", "FindColorEx", "FindMultiColorEx",
    "FindPicS", "FindPicEx", "FindPicExS", "GetOcrError",
    "Ocr", "OcrEx", "OcrExOne", "OcrInFile",
    "FindStrS", "FindStrEx", "FindStrExS",
    "FindStrFastS", "FindStrFastEx", "FindStrFastExS",
    "AiYoloDetectObjects", "AiYoloSortsObjects", "AiYoloObjectsToString"
};
static_assert(names.size() == static_cast<size_t>(Function::count));
using Results = std::unordered_map<const char*, std::unique_ptr<char[]>>;
struct Registry {
    std::mutex lock;
    std::array<Results, names.size()> groups;
};
Registry& Storage() {
    // 不挂在线程本地：生成结果的线程退出后，其他线程仍能使用和释放该结果。
    // 容器最终销毁时，unique_ptr 使用同一个 DLL 内的 delete[] 回收剩余数组。
    static Registry registry;
    return registry;
}
}

const char* Save(Function function, std::string_view text) noexcept {
    try {
        const auto index = static_cast<size_t>(function);
        if (index >= names.size() || text.size() == std::numeric_limits<size_t>::max()) return nullptr;
        std::unique_ptr<char[]> bytes(new(std::nothrow) char[text.size() + 1]);
        if (!bytes) return nullptr;
        if (!text.empty()) std::memcpy(bytes.get(), text.data(), text.size());
        bytes[text.size()] = '\0';
        const char* address = bytes.get();
        auto& registry = Storage();
        std::lock_guard guard(registry.lock);
        // 如果登记本身内存不足，局部 unique_ptr 仍负责回收，不泄漏、不抛异常越过 C 接口。
        registry.groups[index].emplace(address, std::move(bytes));
        return address;
    } catch (...) { return nullptr; }
}

void FreeOne(const char* result) noexcept {
    if (!result) return;
    try {
        auto& registry = Storage();
        std::lock_guard guard(registry.lock);
        for (auto& group : registry.groups)
            if (group.erase(result)) return;
    } catch (...) {}
}

size_t Outstanding() noexcept {
    try {
        auto& registry = Storage();
        std::lock_guard guard(registry.lock);
        size_t count{};
        for (const auto& group : registry.groups) count += group.size();
        return count;
    } catch (...) { return 0; }
}
}

int WINAPI FreeAllStringResults(void) {
    try {
        auto& registry = fsdm::zifuchuan::Storage();
        std::lock_guard guard(registry.lock);
        for (auto& group : registry.groups) group.clear();
        fsdm::jieguo::FreeAll();
        return 1;
    } catch (...) { return 0; }
}

