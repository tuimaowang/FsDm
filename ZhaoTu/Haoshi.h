#pragma once
// 本模块内部的分段耗时，不增加 DLL 的公开接口或改变返回值。
// 每个线程保存自己的记录；工作台调用前清空，查找入口设置操作类型。
#include <chrono>

namespace fsdm::haoshi {
enum class Operation { none, getColor, findColor, findPicture, ocr, compareColor, screenData, displayDead, matchNames };
enum class Phase { acquire, prepare, search, resources, result, initialize, preprocess, detect, recognize, wait };
struct Stage {
    bool executed{};
    double ms{};
};
struct Record {
    Operation operation{};
    Stage acquire, prepare, search;
    Stage resources, result;
    Stage initialize, preprocess, detect, recognize;
    Stage wait; // 卡屏检测主动等待的时间，不能混入像素比较耗时。
    bool gpu{};
};
inline thread_local Record current;
inline thread_local class Timer* activeTimer{};

inline void Reset(Operation operation = Operation::none) noexcept {
    current = {};
    current.operation = operation;
}

// 在整个阶段入口创建，离开作用域时累加；提前返回和异常同样计入。
// 多模板只在加载/扫描边界计时，绝不在逐像素热循环里查询时钟。
class Timer {
    using Clock = std::chrono::steady_clock;
    Stage& stage;
    Clock::time_point start;
    Timer* parent{};
public:
    explicit Timer(Phase phase) noexcept
        : stage(phase == Phase::acquire ? current.acquire :
                phase == Phase::prepare ? current.prepare :
                phase == Phase::resources ? current.resources :
                phase == Phase::initialize ? current.initialize :
                phase == Phase::preprocess ? current.preprocess :
                phase == Phase::detect ? current.detect :
                phase == Phase::recognize ? current.recognize :
                phase == Phase::wait ? current.wait :
                phase == Phase::result ? current.result : current.search),
          start(Clock::now()) {
        // 子阶段只记自身时间，同时暂停外层阶段，防止模板上传/结果读取重复计时。
        parent=activeTimer;
        if(parent) parent->stage.ms+=std::chrono::duration<double,std::milli>(start-parent->start).count();
        activeTimer=this;
        stage.executed = true;
    }
    ~Timer() noexcept {
        const auto end=Clock::now();
        stage.ms += std::chrono::duration<double, std::milli>(end - start).count();
        activeTimer=parent;
        if(parent) parent->start=end;
    }
    Timer(const Timer&) = delete;
    Timer& operator=(const Timer&) = delete;
};
}
