// 呈现时只提交 GPU 快照；读取线程按需创建 CPU 暂存资源并转换为 BGRA。
#include "Huichuan.h"
#include <d3d10_1.h>
#include <d3d11on12.h>
#include <wrl/client.h>
#include <cstring>
#include <d3d11_4.h>
#include <chrono>
#include <array>
#include <atomic>
#include <unordered_map>
#ifdef ZHUATU_SHENGCHENG
#include "ZhuaTu.h"
#endif

using Microsoft::WRL::ComPtr;

namespace replica {
namespace {

DXGI_FORMAT TypedFormat(DXGI_FORMAT format) {
    switch (format) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_B8G8R8X8_TYPELESS: return DXGI_FORMAT_B8G8R8X8_UNORM;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
    default: return format;
    }
}

HRESULT CheckSize(UINT width, UINT height, DXGI_FORMAT format) {
    if (!width || !height || uint64_t{width} * height > kPixelCapacity / 4)
        return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
    switch (TypedFormat(format)) {
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8X8_UNORM:
    case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
    case DXGI_FORMAT_B5G6R5_UNORM:
    case DXGI_FORMAT_R10G10B10A2_UNORM:
        return S_OK;
    default:
        return DXGI_ERROR_UNSUPPORTED;
    }
}

void Convert(const void* data, UINT pitch, DXGI_FORMAT format, const Frame& frame, BYTE* dst) {
    format = TypedFormat(format);
    for (UINT y = 0; y < frame.height; ++y) {
        const auto* src = static_cast<const BYTE*>(data) + size_t{pitch} * y;
        if (format == DXGI_FORMAT_B8G8R8A8_UNORM || format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB) {
            std::memcpy(dst, src, size_t{frame.width} * 4);
            dst += size_t{frame.width} * 4;
            continue;
        }
        for (UINT x = 0; x < frame.width; ++x, dst += 4) {
            switch (format) {
            case DXGI_FORMAT_R8G8B8A8_UNORM:
            case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
                dst[0] = src[4 * x + 2];
                dst[1] = src[4 * x + 1];
                dst[2] = src[4 * x];
                dst[3] = src[4 * x + 3];
                break;
            case DXGI_FORMAT_B5G6R5_UNORM: {
                uint16_t value;
                std::memcpy(&value, src + 2 * x, sizeof(value));
                dst[0] = static_cast<BYTE>((value & 31) * 255 / 31);
                dst[1] = static_cast<BYTE>(((value >> 5) & 63) * 255 / 63);
                dst[2] = static_cast<BYTE>((value >> 11) * 255 / 31);
                dst[3] = 255;
                break;
            }
            case DXGI_FORMAT_R10G10B10A2_UNORM: {
                uint32_t value;
                std::memcpy(&value, src + 4 * x, sizeof(value));
                dst[0] = static_cast<BYTE>(((value >> 20) & 1023) * 255 / 1023);
                dst[1] = static_cast<BYTE>(((value >> 10) & 1023) * 255 / 1023);
                dst[2] = static_cast<BYTE>((value & 1023) * 255 / 1023);
                dst[3] = static_cast<BYTE>((value >> 30) * 85);
                break;
            }
            default: // BGRX
                std::memcpy(dst, src + 4 * x, 3);
                dst[3] = 255;
            }
        }
    }
}


// DX12 桥接按设备/队列复用；绝不在缓存中长期持有交换链后缓冲。
ComPtr<ID3D12Device> bridgeDevice;
ComPtr<ID3D12CommandQueue> bridgeQueue;
ComPtr<ID3D11Device> bridge11;
ComPtr<ID3D11On12Device> bridge;

bool SameAdapter(const LUID& a, const LUID& b) {
    return a.LowPart==b.LowPart && a.HighPart==b.HighPart;
}
// 三个快照槽最多同时使用三张显卡；这里最多保留三个读回设备。
// 无槽位引用的旧设备可被淘汰，健康设备跨尺寸变化、解绑重绑继续使用。
// 缓存锁不参与 GPU 等待；每个读回设备另外串行保护自己的立即上下文。
std::mutex readerCacheLock;
struct ReaderEntry { std::shared_ptr<ReadDevice> value; uint64_t used{}; };
auto& ReaderCache() {
    // DLL 固定加载到进程结束，缓存也采用相同生命周期。首次读回时才创建，
    // 不注册静态析构：进程退出时在 DLL 卸载锁内释放 D3D 设备会触发驱动清理，
    // 此时依赖模块可能已开始退出。正常淘汰/设备丢失仍在读回线程释放旧项；
    // 最多三个常驻缓存项由系统随进程回收，不能在 DllMain 中强行释放。
    static auto* cache = new std::array<ReaderEntry, 3>;
    return *cache;
}
uint64_t readerClock{};
std::atomic<uint64_t> devicesCreated{};
HRESULT Reader(const LUID& luid, std::shared_ptr<ReadDevice>& output) {
    std::lock_guard lock(readerCacheLock);
    ReaderEntry* replace{};
    for (auto& entry : ReaderCache()) {
        if (entry.value && SameAdapter(entry.value->luid,luid)) {
            if (SUCCEEDED(entry.value->device->GetDeviceRemovedReason())) {
                entry.used=++readerClock; output=entry.value; return S_OK;
            }
            replace=&entry;
            break;
        }
        if ((!entry.value || entry.value.use_count()==1) &&
            (!replace || entry.used<replace->used)) replace=&entry;
    }
    if (!replace) return DXGI_ERROR_NOT_CURRENTLY_AVAILABLE;
    auto created=std::make_shared<ReadDevice>();
    created->luid=luid;
    ComPtr<IDXGIFactory4> factory;
    ComPtr<IDXGIAdapter> adapter;
    HRESULT hr=CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    if (SUCCEEDED(hr)) hr=factory->EnumAdapterByLuid(luid,IID_PPV_ARGS(&adapter));
    if (SUCCEEDED(hr)) hr=D3D11CreateDevice(adapter.Get(),D3D_DRIVER_TYPE_UNKNOWN,nullptr,0,
        nullptr,0,D3D11_SDK_VERSION,&created->device,nullptr,&created->context);
    if (FAILED(hr)) return hr;
    created->serial=++devicesCreated;
    replace->value=created; replace->used=++readerClock;
    output=std::move(created);
    return S_OK;
}
void ResetReadTextures(GpuSlot& slot) {
    slot.opened.Reset(); slot.staging.Reset(); slot.readerQuery.Reset();
}

HRESULT Adapter(IUnknown* device, LUID& luid) {
    ComPtr<IDXGIDevice> dxgi;
    ComPtr<IDXGIAdapter> adapter;
    DXGI_ADAPTER_DESC desc{};
    HRESULT hr = device->QueryInterface(IID_PPV_ARGS(&dxgi));
    if (SUCCEEDED(hr)) hr = dxgi->GetAdapter(&adapter);
    if (SUCCEEDED(hr)) hr = adapter->GetDesc(&desc);
    if (SUCCEEDED(hr)) luid = desc.AdapterLuid;
    return hr;
}

// Query 的 GetData 可能由读取线程调用，打开运行库的线程保护以协调立即上下文。
HRESULT Protect11(ID3D11DeviceContext* context) {
    ComPtr<ID3D11Multithread> protect;
    const HRESULT hr = context->QueryInterface(IID_PPV_ARGS(&protect));
    if (SUCCEEDED(hr)) protect->SetMultithreadProtected(TRUE);
    return hr;
}

HRESULT Prepare11(ID3D11Texture2D* source, GpuSlot& slot, const D3D11_BOX* region = nullptr) {
    D3D11_TEXTURE2D_DESC desc{};
    source->GetDesc(&desc);
    if (region) {
        desc.Width = region->right - region->left;
        desc.Height = region->bottom - region->top;
    }
    HRESULT hr = CheckSize(desc.Width, desc.Height, desc.Format);
    if (FAILED(hr)) return hr;
    ComPtr<ID3D11Device> device;
    source->GetDevice(&device);
    if (slot.texture11 && slot.device11.Get() == device.Get() && slot.width == desc.Width &&
        slot.height == desc.Height && slot.format == TypedFormat(desc.Format)) return S_OK;
    // 调用方只会对 users==0 的槽位重建；已有租约的旧尺寸不受影响。
    slot.texture10.Reset(); slot.query10.Reset(); slot.device10.Reset();
    slot.texture11.Reset(); slot.query11.Reset();
    ResetReadTextures(slot);
    slot.shared = nullptr;
    slot.device11 = device;
    device->GetImmediateContext(&slot.context);
    hr = Protect11(slot.context.Get());
    if (FAILED(hr)) return hr;
    desc.Format = TypedFormat(desc.Format);
    desc.SampleDesc = {1, 0};
    desc.MipLevels = desc.ArraySize = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    desc.CPUAccessFlags = 0;
    desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
    hr = device->CreateTexture2D(&desc, nullptr, &slot.texture11);
    ComPtr<IDXGIResource> shared;
    if (SUCCEEDED(hr)) hr = slot.texture11.As(&shared);
    if (SUCCEEDED(hr)) hr = shared->GetSharedHandle(&slot.shared);
    D3D11_QUERY_DESC query{D3D11_QUERY_EVENT, 0};
    if (SUCCEEDED(hr)) hr = device->CreateQuery(&query, &slot.query11);
    if (SUCCEEDED(hr)) hr = Adapter(device.Get(), slot.luid);
    if (FAILED(hr)) { slot.texture11.Reset(); return hr; }
    slot.width = desc.Width; slot.height = desc.Height; slot.format = desc.Format;
    return S_OK;
}

HRESULT Copy11(ID3D11Texture2D* source, GpuSlot& slot, ID3D11On12Device* wrapper = nullptr,
               const D3D11_BOX* region = nullptr) {
    HRESULT hr = Prepare11(source, slot, region);
    if (FAILED(hr)) return hr;
    D3D11_TEXTURE2D_DESC desc{}; source->GetDesc(&desc);
    ID3D11Resource* resources[] = {source};
    if (wrapper) wrapper->AcquireWrappedResources(resources, 1);
    if (region)
        slot.context->CopySubresourceRegion(slot.texture11.Get(), 0, 0, 0, 0, source, 0, region);
    else if (desc.SampleDesc.Count > 1)
        slot.context->ResolveSubresource(slot.texture11.Get(), 0, source, 0, slot.format);
    else slot.context->CopyResource(slot.texture11.Get(), source);
    if (wrapper) wrapper->ReleaseWrappedResources(resources, 1);
    slot.context->End(slot.query11.Get());
    slot.context->Flush(); // 提交给 GPU；不在呈现线程等待事件或像素。
    return S_OK;
}

HRESULT Copy10(ID3D10Texture2D* source, GpuSlot& slot) {
    D3D10_TEXTURE2D_DESC desc{}; source->GetDesc(&desc);
    HRESULT hr = CheckSize(desc.Width, desc.Height, desc.Format);
    if (FAILED(hr)) return hr;
    ComPtr<ID3D10Device> device; source->GetDevice(&device);
    if (!slot.texture10 || slot.device10.Get() != device.Get() || slot.width != desc.Width ||
        slot.height != desc.Height || slot.format != TypedFormat(desc.Format)) {
        slot.texture11.Reset(); slot.query11.Reset(); slot.device11.Reset(); slot.context.Reset();
        slot.texture10.Reset(); slot.query10.Reset();
        ResetReadTextures(slot);
        slot.device10 = device; slot.shared = nullptr;
        ComPtr<ID3D10Multithread> protect;
        hr = device.As(&protect);
        if (FAILED(hr)) return hr;
        protect->SetMultithreadProtected(TRUE);
        auto create = desc;
        create.Format = TypedFormat(desc.Format); create.SampleDesc = {1, 0};
        create.MipLevels = create.ArraySize = 1;
        create.Usage = D3D10_USAGE_DEFAULT; create.CPUAccessFlags = 0;
        create.BindFlags = D3D10_BIND_SHADER_RESOURCE; create.MiscFlags = D3D10_RESOURCE_MISC_SHARED;
        hr = device->CreateTexture2D(&create, nullptr, &slot.texture10);
        ComPtr<IDXGIResource> shared;
        if (SUCCEEDED(hr)) hr = slot.texture10.As(&shared);
        if (SUCCEEDED(hr)) hr = shared->GetSharedHandle(&slot.shared);
        D3D10_QUERY_DESC query{D3D10_QUERY_EVENT, 0};
        if (SUCCEEDED(hr)) hr = device->CreateQuery(&query, &slot.query10);
        if (SUCCEEDED(hr)) hr = Adapter(device.Get(), slot.luid);
        if (FAILED(hr)) { slot.texture10.Reset(); return hr; }
        slot.width = desc.Width; slot.height = desc.Height; slot.format = create.Format;
    }
    if (desc.SampleDesc.Count > 1)
        device->ResolveSubresource(slot.texture10.Get(), 0, source, 0, slot.format);
    else device->CopyResource(slot.texture10.Get(), source);
    slot.query10->End();
    device->Flush();
    slot.backend = 10; slot.flip = 0;
    return S_OK;
}
} // namespace

