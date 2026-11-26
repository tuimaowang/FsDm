// 直接使用实际采集实现和真实 WARP 纹理，检查资源身份与像素内容。
// 不增加 DLL 导出或工作台命令，覆盖跨槽位读回、尺寸变化及 OBS 源纹理缓存。
#include "Huichuan.h"
#include "Xuanran.h"
#include <atomic>
#include <thread>
#include <vector>

namespace {
void Require(bool ok,const char* message) {
    if (!ok) { std::fprintf(stderr,"FAIL: %s\n",message); throw Failure{}; }
}
struct Device {
    ComPtr<ID3D11Device> native;
    ComPtr<ID3D11DeviceContext> context;
    Device() {
        Check(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,
            D3D11_SDK_VERSION,&native,nullptr,&context),"reuse WARP device");
    }
    ComPtr<ID3D11Texture2D> texture(UINT width,UINT height,UINT color) {
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width=width; desc.Height=height; desc.MipLevels=desc.ArraySize=1;
        desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM; desc.SampleDesc.Count=1;
        desc.Usage=D3D11_USAGE_DEFAULT; desc.BindFlags=D3D11_BIND_SHADER_RESOURCE;
        desc.MiscFlags=D3D11_RESOURCE_MISC_SHARED;
        std::vector<UINT> pixels(size_t(width)*height,color);
        D3D11_SUBRESOURCE_DATA data{pixels.data(),width*4,0};
        ComPtr<ID3D11Texture2D> result;
        Check(native->CreateTexture2D(&desc,&data,&result),"reuse source texture");
        context->Flush();
        return result;
    }
    ComPtr<IDXGISwapChain> chain(HWND window) {
        ComPtr<IDXGIDevice> dxgi;
        ComPtr<IDXGIAdapter> adapter;
        ComPtr<IDXGIFactory> factory;
        Check(native.As(&dxgi),"reuse DXGI device");
        Check(dxgi->GetAdapter(&adapter),"reuse adapter");
        Check(adapter->GetParent(IID_PPV_ARGS(&factory)),"reuse factory");
        auto desc=LegacyDesc(window);
        ComPtr<IDXGISwapChain> result;
        Check(factory->CreateSwapChain(native.Get(),&desc,&result),"reuse swap chain");
        return result;
    }
};
bool ReadColor(replica::GpuSlot& slot,BYTE b,BYTE r) {
    std::vector<BYTE> bytes(size_t(slot.width)*slot.height*4);
    if (FAILED(replica::Read(slot,bytes.data()))) return false;
    for(size_t i=0;i<bytes.size();i+=4)
        if(bytes[i]!=b || bytes[i+1]!=0 || bytes[i+2]!=r || bytes[i+3]!=255) return false;
    return true;
}
void Readers() {
    Device device;
    auto red=device.texture(64,48,0xff0000ff),blue=device.texture(64,48,0xffff0000);
    auto first=std::make_unique<replica::GpuSlot>(),second=std::make_unique<replica::GpuSlot>();
    Check(replica::SnapshotTexture(red.Get(),*first,0),"snapshot first");
    Require(!first->reader && !first->staging,"GPU submission creates no readback device");
    Require(ReadColor(*first,0,255),"first CPU pixels");
    const auto reader=first->reader;
    const auto texture=first->texture11,opened=first->opened,staging=first->staging;
    const auto query=first->readerQuery;
    Check(replica::SnapshotTexture(red.Get(),*first,0),"repeat first");
    Require(ReadColor(*first,0,255) && first->reader==reader &&
        first->texture11==texture && first->opened==opened && first->staging==staging &&
        first->readerQuery==query,"stable size reuses device, texture, open, staging and query");
    Check(replica::SnapshotTexture(blue.Get(),*second,0),"snapshot second");
    Require(ReadColor(*second,255,0) && first->reader==second->reader,
        "same adapter shares readback device across slots");
    // 两个线程固定读取不同槽位、不同颜色，能暴露共享上下文交错导致的污染。
    std::atomic_bool ok{true};
    std::thread a([&] { for(int n=0;n<12;++n) if(!ReadColor(*first,0,255)) ok=false; });
    std::thread b([&] { for(int n=0;n<12;++n) if(!ReadColor(*second,255,0)) ok=false; });
    a.join(); b.join();
    Require(ok,"different-slot concurrent readback preserves complete frames");
    auto resized=device.texture(79,51,0xffff0000);
    Check(replica::SnapshotTexture(resized.Get(),*first,0),"resize first");
    Require(first->reader==reader && !first->opened && !first->staging &&
        !first->readerQuery,"resize retains reader but clears size-dependent resources");
    Require(ReadColor(*first,255,0) && first->width==79 && first->height==51 &&
        ReadColor(*second,255,0),"resize rebuilds only affected slot");
    first.reset(); second.reset();
    replica::GpuSlot rebound;
    Check(replica::SnapshotTexture(red.Get(),rebound,0),"fresh slot");
    Require(ReadColor(rebound,0,255) && rebound.reader==reader,"device cache survives slot destruction");
    std::puts("PASS reuse: per-adapter device, per-slot textures, concurrent readback and resize.");
}
HANDLE Shared(ID3D11Texture2D* texture) {
    ComPtr<IDXGIResource> resource;
    Check(texture->QueryInterface(IID_PPV_ARGS(&resource)),"reuse shared resource");
    HANDLE result{};
    Check(resource->GetSharedHandle(&result),"reuse shared handle");
    return result;
}
void Sources() {
    Window window,otherWindow;
    Device device,other;
    auto chain=device.chain(window.hwnd),otherChain=other.chain(otherWindow.hwnd);
    auto red=device.texture(64,48,0xff0000ff),blue=device.texture(79,51,0xffff0000);
    replica::SharedSource source;
    replica::GpuSlot first,second;
    const HANDLE handle=Shared(red.Get());
    Check(replica::SnapshotShared(chain.Get(),nullptr,handle,source,first,0),"OBS initial source");
    const auto opened=source.texture11;
    Require(ReadColor(first,0,255),"OBS source initial pixels");
    Check(replica::SnapshotShared(chain.Get(),nullptr,handle,source,second,0),"OBS repeat source");
    Require(source.texture11==opened && ReadColor(second,0,255),"OBS source reused across slots");
    Check(replica::SnapshotShared(chain.Get(),nullptr,Shared(blue.Get()),source,second,0),"OBS new handle");
    Require(source.texture11!=opened && second.width==79 && ReadColor(second,255,0),
        "OBS handle replacement changes pixels and size");
    Check(replica::SnapshotShared(otherChain.Get(),nullptr,handle,source,second,0),"OBS other device");
    Require(source.device11==other.native && ReadColor(second,0,255) &&
        second.reader==first.reader,"OBS device change reopens source while sharing adapter reader");
    source={};
    Require(!source.texture11 && !source.texture10 && !source.shared,"OBS session reset releases source");
    Check(replica::SnapshotShared(chain.Get(),nullptr,handle,source,second,0),"OBS session reopen");
    Require(ReadColor(second,0,255),"OBS same numeric handle can reopen after session reset");
    std::puts("PASS reuse: OBS source cache, handle/device/session replacement and pixels.");
}
}
int main() {
    try { Readers(); Sources(); return 0; }
    catch(...) { return 1; }
}
