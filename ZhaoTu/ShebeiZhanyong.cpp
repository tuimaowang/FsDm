// 按调用现场采样整机 CPU/GPU 占用率；每次创建独立查询，支持多线程同时测试。
// PDH 是 Windows 的性能计数器接口，由系统提供累计时间，两次采样后计算占用百分比。
#include "ZhaoTu.h"
#include "ShebeiZhanyong.h"
#include <pdh.h>
#include <pdhmsg.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <map>
#include <string>
#include <vector>

namespace {
constexpr DWORD sampleMilliseconds = 200;
constexpr const wchar_t* cpuCounterPath = L"\\Processor Information(_Total)\\% Processor Time";
constexpr const wchar_t* gpuCounterPath = L"\\GPU Engine(*)\\Utilization Percentage";

// 工作台附加统计不能覆盖被测函数的 GetLastError，清理查询时也遵循同一规则。
struct PreserveError {
    DWORD error = GetLastError();
    ~PreserveError() { SetLastError(error); }
};

void CloseQuery(PDH_HQUERY query) noexcept {
    PreserveError preserved;
    if (query) PdhCloseQuery(query);
}

// 查询拥有其添加的全部计数器，成功或失败退出时均由 PdhCloseQuery 统一释放。
struct Query {
    PDH_HQUERY handle{};
    ~Query() {
        CloseQuery(handle);
    }
    Query() = default;
    Query(const Query&) = delete;
    Query& operator=(const Query&) = delete;
};

double Failure(DWORD error) noexcept {
    SetLastError(error);
    return -1.0;
}

bool ValidStatus(DWORD status) noexcept {
    return status == PDH_CSTATUS_VALID_DATA || status == PDH_CSTATUS_NEW_DATA;
}

// 英文路径接口会让系统转换为本机语言，避免中文 Windows 上计数器名称不同。
// 独立保存起止样本，连续调用与并发调用均测量各自的约 200 毫秒时间窗口。
PDH_STATUS Sample(Query& query, const wchar_t* path, PDH_HCOUNTER& counter) {
    PDH_STATUS status = PdhOpenQueryW(nullptr, 0, &query.handle);
    if (status != ERROR_SUCCESS) return status;
    status = PdhAddEnglishCounterW(query.handle, path, 0, &counter);
    if (status != ERROR_SUCCESS) return status;
    status = PdhCollectQueryData(query.handle);
    if (status != ERROR_SUCCESS) return status;
    Sleep(sampleMilliseconds);
    return PdhCollectQueryData(query.handle);
}

// 导出函数和工作台读取同一套百分比，工作台自行提供调用前后的样本。
double ReadCpuUsage(PDH_HCOUNTER counter) noexcept {
    PDH_FMT_COUNTERVALUE value{};
    const PDH_STATUS formatted = PdhGetFormattedCounterValue(counter, PDH_FMT_DOUBLE, nullptr, &value);
    if (formatted != ERROR_SUCCESS) return Failure(formatted);
    if (!ValidStatus(value.CStatus)) return Failure(value.CStatus);
    if (!std::isfinite(value.doubleValue)) return Failure(ERROR_INVALID_DATA);
    SetLastError(ERROR_SUCCESS);
    return std::clamp(value.doubleValue, 0.0, 100.0);
}

double ReadGpuUsage(PDH_HCOUNTER counter) {
    // 通配符计数器同时包含各进程、各显卡和各引擎。先查询容量，再取得同一批格式化样本。
    DWORD bytes{}, count{};
    constexpr DWORD format = PDH_FMT_DOUBLE | PDH_FMT_NOCAP100;
    PDH_STATUS formatted = PdhGetFormattedCounterArrayW(counter, format, &bytes, &count, nullptr);
    if (formatted != PDH_MORE_DATA) {
        return Failure(formatted == ERROR_SUCCESS ? PDH_NO_DATA : formatted);
    }
    std::vector<unsigned char> buffer(bytes);
    auto* items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buffer.data());
    formatted = PdhGetFormattedCounterArrayW(counter, format, &bytes, &count, items);
    if (formatted != ERROR_SUCCESS) return Failure(formatted);