void ResetBridge() {
    bridge.Reset(); bridge11.Reset(); bridgeQueue.Reset(); bridgeDevice.Reset();
}

HRESULT Snapshot(IDXGISwapChain* chain, ID3D12CommandQueue* queue, GpuSlot& slot) {
    ComPtr<ID3D12Device> device12;
    if (SUCCEEDED(chain->GetDevice(IID_PPV_ARGS(&device12)))) {
        if (!queue) return kQueuePending;
        if (bridgeDevice.Get() != device12.Get() || bridgeQueue.Get() != queue || !bridge) {
            ResetBridge();
            IUnknown* queues[] = {queue};
            HRESULT hr = D3D11On12CreateDevice(device12.Get(), D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                nullptr, 0, queues, 1, 0, &bridge11, nullptr, nullptr);
            if (FAILED(hr)) return hr;
            hr = bridge11.As(&bridge);
            if (FAILED(hr)) return hr;
            bridgeDevice = device12; bridgeQueue = queue;
        }
        ComPtr<IDXGISwapChain3> flip;
        HRESULT hr = chain->QueryInterface(IID_PPV_ARGS(&flip));
        ComPtr<ID3D12Resource> back;
        if (SUCCEEDED(hr)) hr = flip->GetBuffer(flip->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&back));
        ComPtr<ID3D11Texture2D> wrapped;
        D3D11_RESOURCE_FLAGS flags{};
        if (SUCCEEDED(hr)) hr = bridge->CreateWrappedResource(back.Get(), &flags,
            D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_PRESENT, IID_PPV_ARGS(&wrapped));
        if (SUCCEEDED(hr)) hr = Copy11(wrapped.Get(), slot, bridge.Get());
        // 11On12 延迟销毁包装资源。最后一个包装引用释放后再 Flush，
        // 才能解除对交换链后缓冲的内部引用，让 ResizeBuffers 正常工作。
        wrapped.Reset();
        back.Reset();
        if (slot.context) slot.context->Flush();
        slot.backend = 12; slot.flip = 0;
        return hr;
    }
    ComPtr<ID3D10Device> device10;
    if (SUCCEEDED(chain->GetDevice(IID_PPV_ARGS(&device10)))) {
        ComPtr<ID3D10Texture2D> source;
        HRESULT hr = chain->GetBuffer(0, IID_PPV_ARGS(&source));
        return FAILED(hr) ? hr : Copy10(source.Get(), slot);
    }
    ComPtr<ID3D11Texture2D> source;
    HRESULT hr = chain->GetBuffer(0, IID_PPV_ARGS(&source));
    if (SUCCEEDED(hr)) hr = Copy11(source.Get(), slot);
    slot.backend = 11; slot.flip = 0;
    return hr;
}

