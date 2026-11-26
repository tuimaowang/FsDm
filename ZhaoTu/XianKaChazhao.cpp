// 将 TSPlug 的逐通道比较移到 GPU；不使用评分、近似抽样或 CPU 图像校验。
// 大批候选分批处理，模板比较也分段提交，限制单次 GPU 工作量。
#include "XianKaChazhao.h"
#include "ZhaoTu.h"
#include "Haoshi.h"
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <list>
#include <mutex>
#include <stdexcept>

HRESULT FsAcquireProcessor(uint32_t,int32_t,FsGpuChuli*);
HRESULT FsReleaseProcessor(uint64_t);
using Microsoft::WRL::ComPtr;
namespace fsdm::gpu {
namespace {
using chazhao::Point;
constexpr UINT kBatch=65536, kTemplateChunk=256;
// 每个处理设备的模板缓存按需增长，只受共享配置中的字节预算约束；
// 超出预算时从最近较少使用的模板开始淘汰，避免反复上传常用图片。
std::atomic<uint64_t> shaderBuilds{},resourceBuilds{},templateUploads{};
thread_local FsChazhaoZhuangtai recent{};
struct Error {};
void Check(HRESULT hr) {
    if (FAILED(hr)) {
        Fail(hr==HRESULT_FROM_WIN32(WAIT_TIMEOUT) ? FS_GPU_CHAOSHI :
            hr==DXGI_ERROR_UNSUPPORTED ? FS_BUZHICHI : FS_CAIJI_SHIBAI,hr);
        throw Error{};
    }
}
// SRGB 使用 UNORM 视图读取原始通道，不能让硬件自动做伽马转换。
// 565 / 10 位先恢复原始整数，再按 CPU Convert 的整数除法得到相同的 8 位颜色。
constexpr char shader[] = R"(
Texture2D<float4> source : register(t0);
struct Point { int x; int y; uint color; uint tolerance; };
StructuredBuffer<Point> points : register(t1);
StructuredBuffer<int2> spiral : register(t2);
RWStructuredBuffer<uint> flags : register(u0);
RWStructuredBuffer<uint> bits : register(u1);
cbuffer Params : register(b0) {
    uint width,height,clientWidth,clientHeight;
    int left,top; uint scanWidth,scanHeight;
    uint direction,baseRank,count,renderCoordinates;
    uint pointStart,pointCount,initialize,mode;
    uint formatKind,flip,tolerance,picture;
};
uint scale(uint a,uint b,uint d) {
    // Shader Model 5 没有 64 位整数；以 16 位分块得到完整乘积，避免大客户区坐标溢出。
    uint a0=a&65535,a1=a>>16,b0=b&65535,b1=b>>16;
    uint w0=a0*b0,t=a1*b0+(w0>>16),w1=(t&65535)+a0*b1;
    uint hi=a1*b1+(t>>16)+(w1>>16),lo=(w1<<16)+(w0&65535);
    if(hi==0) return lo/d;
    uint q=0,rem=hi;
    for(int i=31;i>=0;--i) {
        uint carry=rem>>31; rem=(rem<<1)|((lo>>i)&1);
        if(carry!=0 || rem>=d) { rem-=d; q|=(1u<<i); }
    }
    return q;
}
uint3 rgb(uint c) { return uint3((c>>16)&255,(c>>8)&255,c&255); }
uint3 pixel(int2 p) {
    if(renderCoordinates==0)
        p=int2(scale(p.x,width,clientWidth),scale(p.y,height,clientHeight));
    if(flip!=0) p.y=height-1-p.y;
    float3 f=source.Load(int3(p,0)).rgb;
    if(formatKind==1) return (uint3(round(f*float3(31,63,31)))*255)/uint3(31,63,31);
    if(formatKind==2) return (uint3(round(f*1023))*255)/1023;
    return uint3(round(f*255));
}
int2 position(uint i) {
    if(direction==4) return spiral[i];
    uint rank=baseRank+i;
    uint x,y;
    if(direction>=5) { x=rank/scanHeight; y=rank%scanHeight; }
    else { x=rank%scanWidth; y=rank/scanWidth; }
    if(direction==2 || direction==3 || direction==6 || direction==8) x=scanWidth-1-x;
    if(direction==1 || direction==3 || direction==7 || direction==8) y=scanHeight-1-y;
    return int2(left+x,top+y);
}
[numthreads(64,1,1)]
void Compare(uint3 id:SV_DispatchThreadID) {
    uint i=id.x;
    if(i>=count) return;
    if(initialize==0 && flags[i]==0) return;
    int2 p=position(i);
    if(mode==1) { uint3 c=pixel(p); flags[0]=(c.r<<16)|(c.g<<8)|c.b; return; }
    uint matches=1;
    for(uint n=0;n<pointCount;++n) {
        Point t=points[pointStart+n];
        uint3 c=pixel(p+int2(t.x,t.y));
        uint3 target=rgb(t.color);
        uint3 delta=rgb(picture!=0?tolerance:t.tolerance);
        bool eq=all(abs(int3(c)-int3(target))<=int3(delta));
        bool anti=picture==0 && (t.tolerance&0x1000000)!=0;
        if(eq==anti) { matches=0; break; }
    }
    flags[i]=matches;
}
// 每 32 个命中标记压成一个整数。CPU 只读取这些位和 GetColor 的一个 RGB，
// 没有完整像素暂存纹理，也不会调用 CopyFrame。
[numthreads(64,1,1)]
void Pack(uint3 id:SV_DispatchThreadID) {
    uint word=id.x;
    if(word>=(count+31)/32) return;
    uint value=0;
    for(uint b=0;b<32;++b) {
        uint i=word*32+b;
        if(i<count && flags[i]!=0) value|=1u<<b;
    }
    bits[word]=value;
}
)";
struct Params {
    UINT width,height,clientWidth,clientHeight;
    int left,top; UINT scanWidth,scanHeight;
    UINT direction,baseRank,count,renderCoordinates;
    UINT pointStart,pointCount,initialize,mode;
    UINT formatKind,flip,tolerance,picture;
};
static_assert(sizeof(Params)==80 && sizeof(Point)==16);
struct Pattern {
    std::shared_ptr<const chazhao::TemplateData> owner;
    ComPtr<ID3D11Buffer> buffer;
    ComPtr<ID3D11ShaderResourceView> view;
    size_t bytes{};
};
struct Opened {
    uint64_t handle{},binding{};
    ComPtr<ID3D11Texture2D> texture;
};
struct Resources {
    uint64_t serial{};
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11ComputeShader> compare,pack;
    ComPtr<ID3D11Buffer> constants,flags,bits,staging,points,positions;
    ComPtr<ID3D11UnorderedAccessView> flagView,bitView;
    ComPtr<ID3D11ShaderResourceView> pointView,positionView;
    ComPtr<ID3D11Query> done;
    // 异常后 GPU 可能仍在读取快照。保留租约，待事件确认完成再归还采集槽位。
    uint64_t pendingFrame{};
    std::array<Opened,3> opened;
    size_t nextOpen{};
    ComPtr<ID3D11Texture2D> readable;
    ComPtr<ID3D11ShaderResourceView> sourceView;
    UINT width{},height{},pointCapacity{};
    DXGI_FORMAT format{};
    // 普通查找已持有处理上下文锁；此锁额外协调 FreePic 对模板资源的清理。
    std::mutex patternLock;
    std::list<Pattern> patterns;
    size_t patternBytes{};
};
std::mutex cacheLock;
auto& Cache() {
    // 与采集处理设备保持进程生命周期；不在 DllMain 的卸载锁里销毁 D3D 对象。
    static auto* cache=new std::array<std::shared_ptr<Resources>,3>;
    return *cache;
}
void Buffer(ID3D11Device* device,UINT bytes,UINT stride,UINT bind,
            ComPtr<ID3D11Buffer>& output,const void* data=nullptr,bool dynamic=false) {
    D3D11_BUFFER_DESC desc{};
    desc.ByteWidth=bytes; desc.BindFlags=bind;
    desc.Usage=dynamic ? D3D11_USAGE_DYNAMIC : D3D11_USAGE_DEFAULT;
    desc.CPUAccessFlags=dynamic ? D3D11_CPU_ACCESS_WRITE : 0;
    if(stride) { desc.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_STRUCTURED; desc.StructureByteStride=stride; }
    D3D11_SUBRESOURCE_DATA initial{data,0,0};
    Check(device->CreateBuffer(&desc,data?&initial:nullptr,&output)); ++resourceBuilds;
}
std::shared_ptr<Resources> GetResources(const FsGpuChuli& processor) {
    std::lock_guard guard(cacheLock);
    auto& cache=Cache();
    for(auto& r:cache) if(r && r->serial==processor.deviceSerial) return r;
    auto slot=std::find_if(cache.begin(),cache.end(),[](const auto& r){
        return !r || (r.use_count()==1 && !r->pendingFrame);
    });
    if(slot==cache.end()) { Fail(FS_ZIYUAN_MANG); throw Error{}; }
    auto r=std::make_shared<Resources>();
    r->serial=processor.deviceSerial; r->device=reinterpret_cast<ID3D11Device*>(processor.device);
    if(r->device->GetFeatureLevel()<D3D_FEATURE_LEVEL_11_0) Check(DXGI_ERROR_UNSUPPORTED);
    for(const auto* entry:{"Compare","Pack"}) {
        ComPtr<ID3DBlob> code,errors;
        Check(D3DCompile(shader,sizeof(shader)-1,"TSPlugGpu",nullptr,nullptr,entry,"cs_5_0",
            D3DCOMPILE_ENABLE_STRICTNESS|D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&code,&errors));
        auto& output=!strcmp(entry,"Compare")?r->compare:r->pack;
        Check(r->device->CreateComputeShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&output));
        ++shaderBuilds;
    }
    Buffer(r->device.Get(),sizeof(Params),0,D3D11_BIND_CONSTANT_BUFFER,r->constants,nullptr,true);
    Buffer(r->device.Get(),kBatch*4,4,D3D11_BIND_UNORDERED_ACCESS,r->flags);
    Buffer(r->device.Get(),kBatch/8,4,D3D11_BIND_UNORDERED_ACCESS,r->bits);
    Check(r->device->CreateUnorderedAccessView(r->flags.Get(),nullptr,&r->flagView));
    Check(r->device->CreateUnorderedAccessView(r->bits.Get(),nullptr,&r->bitView));
    D3D11_BUFFER_DESC desc{}; desc.ByteWidth=kBatch/8; desc.Usage=D3D11_USAGE_STAGING;
    desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    Check(r->device->CreateBuffer(&desc,nullptr,&r->staging)); ++resourceBuilds;
    // 结构化缓冲区不能同时带 CPU 写访问，使用 DEFAULT + UpdateSubresource 上传。
    Buffer(r->device.Get(),kBatch*8,8,D3D11_BIND_SHADER_RESOURCE,r->positions);
    Check(r->device->CreateShaderResourceView(r->positions.Get(),nullptr,&r->positionView));
    D3D11_QUERY_DESC query{D3D11_QUERY_EVENT,0};
    Check(r->device->CreateQuery(&query,&r->done));
    *slot=r;
    return r;
}
DXGI_FORMAT PlainFormat(DXGI_FORMAT f) {
    switch(f) {
    case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_B8G8R8X8_UNORM: case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8X8_TYPELESS: return DXGI_FORMAT_B8G8R8X8_UNORM;
    case DXGI_FORMAT_R10G10B10A2_UNORM: case DXGI_FORMAT_R10G10B10A2_TYPELESS:
        return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_B5G6R5_UNORM: return f;
    default: Check(DXGI_ERROR_UNSUPPORTED); return f;
    }
}
DXGI_FORMAT Typeless(DXGI_FORMAT f) {
    switch(f) {
    case DXGI_FORMAT_R8G8B8A8_UNORM:return DXGI_FORMAT_R8G8B8A8_TYPELESS;
    case DXGI_FORMAT_B8G8R8A8_UNORM:return DXGI_FORMAT_B8G8R8A8_TYPELESS;
    case DXGI_FORMAT_B8G8R8X8_UNORM:return DXGI_FORMAT_B8G8R8X8_TYPELESS;
    case DXGI_FORMAT_R10G10B10A2_UNORM:return DXGI_FORMAT_R10G10B10A2_TYPELESS;
    default:return f;
    }
}
// 与公共 scan 相同的螺旋，但保留游标，分批时不会从中心重新遍历。
struct Spiral {
    int left,top,right,bottom;
    int64_t x,y,step=1,first=0,last=-1;
    int side=0,dx=0,dy=0;
    bool segment=false;
    Spiral(int l,int t,int r,int b):left(l),top(t),right(r),bottom(b),
        x(l+(r-l-1)/2),y(t+(b-t-1)/2) {}
    std::pair<int,int> Next() {
        for(;;) {
            if(segment && first<=last) {
                const auto i=first++;
                return {int(x+i*dx),int(y+i*dy)};
            }
            if(segment) {
                x+=step*dx; y+=step*dy;
                if(++side==2) {side=0;++step;}
            }
            const int direction=int((2*(step-1)+side)%4);
            const int ddx[]={0,1,0,-1},ddy[]={-1,0,1,0};
            dx=ddx[direction];dy=ddy[direction]; first=0;last=step-1; segment=true;
            if(dx) {
                if(y<top || y>=bottom) last=-1;
                else if(dx>0) {first=(std::max)(first,int64_t(left)-x);last=(std::min)(last,int64_t(right)-1-x);}
                else {first=(std::max)(first,x-right+1);last=(std::min)(last,x-left);}
            } else {
                if(x<left || x>=right) last=-1;
                else if(dy>0) {first=(std::max)(first,int64_t(top)-y);last=(std::min)(last,int64_t(bottom)-1-y);}
                else {first=(std::max)(first,y-bottom+1);last=(std::min)(last,y-top);}
            }
        }
    }
};
}
void PruneTemplates() noexcept {
    try {
        std::lock_guard guard(cacheLock);
        for(auto& r:Cache()) {
            if(!r) continue;
            // 不能反向等待正在查找的线程，否则可能与处理上下文锁形成死锁。
            std::unique_lock lock(r->patternLock,std::try_to_lock);
            if(!lock.owns_lock()) continue;
            for(auto it=r->patterns.begin();it!=r->patterns.end();) {
                if(it->owner->retired.load()) {
                    r->patternBytes-=it->bytes;
                    it=r->patterns.erase(it);
                } else ++it;
            }
            const size_t limit=chazhao::templateCacheLimit.load(std::memory_order_relaxed);
            while(r->patternBytes>limit && !r->patterns.empty()) {
                r->patternBytes-=r->patterns.back().bytes;
                r->patterns.pop_back();
            }
        }
    } catch(...) {}
}
void Reset() noexcept { recent={}; recent.result=FS_CANSHU_CUOWU; }
void Fail(int result,HRESULT detail) noexcept { recent.result=result;recent.detail=detail; }
void CpuFrame(const FsZhen& frame) noexcept {
    Fail(FS_CHENGGONG);recent.frameNumber=frame.frameNumber;recent.bindingVersion=frame.bindingVersion;
}
void ReuseResult(const FsChazhaoZhuangtai& status) noexcept {
    recent=status;
    recent.resultReadbackBytes=0;
}
void Status(FsChazhaoZhuangtai& output) noexcept {
    output=recent; output.size=sizeof(output); output.version=1;
    output.shaderBuilds=shaderBuilds.load(); output.resourceBuilds=resourceBuilds.load();
    output.templateUploads=templateUploads.load();
}
struct Session::Impl {
    FsGpuZhen frame{};
    FsGpuChuli processor{};
    std::shared_ptr<Resources> resources;
    ID3D11DeviceContext* context{};
    bool submitted{},completed{};
    ~Impl() {
        if(processor.token) {
            if(resources && submitted && !completed &&
                SUCCEEDED(resources->device->GetDeviceRemovedReason())) {
                context->End(resources->done.Get());context->Flush();
                BOOL ready{};
                const HRESULT hr=context->GetData(resources->done.Get(),&ready,sizeof(ready),
                    D3D11_ASYNC_GETDATA_DONOTFLUSH);
                if(hr==S_FALSE || (hr==S_OK && !ready)) {
                    resources->pendingFrame=frame.token;
                    frame.token=0;
                }
            }
            // 包括异常路径也解除绑定；下一次 CPU 读回不会遇到仍在使用的 UAV/SRV。
            ID3D11ShaderResourceView* srv[3]{};
            ID3D11UnorderedAccessView* uav[2]{};
            context->CSSetShaderResources(0,3,srv);
            context->CSSetUnorderedAccessViews(0,2,uav,nullptr);
            context->CSSetShader(nullptr,nullptr,0);
            FsReleaseProcessor(processor.token);
        }
        if(frame.token) ReleaseGpuFrame(frame.token);
    }
    void Constants(const Params& p) {
        D3D11_MAPPED_SUBRESOURCE mapped{};
        Check(context->Map(resources->constants.Get(),0,D3D11_MAP_WRITE_DISCARD,0,&mapped));
        memcpy(mapped.pData,&p,sizeof(p));context->Unmap(resources->constants.Get(),0);
        auto* constant=resources->constants.Get();context->CSSetConstantBuffers(0,1,&constant);
    }
    void Wait() {
        context->End(resources->done.Get());context->Flush();
        const ULONGLONG end=GetTickCount64()+2000;
        for(;;) {
            BOOL ready{};
            const HRESULT hr=context->GetData(resources->done.Get(),&ready,sizeof(ready),D3D11_ASYNC_GETDATA_DONOTFLUSH);
            Check(hr);
            if(hr==S_OK && ready) {completed=true;return;}
            if(GetTickCount64()>=end) Check(HRESULT_FROM_WIN32(WAIT_TIMEOUT));
            SwitchToThread();
        }
    }
    std::vector<uint32_t> ReadResults(Params& p,bool color=false) {
        haoshi::Timer timer(haoshi::Phase::result);
        const UINT bytes=color?4:((p.count+31)/32)*4;
        if(!color) {
            context->CSSetShader(resources->pack.Get(),nullptr,0);
            context->Dispatch(((p.count+31)/32+63)/64,1,1);
        }
        ID3D11UnorderedAccessView* unbound[2]{};
        context->CSSetUnorderedAccessViews(0,2,unbound,nullptr);
        D3D11_BOX box{0,0,0,bytes,1,1};
        context->CopySubresourceRegion(resources->staging.Get(),0,0,0,0,
            color?resources->flags.Get():resources->bits.Get(),0,&box);
        Wait();
        D3D11_MAPPED_SUBRESOURCE mapped{};
        Check(context->Map(resources->staging.Get(),0,D3D11_MAP_READ,D3D11_MAP_FLAG_DO_NOT_WAIT,&mapped));
        std::vector<uint32_t> result(bytes/4);
        memcpy(result.data(),mapped.pData,bytes);context->Unmap(resources->staging.Get(),0);
        recent.resultReadbackBytes+=bytes;
        return result;
    }
    void Views() {
        auto& r=*resources;
        ID3D11UnorderedAccessView* uavs[]={r.flagView.Get(),r.bitView.Get()};
        context->CSSetUnorderedAccessViews(0,2,uavs,nullptr);
        auto* source=r.sourceView.Get();context->CSSetShaderResources(0,1,&source);
    }
    Params Parameters() const {
        const auto& f=frame.frame;
        return {f.width,f.height,f.clientWidth,f.clientHeight,0,0,0,0,0,0,0,0,0,0,0,0,
            resources->format==DXGI_FORMAT_B5G6R5_UNORM?1u:
            resources->format==DXGI_FORMAT_R10G10B10A2_UNORM?2u:0u,frame.flip,0,0};
    }
    std::vector<std::pair<int,int>> Run(int left,int top,int right,int bottom,int direction,
        ID3D11ShaderResourceView* pattern,UINT pointCount,size_t limit,bool picture,uint32_t tolerance) {
        std::vector<std::pair<int,int>> hits;
        if(left>=right || top>=bottom || !pointCount || !limit) return hits;
        auto p=Parameters();
        p.left=left;p.top=top;p.scanWidth=right-left;p.scanHeight=bottom-top;p.direction=direction;
        p.renderCoordinates=p.picture=picture;p.tolerance=tolerance;
        const uint64_t total=uint64_t(p.scanWidth)*p.scanHeight;
        if(total>UINT32_MAX) {Fail(FS_BUZHICHI,E_INVALIDARG);throw Error{};}
        Spiral cursor(left,top,right,bottom);
        std::vector<std::pair<int,int>> coordinates;
        for(uint64_t base=0;base<total;base+=kBatch) {
            p.baseRank=UINT(base);p.count=UINT((std::min)(uint64_t(kBatch),total-base));
            if(direction==4) {
                coordinates.clear();coordinates.reserve(p.count);
                for(UINT i=0;i<p.count;++i) coordinates.push_back(cursor.Next());
                static_assert(sizeof(std::pair<int,int>)==8);
                const D3D11_BOX box{0,0,0,p.count*8,1,1};
                context->UpdateSubresource(resources->positions.Get(),0,&box,coordinates.data(),0,0);
                auto* view=resources->positionView.Get();context->CSSetShaderResources(2,1,&view);
            }
            context->CSSetShaderResources(1,1,&pattern);
            std::vector<uint32_t> result;
            for(UINT start=0;start<pointCount;) {
                // 先比较少量样本；整批都不匹配时立即换下一批，避免大模板空提交。
                p.pointStart=start;p.pointCount=(std::min)(pointCount-start,start? kTemplateChunk:32u);
                p.initialize=start==0;Constants(p);Views();
                context->CSSetShader(resources->compare.Get(),nullptr,0);
                completed=false;context->Dispatch((p.count+63)/64,1,1);
                start+=p.pointCount;
                if(start==pointCount || p.initialize) {
                    result=ReadResults(p);
                    if(std::none_of(result.begin(),result.end(),[](UINT v){return v!=0;})) break;
                }
            }
            for(UINT i=0;i<p.count && hits.size()<limit;++i) {
                if(!(result[i/32]&(1u<<(i%32)))) continue;
                if(direction==4) hits.push_back(coordinates[i]);
                else {
                    const UINT rank=UINT(base)+i;
                    const bool column=direction>=5;
                    UINT x=column?rank/p.scanHeight:rank%p.scanWidth;
                    UINT y=column?rank%p.scanHeight:rank/p.scanWidth;
                    if(direction==2 || direction==3 || direction==6 || direction==8) x=p.scanWidth-1-x;
                    if(direction==1 || direction==3 || direction==7 || direction==8) y=p.scanHeight-1-y;
                    hits.emplace_back(left+int(x),top+int(y));
                }
            }
            if(hits.size()>=limit) break;
        }
        return hits;
    }
};
Session::Session():impl(std::make_unique<Impl>()) {}
Session::~Session() {
    // 先结束本次 GPU 使用并释放处理上下文，再清理查找期间被 FreePic 标记的旧模板。
    impl.reset();
    PruneTemplates();
}
bool Session::Read(FsZhen& info) {
    auto& s=*impl;
    s.frame.size=sizeof(s.frame);
    const int result=AcquireGpuFrame(&s.frame);
    if(result!=FS_CHENGGONG) {Fail(result);return false;}
    recent.frameNumber=s.frame.frame.frameNumber;recent.bindingVersion=s.frame.frame.bindingVersion;
    s.processor.size=sizeof(s.processor);
    Check(FsAcquireProcessor(s.frame.adapterLuidLow,s.frame.adapterLuidHigh,&s.processor));
    s.context=reinterpret_cast<ID3D11DeviceContext*>(s.processor.context);
    recent.deviceSerial=s.processor.deviceSerial;recent.devicesCreated=s.processor.devicesCreated;
    {
        haoshi::Timer timer(haoshi::Phase::resources);
        s.resources=GetResources(s.processor);
        auto& r=*s.resources;
        if(r.pendingFrame) {
            // 只在同一处理设备的上下文锁内检查旧工作，禁止重用尚在执行的资源。
            s.Wait();
            ReleaseGpuFrame(r.pendingFrame);r.pendingFrame=0;
        }
        auto opened=std::find_if(r.opened.begin(),r.opened.end(),[&](const Opened& o){
            return o.handle==s.frame.sharedHandle && o.binding==s.frame.frame.bindingVersion;
        });
        if(opened==r.opened.end()) {
            opened=r.opened.begin()+(r.nextOpen++%r.opened.size());
            *opened={};
            Check(r.device->OpenSharedResource(reinterpret_cast<HANDLE>(s.frame.sharedHandle),
                IID_PPV_ARGS(&opened->texture)));
            opened->handle=s.frame.sharedHandle;opened->binding=s.frame.frame.bindingVersion;
        }
        D3D11_TEXTURE2D_DESC desc{};opened->texture->GetDesc(&desc);
        const auto format=PlainFormat(desc.Format);
        if(!r.readable || r.width!=desc.Width || r.height!=desc.Height || r.format!=format) {
            r.sourceView.Reset();r.readable.Reset();
            auto d=desc;d.Format=Typeless(format);d.MiscFlags=0;d.BindFlags=D3D11_BIND_SHADER_RESOURCE;
            Check(r.device->CreateTexture2D(&d,nullptr,&r.readable));++resourceBuilds;
            D3D11_SHADER_RESOURCE_VIEW_DESC view{};
            view.Format=format;view.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2D;view.Texture2D.MipLevels=1;
            Check(r.device->CreateShaderResourceView(r.readable.Get(),&view,&r.sourceView));
            r.width=desc.Width;r.height=desc.Height;r.format=format;
        }
        s.context->CopyResource(r.readable.Get(),opened->texture.Get());
        s.submitted=true;s.completed=false;
    }
    info=s.frame.frame;
    Fail(FS_CHENGGONG);
    return info.width && info.height && info.clientWidth && info.clientHeight;
}
bool Session::Current() const {
    FsZhuangtai status{};status.size=sizeof(status);
    // 读取目标窗口的生命状态，防止解绑/重绑期间将旧版本结果作为新绑定结果返回。
    if(GetStatus(&status)!=FS_CHENGGONG || !status.bound ||
        status.bindingVersion!=impl->frame.frame.bindingVersion) {
        Fail(FS_WEIBANGDING);return false;
    }
    return true;
}
uint32_t Session::Color(int x,int y) {
    auto& s=*impl;auto p=s.Parameters();
    p.left=x;p.top=y;p.scanWidth=p.scanHeight=p.count=1;p.mode=1;p.initialize=1;
    s.Constants(p);s.Views();s.context->CSSetShader(s.resources->compare.Get(),nullptr,0);
    s.completed=false;s.context->Dispatch(1,1,1);
    const auto result=s.ReadResults(p,true);
    if(!Current()) throw Error{};
    return result[0];
}
std::vector<std::pair<int,int>> Session::Find(int left,int top,int right,int bottom,int direction,
    std::span<const Point> points,size_t limit) {
    auto& s=*impl;auto& r=*s.resources;
    {
        haoshi::Timer timer(haoshi::Phase::resources);
        if(points.size()>UINT32_MAX/sizeof(Point)) Check(E_INVALIDARG);
        if(points.size()>r.pointCapacity) {
            r.pointView.Reset();r.points.Reset();
            Buffer(r.device.Get(),UINT(points.size_bytes()),sizeof(Point),D3D11_BIND_SHADER_RESOURCE,r.points);
            Check(r.device->CreateShaderResourceView(r.points.Get(),nullptr,&r.pointView));
            r.pointCapacity=UINT(points.size());
        }
        const D3D11_BOX box{0,0,0,UINT(points.size_bytes()),1,1};
        s.context->UpdateSubresource(r.points.Get(),0,&box,points.data(),0,0);
    }
    auto result=s.Run(left,top,right,bottom,direction,r.pointView.Get(),UINT(points.size()),limit,false,0);
    if(!Current()) throw Error{};
    return result;
}
std::vector<std::pair<int,int>> Session::Picture(int left,int top,int right,int bottom,int direction,
    const std::shared_ptr<const chazhao::TemplateData>& pattern,UINT stride,chazhao::Color tolerance,size_t limit) {
    auto& s=*impl;auto& r=*s.resources;
    std::unique_lock patternGuard(r.patternLock);
    Pattern transient;ID3D11ShaderResourceView* view{};
    {
        haoshi::Timer timer(haoshi::Phase::prepare);
        auto entry=std::find_if(r.patterns.begin(),r.patterns.end(),[&](const Pattern& p){return p.owner==pattern;});
        if(entry!=r.patterns.end()) {
            view=entry->view.Get();r.patterns.splice(r.patterns.begin(),r.patterns,entry);
        } else {
            std::vector<Point> points;points.reserve(pattern->samples.size());
            for(const auto& p:pattern->samples)
                points.push_back({int32_t((p.offset%stride)/4),int32_t(p.offset/stride),chazhao::Pack(p.color),0});
            if(points.size()>UINT32_MAX/sizeof(Point)) Check(E_INVALIDARG);
            transient.owner=pattern;transient.bytes=points.size()*sizeof(Point);
            Buffer(r.device.Get(),UINT(transient.bytes),sizeof(Point),D3D11_BIND_SHADER_RESOURCE,transient.buffer,points.data());
            Check(r.device->CreateShaderResourceView(transient.buffer.Get(),nullptr,&transient.view));
            ++templateUploads;view=transient.view.Get();
            if(!pattern->retired.load() &&
                transient.bytes<=chazhao::templateCacheLimit.load(std::memory_order_relaxed)) {
                r.patternBytes+=transient.bytes;r.patterns.push_front(std::move(transient));
                const size_t cacheLimit=chazhao::templateCacheLimit.load(std::memory_order_relaxed);
                while(r.patternBytes>cacheLimit && !r.patterns.empty()) {
                    r.patternBytes-=r.patterns.back().bytes;r.patterns.pop_back();
                }
            }
        }
    }
    const auto cap=[](int n){return (std::min)(255,n);};
    const uint32_t delta=chazhao::Pack({cap(tolerance.r),cap(tolerance.g),cap(tolerance.b)});
    auto result=s.Run(left,top,right,bottom,direction,view,UINT(pattern->samples.size()),limit,true,delta);
    const bool current=Current();
    patternGuard.unlock();
    PruneTemplates();
    if(!current) throw Error{};
    return result;
}
}
