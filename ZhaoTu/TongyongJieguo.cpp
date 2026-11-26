// 结果、条目数组和文字全部由同一个 DLL 分配及释放，避免跨运行库 delete[]。
// 登记表不依赖线程；释放接口与读取不能并发，调用方须先结束相关结果的使用。
#include "TongyongJieguo.h"
#include "ZhaoTu.h"
#include <cstring>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace fsdm::jieguo {
namespace {
struct Owned {
    FsTongyongJieguo result{};
    std::unique_ptr<FsJieguoXiang[]> items;
    std::vector<std::unique_ptr<char[]>> strings;
    const char* Copy(const std::string& text) {
        auto bytes=std::make_unique<char[]>(text.size()+1);
        std::memcpy(bytes.get(),text.c_str(),text.size()+1);
        const char* address=bytes.get();
        strings.push_back(std::move(bytes));
        return address;
    }
};
struct Registry {
    std::mutex lock;
    std::unordered_map<const FsTongyongJieguo*,std::unique_ptr<Owned>> results;
};
Registry& Storage() { static Registry storage; return storage; }
}
const FsTongyongJieguo* Save(const char* function,int status,int detail,
    const std::vector<Item>& values,const std::string& fullText) noexcept {
    try {
        if(!function || values.size()>UINT32_MAX) return nullptr;
        auto owned=std::make_unique<Owned>();
        auto& r=owned->result;
        r.size=sizeof(r);r.version=1;r.status=status;r.detail=detail;
        // 执行失败时不返回半途产生的坐标或全文，正常无命中则 status=0/count=0。
        r.fullText=owned->Copy(status==FS_CHENGGONG?fullText:std::string{});
        if(status==FS_CHENGGONG && !values.empty()) {
            r.count=static_cast<uint32_t>(values.size());
            owned->items=std::make_unique<FsJieguoXiang[]>(r.count);
            for(uint32_t i=0;i<r.count;++i) {
                const auto& v=values[i];auto& item=owned->items[i];
                item.fields=v.fields;item.id=v.id;item.x=v.x;item.y=v.y;item.color=v.color;
                item.text=owned->Copy(v.text);item.file=owned->Copy(v.file);
            }
            r.items=owned->items.get();
        }
        const auto* address=&r;
        auto& registry=Storage();std::lock_guard guard(registry.lock);
        registry.results.emplace(address,std::move(owned));
        return address;
    } catch(...) { return nullptr; }
}
bool FreeOne(const FsTongyongJieguo* result) noexcept {
    if(!result) return true;
    try {
        auto& registry=Storage();std::lock_guard guard(registry.lock);
        return registry.results.erase(result)!=0;
    } catch(...) { return false; }
}
void FreeAll() noexcept {
    try {auto& registry=Storage();std::lock_guard guard(registry.lock);registry.results.clear();} catch(...) {}
}
size_t Outstanding() noexcept {
    try {auto& registry=Storage();std::lock_guard guard(registry.lock);return registry.results.size();} catch(...) {return 0;}
}
}
int WINAPI FreeResult(const FsTongyongJieguo* result) {return fsdm::jieguo::FreeOne(result)?1:0;}
int WINAPI FreeAllResults(void) {return FreeAllStringResults();}