HRESULT SnapshotTexture(ID3D11Texture2D* source, GpuSlot& slot, uint32_t flip) {
    const HRESULT hr = Copy11(source, slot);
    slot.backend = 11; slot.flip = flip;
    return hr;
}

HRESULT SnapshotRegion(ID3D11Texture2D* source, const D3D11_BOX& region, GpuSlot& slot) {
    if (!source) return E_INVALIDARG;
    D3D11_TEXTURE2D_DESC desc{};
    source->GetDesc(&desc);
    if (region.left >= region.right || region.top >= region.bottom ||
        region.right > desc.Width || region.bottom > desc.Height ||
        region.front != 0 || region.back != 1 || desc.SampleDesc.Count != 1)
        return E_INVALIDARG;
    const HRESULT hr = Copy11(source, slot, nullptr, &region);
    slot.backend = 11; slot.flip = 0;
    return hr;
}

HRESULT SnapshotShared(IDXGISwapChain* chain, ID3D12CommandQueue* queue, HANDLE shared,
                       SharedSource& cached, GpuSlot& slot, uint32_t flip) {
    ComPtr<ID3D10Device> device10;
    if (SUCCEEDED(chain->GetDevice(IID_PPV_ARGS(&device10)))) {
        if (cached.shared!=shared || cached.device10.Get()!=device10.Get() || !cached.texture10) {
            cached={};
            HRESULT hr=device10->OpenSharedResource(shared,IID_PPV_ARGS(&cached.texture10));
            if (FAILED(hr)) return hr;
            cached.device10=device10; cached.shared=shared;
        }
        HRESULT hr = Copy10(cached.texture10.Get(), slot);
        slot.flip = flip;
        return hr;
    }
    ComPtr<ID3D11Device> device11;
    ComPtr<ID3D12Device> device12;
    const bool is12 = SUCCEEDED(chain->GetDevice(IID_PPV_ARGS(&device12)));
    if (is12) {
        if (!queue) return kQueuePending;
        if (bridgeDevice.Get() != device12.Get() || bridgeQueue.Get() != queue || !bridge) {
            ResetBridge();
            IUnknown* queues[] = {queue};
            HRESULT hr = D3D11On12CreateDevice(device12.Get(), D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                nullptr, 0, queues, 1, 0, &bridge11, nullptr, nullptr);
            if (FAILED(hr)) return hr;
            hr = bridge11.As(&bridge);
            if (FAILED(hr)) return hr;
            bridgeDevice = device12; bridgeQueue = queue;
        }
        device11 = bridge11;
    } else {
        HRESULT hr = chain->GetDevice(IID_PPV_ARGS(&device11));
        if (FAILED(hr)) return hr;
    }
    if (cached.shared!=shared || cached.device11.Get()!=device11.Get() || !cached.texture11) {
        cached={};
        HRESULT hr=device11->OpenSharedResource(shared,IID_PPV_ARGS(&cached.texture11));
        if (FAILED(hr)) return hr;
        cached.device11=device11; cached.shared=shared;
    }
    HRESULT hr = SnapshotTexture(cached.texture11.Get(), slot, flip);
    slot.backend = is12 ? 12u : 11u;
    return hr;
}