    // 实例格式为 pid_进程_luid_适配器_phys_显卡_eng_编号_engtype_类型。
    // 同一显卡同一引擎需要先累加所有进程，随后取最繁忙引擎。
    // 保留 LUID、phys、eng，防止不同显卡或不同引擎的使用时间被错误合并。
    std::map<std::wstring, double> engines;
    for (DWORD i = 0; i < count; ++i) {
        const auto& item = items[i];
        if (!item.szName || !ValidStatus(item.FmtValue.CStatus) ||
            !std::isfinite(item.FmtValue.doubleValue)) continue;
        const std::wstring name(item.szName);
        const auto adapter = name.find(L"_luid_");
        const auto type = name.find(L"_engtype_", adapter);
        if (adapter == std::wstring::npos || type == std::wstring::npos) continue;
        engines[name.substr(adapter + 1, type - adapter - 1)] +=
            std::max(0.0, item.FmtValue.doubleValue);
    }
    // 驱动、权限或实例尚无有效采样时明确报失败，避免将无法测量误报成空闲。
    if (engines.empty()) return Failure(PDH_NO_DATA);
    double busiest{};
    for (const auto& engine : engines) busiest = std::max(busiest, engine.second);
    SetLastError(ERROR_SUCCESS);
    return std::clamp(busiest, 0.0, 100.0);
}
}

double WINAPI GetCpuUsage(void) {
    Query query;
    PDH_HCOUNTER counter{};
    // Processor Information 的 _Total 包括所有处理器组，覆盖超过 64 个逻辑处理器的设备。
    const PDH_STATUS status = Sample(query, cpuCounterPath, counter);
    if (status != ERROR_SUCCESS) return Failure(status);
    return ReadCpuUsage(counter);
}

double WINAPI GetGpuUsage(void) {
    try {
        Query query;
        PDH_HCOUNTER counter{};
        const PDH_STATUS status = Sample(query, gpuCounterPath, counter);
        if (status != ERROR_SUCCESS) return Failure(status);
        return ReadGpuUsage(counter);
    } catch (const std::bad_alloc&) {
        return Failure(ERROR_NOT_ENOUGH_MEMORY);
    } catch (...) {
        return Failure(ERROR_INVALID_DATA);
    }
}

namespace fsdm::zhanyong {
CallSample::CallSample() noexcept {
    PreserveError preserved;
    const auto start = std::chrono::steady_clock::now();
    if (PdhOpenQueryW(nullptr, 0, &query_) == ERROR_SUCCESS) {
        if (PdhAddEnglishCounterW(query_, cpuCounterPath, 0, &cpu_) != ERROR_SUCCESS)
            cpu_ = nullptr;
        if (PdhAddEnglishCounterW(query_, gpuCounterPath, 0, &gpu_) != ERROR_SUCCESS)
            gpu_ = nullptr;
        if ((!cpu_ && !gpu_) || PdhCollectQueryData(query_) != ERROR_SUCCESS) {
            CloseQuery(query_);
            query_ = nullptr;
        }
    } else {
        CloseQuery(query_);
        query_ = nullptr;
    }
    setupMs_ = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - start).count();
}

CallSample::~CallSample() noexcept {
    CloseQuery(query_);
}

CallUsage CallSample::Finish(double elapsed) noexcept {
    PreserveError preserved;
    const auto start = std::chrono::steady_clock::now();
    CallUsage result;
    result.setupMs = setupMs_;
    result.tooShort = elapsed < minimumCallMilliseconds;
    if (query_ && !result.tooShort && PdhCollectQueryData(query_) == ERROR_SUCCESS) {
        if (cpu_) result.cpu = ReadCpuUsage(cpu_);
        try {
            if (gpu_) result.gpu = ReadGpuUsage(gpu_);
        } catch (...) {
            // 占用率统计失败只影响这一项显示，OCR 的结果和诊断仍由原调用提供。
        }
    }
    CloseQuery(query_);
    query_ = nullptr;
    result.finishMs = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - start).count();
    return result;
}
}
