#pragma once
// 持有同一份 GPU 快照和统一设备租约；所有比较结束后才释放，避免采集覆盖。
#include "ChazhaoGonggong.h"
#include "../Gonggong/Jiekou.h"
#include <span>
#include <utility>
namespace fsdm::gpu {
void Reset() noexcept;
void Fail(int result, HRESULT detail = S_OK) noexcept;
void Status(FsChazhaoZhuangtai& output) noexcept;
void CpuFrame(const FsZhen& frame) noexcept;
// 恢复缓存结果所属的快照和设备记录，本次调用的 GPU 结果读回字节数清零。
void ReuseResult(const FsChazhaoZhuangtai& status) noexcept;
// 只清除已经释放/替换的模板；忙碌的查找不等待，在该查找结束后继续清理。
void PruneTemplates() noexcept;
class Session {
    struct Impl;
    std::unique_ptr<Impl> impl;
public:
    Session();
    ~Session();
    bool Read(FsZhen& frame);
    uint32_t Color(int x, int y);
    // color 坐标在客户区；picture 坐标在原图。输出按 TSPlug 扫描顺序排列。
    std::vector<std::pair<int,int>> Find(int left,int top,int right,int bottom,int direction,
        std::span<const chazhao::Point> points,size_t limit);
    std::vector<std::pair<int,int>> Picture(int left,int top,int right,int bottom,int direction,
        const std::shared_ptr<const chazhao::TemplateData>& pattern,UINT stride,
        chazhao::Color tolerance,size_t limit);
    bool Current() const;
};
}