HRESULT Wait(GpuSlot& slot) {
    const ULONGLONG deadline = GetTickCount64() + 2000;
    for (;;) {
        BOOL ready = FALSE;
        HRESULT hr = slot.query11
            ? slot.context->GetData(slot.query11.Get(), &ready, sizeof(ready), D3D11_ASYNC_GETDATA_DONOTFLUSH)
            : slot.query10->GetData(&ready, sizeof(ready), D3D10_ASYNC_GETDATA_DONOTFLUSH);
        if (FAILED(hr)) return hr;
        if (hr == S_OK && ready) return S_OK;
        if (GetTickCount64() >= deadline) return HRESULT_FROM_WIN32(WAIT_TIMEOUT);
        Sleep(1);
    }
}

HRESULT Read(GpuSlot& slot, BYTE* pixels) {
    // 一个槽位的暂存资源串行使用；其他槽位仍可正常提交 GPU 快照。
    std::lock_guard lock(slot.readLock);
    HRESULT hr = Wait(slot);
    if (FAILED(hr)) return hr;
    if (slot.reader && (!SameAdapter(slot.reader->luid,slot.luid) ||
        FAILED(slot.reader->device->GetDeviceRemovedReason()))) {
        ResetReadTextures(slot); slot.reader.reset();
    }
    if (!slot.reader) {
        hr=Reader(slot.luid,slot.reader);
        if (FAILED(hr)) return hr;
    }
    // 锁顺序固定为槽位→读回设备；绝不持有采集状态锁等待 GPU。
    // 尺寸变化只清空 opened/staging/query，适配器不变时保留 reader。
    std::lock_guard contextLock(slot.reader->lock);
    auto* device=slot.reader->device.Get();
    auto* context=slot.reader->context.Get();
    if (!slot.opened) {
        hr = device->OpenSharedResource(slot.shared, IID_PPV_ARGS(&slot.opened));
        if (FAILED(hr)) return hr;
    }
    if (!slot.staging) {
        D3D11_TEXTURE2D_DESC desc{}; slot.opened->GetDesc(&desc);
        desc.BindFlags = desc.MiscFlags = 0; desc.Usage = D3D11_USAGE_STAGING;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        hr = device->CreateTexture2D(&desc, nullptr, &slot.staging);
        if (FAILED(hr)) return hr;
    }
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (!slot.readerQuery) {
        const D3D11_QUERY_DESC desc{D3D11_QUERY_EVENT, 0};
        hr = device->CreateQuery(&desc, &slot.readerQuery);
        if (FAILED(hr)) return hr;
    }
    context->CopyResource(slot.staging.Get(), slot.opened.Get());
    context->End(slot.readerQuery.Get());
    context->Flush();
    // 读回线程有超时保护；呈现线程不参与等待。Map 使用非阻塞标志，
    // 避免设备挂起时被无限卡在同步 Map 中。
    const ULONGLONG deadline = GetTickCount64() + 2000;
    do {
        BOOL ready{};
        hr = context->GetData(slot.readerQuery.Get(), &ready, sizeof(ready),
                                        D3D11_ASYNC_GETDATA_DONOTFLUSH);
        if (FAILED(hr)) return hr;
        if (hr == S_OK && ready) break;
        if (GetTickCount64() >= deadline) return HRESULT_FROM_WIN32(WAIT_TIMEOUT);
        Sleep(1);
    } while (true);
    hr = context->Map(slot.staging.Get(), 0, D3D11_MAP_READ,
                                D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
    if (FAILED(hr)) return hr;
    const Frame frame{slot.width, slot.height, slot.backend};
    if (!slot.flip) Convert(mapped.pData, mapped.RowPitch, slot.format, frame, pixels);
    else {
        const Frame row{slot.width, 1, slot.backend};
        for (UINT y = 0; y < slot.height; ++y)
            Convert(static_cast<const BYTE*>(mapped.pData) + size_t{mapped.RowPitch} * (slot.height - 1 - y),
                mapped.RowPitch, slot.format, row, pixels + size_t{slot.width} * y * 4);
    }
    context->Unmap(slot.staging.Get(), 0);
    return S_OK;
}
} // namespace replica

