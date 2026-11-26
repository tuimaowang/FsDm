#pragma once
// 工作台内部的整机占用率采样。创建对象时收集起点，OCR 返回后收集终点。
// 采样与 OCR 共用调用所在的时间区间，初始化、读数和查询释放的开销单独记录。
#include <windows.h>
#include <pdh.h>

namespace fsdm::zhanyong {
// 极短调用容易低于系统计数器的更新时间，工作台明确显示过短，避免误报空闲。
inline constexpr double minimumCallMilliseconds = 10.0;

struct CallUsage {
    double cpu = -1.0;
    double gpu = -1.0;
    double setupMs{};
    double finishMs{};
    bool tooShort{};
};

class CallSample {
    PDH_HQUERY query_{};
    PDH_HCOUNTER cpu_{}, gpu_{};
    double setupMs_{};
public:
    // CPU 与 GPU 使用同一个查询收集起止样本，任一计数器不可用时仍尝试另一项。
    CallSample() noexcept;
    ~CallSample() noexcept;
    CallSample(const CallSample&) = delete;
    CallSample& operator=(const CallSample&) = delete;

    // elapsed 为 OCR 函数自身耗时。所有路径均释放查询并保留调用方的线程错误码。
    // 本方法紧接 OCR 返回执行，单个采样对象只调用一次。
    CallUsage Finish(double elapsed) noexcept;
};
}
