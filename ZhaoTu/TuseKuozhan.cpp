// 原始区域数据和卡屏检测。只在调用时读回 CPU，不建立新设备或改动采集模式。
// 输出按客户区像素取样，避免渲染尺寸与客户区不同时，调用方无法计算内存长度。
#include "ZhaoTu.h"
#include "Haoshi.h"
#include "XianKaChazhao.h"
#include <algorithm>
#include <chrono>
#include <cstring>
#include <vector>

namespace {
struct Region { int x1, y1, x2, y2; };
// 截图的完整帧缓冲与公开数据分离；卡屏检测不会覆盖 GetScreenData 返回的地址。
thread_local std::vector<unsigned char> fullFrame, screenData;

bool Valid(const Region& r) {
    return r.x1>=0 && r.y1>=0 && r.x2>r.x1 && r.y2>r.y1 &&
        uint64_t(r.x2-r.x1)*uint64_t(r.y2-r.y1)<=FSDM_ZUIDA_ZIJIE/4;
}
bool Current(uint64_t version, FsZhuangtai& status) {
    status={};status.size=sizeof(status);
    int result=GetStatus(&status);
    if(result==FS_CHENGGONG) {
        if(!status.bound || (version && status.bindingVersion!=version)) result=FS_WEIBANGDING;
        else if(!status.hasFrame) result=status.result==FS_CHENGGONG ? FS_DENGDAISHOUZHEN : status.result;
        else result=status.result;
    }
    if(result!=FS_CHENGGONG) { fsdm::gpu::Fail(result,static_cast<HRESULT>(status.detail));return false; }
    return true;
}
// 查询容量本身不读回；遇到并发尺寸变化最多重试三次，不发布跨绑定的旧数据。
bool ReadRegion(const Region& r, uint64_t version, std::vector<unsigned char>& output, FsZhen& frame) {
    {
        fsdm::haoshi::Timer timer(fsdm::haoshi::Phase::acquire);
        frame={};frame.size=sizeof(frame);
        int rc=CopyFrame(nullptr,0,&frame);
        for(int attempt=0;attempt<3 && rc==FS_HUANCHONG_BUZU;++attempt) {
            if(!frame.requiredBytes || frame.requiredBytes>FSDM_ZUIDA_ZIJIE) break;
            fullFrame.resize(static_cast<size_t>(frame.requiredBytes));
            frame.size=sizeof(frame);
            rc=CopyFrame(fullFrame.data(),fullFrame.size(),&frame);
        }
        if(rc!=FS_CHENGGONG) {fsdm::gpu::Fail(rc);return false;}
        fsdm::gpu::CpuFrame(frame);
    }
    fsdm::haoshi::Timer timer(fsdm::haoshi::Phase::search);
    if(!frame.width || !frame.height || !frame.clientWidth || !frame.clientHeight ||
        uint32_t(r.x2)>frame.clientWidth || uint32_t(r.y2)>frame.clientHeight) {
        fsdm::gpu::Fail(FS_CANSHU_CUOWU);return false;
    }
    if(version && version!=frame.bindingVersion) {fsdm::gpu::Fail(FS_WEIBANGDING);return false;}
    const size_t width=static_cast<size_t>(r.x2-r.x1);
    output.resize(width*static_cast<size_t>(r.y2-r.y1)*4);
    for(int y=r.y1;y<r.y2;++y) {
        const auto* source=fullFrame.data()+size_t(uint64_t(y)*frame.height/frame.clientHeight)*frame.stride;
        auto* dest=output.data()+size_t(y-r.y1)*width*4;
        for(int x=r.x1;x<r.x2;++x,dest+=4) {
            const auto* pixel=source+size_t(uint64_t(x)*frame.width/frame.clientWidth)*4;
            dest[0]=pixel[0];dest[1]=pixel[1];dest[2]=pixel[2];
            dest[3]=0; // 00RRGGBB 不是 BGRA 的不透明 alpha，必须明确清零。
        }
    }
    FsZhuangtai status{};
    return Current(frame.bindingVersion,status);
}
void Start(fsdm::haoshi::Operation operation) {
    fsdm::haoshi::Reset(operation);
    fsdm::gpu::Reset();
}
}

uintptr_t WINAPI GetScreenData(int x1,int y1,int x2,int y2) {
    Start(fsdm::haoshi::Operation::screenData);
    // 包括失败调用也使旧地址失效，容量保留以便下次复用。
    screenData.clear();
    try {
        const Region r{x1,y1,x2,y2};
        if(!Valid(r)) {fsdm::gpu::Fail(FS_CANSHU_CUOWU);return 0;}
        FsZhuangtai status{};
        if(!Current(0,status)) return 0;
        FsZhen frame{};
        if(!ReadRegion(r,status.bindingVersion,screenData,frame)) {screenData.clear();return 0;}
        return reinterpret_cast<uintptr_t>(screenData.data());
    } catch(...) {screenData.clear();fsdm::gpu::Fail(FS_CAIJI_SHIBAI,E_OUTOFMEMORY);return 0;}
}

int WINAPI IsDisplayDead(int x1,int y1,int x2,int y2,int t) {
    Start(fsdm::haoshi::Operation::displayDead);
    try {
        const Region r{x1,y1,x2,y2};
        if(t<0 || !Valid(r)) {fsdm::gpu::Fail(FS_CANSHU_CUOWU);return 0;}
        FsZhuangtai status{};
        if(!Current(0,status)) return 0;
        const auto version=status.bindingVersion;
        // 两块区域缓冲轮流使用，不会在逐次比较时重新分配完整帧内存。
        std::vector<unsigned char> baseline, next;
        FsZhen first{};
        if(!ReadRegion(r,version,baseline,first)) return 0;
        auto number=first.frameNumber;
        using Clock=std::chrono::steady_clock;
        const auto deadline=Clock::now()+std::chrono::seconds(t);
        for(;;) {
            // 即使渲染暂停，也持续核对绑定生命状态；不能把销毁/重绑当作卡屏。
            if(!Current(version,status)) return 0;
            if(status.frame.width!=first.width || status.frame.height!=first.height ||
                status.frame.clientWidth!=first.clientWidth || status.frame.clientHeight!=first.clientHeight) {
                fsdm::gpu::Fail(FS_CHENGGONG);return 0;
            }
            if(status.frame.frameNumber!=number) {
                FsZhen frame{};
                if(!ReadRegion(r,version,next,frame)) return 0;
                fsdm::haoshi::Timer compare(fsdm::haoshi::Phase::search);
                if(frame.width!=first.width || frame.height!=first.height ||
                    frame.clientWidth!=first.clientWidth || frame.clientHeight!=first.clientHeight ||
                    next!=baseline) return 0;
                number=frame.frameNumber;
            }
            const auto now=Clock::now();
            if(now>=deadline) return 1;
            // 没有新快照时不重复执行相同画面的 CPU 读回。
            fsdm::haoshi::Timer wait(fsdm::haoshi::Phase::wait);
            const auto remaining=std::chrono::duration_cast<std::chrono::milliseconds>(deadline-now).count();
            Sleep(static_cast<DWORD>(std::clamp<int64_t>(remaining,1,50)));
        }
    } catch(...) {fsdm::gpu::Fail(FS_CAIJI_SHIBAI,E_OUTOFMEMORY);return 0;}
}