#ifdef ZHUATU_SHENGCHENG
namespace {
// 租约只跨 DLL 传递整数标识。登记表验证 token 和线程，避免把输入直接当指针解引用。
struct ProcessorLease {
    std::shared_ptr<replica::ReadDevice> reader;
    std::unique_lock<std::mutex> lock;
    DWORD owner=GetCurrentThreadId();
    explicit ProcessorLease(std::shared_ptr<replica::ReadDevice> value)
        : reader(std::move(value)), lock(reader->lock) {}
};
std::mutex processorLeaseLock;
auto& ProcessorLeases() {
    static auto* leases=new std::unordered_map<uint64_t,std::unique_ptr<ProcessorLease>>;
    return *leases;
}
uint64_t processorToken{};
}
HRESULT WINAPI ZhuaTuAcquireProcessor(uint32_t low,int32_t high,FsGpuChuli* output) {
    if (!output || output->size!=sizeof(*output)) return E_INVALIDARG;
    try {
        std::shared_ptr<replica::ReadDevice> reader;
        const HRESULT hr=replica::Reader(LUID{low,high},reader);
        if (FAILED(hr)) return hr;
        auto lease=std::make_unique<ProcessorLease>(reader);
        std::lock_guard guard(processorLeaseLock);
        const uint64_t token=++processorToken;
        ProcessorLeases().emplace(token,std::move(lease));
        *output={sizeof(*output),1,token,reinterpret_cast<uint64_t>(reader->device.Get()),
            reinterpret_cast<uint64_t>(reader->context.Get()),reader->serial,replica::devicesCreated.load()};
        return S_OK;
    } catch (...) { return E_OUTOFMEMORY; }
}
HRESULT WINAPI ZhuaTuReleaseProcessor(uint64_t token) {
    std::unique_ptr<ProcessorLease> lease;
    {
        std::lock_guard guard(processorLeaseLock);
        auto& leases=ProcessorLeases();
        const auto it=leases.find(token);
        if (it==leases.end() || it->second->owner!=GetCurrentThreadId()) return E_INVALIDARG;
        lease=std::move(it->second); leases.erase(it);
    }
    // 先退出登记表的锁，再释放上下文锁，避免与获取路径形成锁反转。
    lease.reset();
    return S_OK;
}
#endif
