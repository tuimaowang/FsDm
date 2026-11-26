#pragma once
// 字符串返回值的统一所有权：每次导出调用登记独立的 new[] 数组。
// 计算过程仍可复用线程临时字符串，但不能直接把临时字符串地址交给调用方。
#include <cstddef>
#include <string_view>

namespace fsdm::zifuchuan {
enum class Function : size_t {
    GetColor, MatchPicName, FindColorEx, FindMultiColorEx,
    FindPicS, FindPicEx, FindPicExS, GetOcrError,
    Ocr, OcrEx, OcrExOne, OcrInFile,
    FindStrS, FindStrEx, FindStrExS,
    FindStrFastS, FindStrFastEx, FindStrFastExS,
    AiYoloDetectObjects, AiYoloSortsObjects, AiYoloObjectsToString, count
};

// 连同结尾的零字节一起分配。空结果也有独立地址，任何分配失败返回 nullptr。
// 返回后不依赖调用线程的存活；仅明确释放或模块最终销毁时回收。
const char* Save(Function function, std::string_view text) noexcept;

// 工作台专用的精确释放，不作为 DLL 导出接口；只删除本次显示的数组。
// 外部保留的数组不受工作台清理影响；对外仅提供全量释放文字数组的接口。
void FreeOne(const char* result) noexcept;

class WorkbenchResult final {
public:
    explicit WorkbenchResult(const char* result) noexcept : result_(result) {}
    ~WorkbenchResult() { FreeOne(result_); }
    WorkbenchResult(const WorkbenchResult&) = delete;
    WorkbenchResult& operator=(const WorkbenchResult&) = delete;
private:
    const char* result_;
};

// 内部诊断供内存生命周期测试使用，不增加 DLL 的公开导出。
size_t Outstanding() noexcept;
}
