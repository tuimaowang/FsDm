// 跨找图、OCR 与 YOLO 的只读帧缓存；以绑定版本和帧编号识别同一份采集快照。
#include "ZhenhuanCun.h"
#include <atomic>
#include <mutex>

namespace fsdm::zhenhuancun {
namespace {
struct Cache {
    std::mutex lock;
    FsZhen info{};
    std::shared_ptr<const std::vector<unsigned char>> pixels;
};
Cache& SharedCache() {
    static auto* cache = new Cache;
    return *cache;
}
std::atomic<bool>& ResultReuseSwitch() {
    static std::atomic<bool> enabled{true};
    return enabled;
}
bool SameFrame(const FsZhen& a, const FsZhen& b) {
    return a.bindingVersion && a.bindingVersion == b.bindingVersion &&
        a.frameNumber == b.frameNumber && a.width == b.width && a.height == b.height &&
        a.stride == b.stride && a.clientWidth == b.clientWidth &&
        a.clientHeight == b.clientHeight;
}
bool ValidFrame(const FsZhen& info, size_t capacity) {
    return info.width && info.height && info.clientWidth && info.clientHeight &&
        info.stride >= uint64_t(info.width) * 4 &&
        uint64_t(info.stride) * info.height <= capacity &&
        info.requiredBytes <= capacity;
}
}

void SetResultReuse(bool enabled) noexcept {
    ResultReuseSwitch().store(enabled, std::memory_order_relaxed);
}
bool ResultReuseEnabled() noexcept {
    return ResultReuseSwitch().load(std::memory_order_relaxed);
}

int Read(Snapshot& snapshot) {
    snapshot = {};
    FsZhen queried{};
    queried.size = sizeof(queried);
    const int queryResult = CopyFrame(nullptr, 0, &queried);
    if (queryResult != FS_HUANCHONG_BUZU || !queried.requiredBytes ||
        queried.requiredBytes > FSDM_ZUIDA_ZIJIE)
        return queryResult == FS_HUANCHONG_BUZU ? FS_CANSHU_CUOWU : queryResult;

    auto& cache = SharedCache();
    std::lock_guard guard(cache.lock);
    if (cache.pixels && SameFrame(queried, cache.info) &&
        cache.pixels->size() >= queried.requiredBytes) {
        // 复用时保留“本次没有读回像素”的含义，且不修改缓存中被其他调用持有的元数据。
        queried.readbackMs = 0;
        snapshot.info = queried;
        snapshot.pixels = cache.pixels;
        snapshot.reused = true;
        return FS_CHENGGONG;
    }

    // 新帧读取失败时不允许后续请求误把旧缓存当作当前帧。
    cache.info = {};
    cache.pixels.reset();
    auto pixels = std::make_shared<std::vector<unsigned char>>(
        static_cast<size_t>(queried.requiredBytes));
    FsZhen copied = queried;
    int result = FS_HUANCHONG_BUZU;
    for (int attempt = 0; attempt < 3; ++attempt) {
        copied.size = sizeof(copied);
        result = CopyFrame(pixels->data(), pixels->size(), &copied);
        if (result == FS_CHENGGONG) {
            if (!ValidFrame(copied, pixels->size())) return FS_CANSHU_CUOWU;
            cache.info = copied;
            cache.pixels = std::move(pixels);
            snapshot.info = copied;
            snapshot.pixels = cache.pixels;
            return FS_CHENGGONG;
        }
        if (result != FS_HUANCHONG_BUZU || !copied.requiredBytes ||
            copied.requiredBytes > FSDM_ZUIDA_ZIJIE)
            return result;
        pixels->resize(static_cast<size_t>(copied.requiredBytes));
    }
    return result;
}
}
