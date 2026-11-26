// 呈现拦截、DX12 队列识别和尺寸变更处理；改编自本地参考项目。
#include "Huichuan.h"
#include "Neibu.h"
#include "Peizhi.h"
#include "../Gonggong/Gongzuotai.h"
#include <d3d11.h>
#include <detours.h>
#include <tlhelp32.h>
#include <wrl/client.h>
#include <array>
#include <atomic>
#include <chrono>
#include <vector>

using Microsoft::WRL::ComPtr;
using namespace replica;

namespace {

// 日志开关在编译时决定，关闭时连控制台句柄也不打开。
class StartupLog {
    Handle output_{FsOpenConsole(kGongzuotaiKaiqi)};
public:
    void Line(const wchar_t* message) const {
        if constexpr (kGongzuotaiKaiqi) FsConsoleLine(output_.get(), message);
    }
    void Result(const wchar_t* message, HRESULT hr) const {
        wchar_t text[320]; swprintf_s(text, L"[ZhuaTu] %ls HRESULT=0x%08lX", message, hr); Line(text);
    }
};

using Present = HRESULT (STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using Present1 = HRESULT (STDMETHODCALLTYPE*)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
using Resize = HRESULT (STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
using Resize1 = HRESULT (STDMETHODCALLTYPE*)(IDXGISwapChain3*, UINT, UINT, UINT, DXGI_FORMAT, UINT,
                                            const UINT*, IUnknown* const*);
using CreateForHwnd = HRESULT (STDMETHODCALLTYPE*)(IDXGIFactory2*, IUnknown*, HWND,
    const DXGI_SWAP_CHAIN_DESC1*, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*, IDXGISwapChain1**);
using ResetList = HRESULT (STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12CommandAllocator*, ID3D12PipelineState*);
using ResourceBarrier = void (STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, const D3D12_RESOURCE_BARRIER*);
using EnhancedBarrier = void (STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList7*, UINT, const D3D12_BARRIER_GROUP*);
using Execute = void (STDMETHODCALLTYPE*)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);

Present realPresent[2]{};
Present1 realPresent1[2]{};
Resize realResize[2]{};
Resize1 realResize1{};
CreateForHwnd realCreateForHwnd{};
struct QueueMethods {
    ResetList reset{};
    ResourceBarrier barrier{};
    EnhancedBarrier enhanced{};
    Execute execute{};
};
constexpr size_t kRuntimeSlots = 2;
QueueMethods queueOriginal[kRuntimeSlots]{};
QueueMethods queueEntries[kRuntimeSlots]{};
struct RuntimeProbe {
    std::atomic<const void*> key{};
    HRESULT result{};
};
std::array<RuntimeProbe, kRuntimeSlots> runtimeProbes{};
std::atomic<ID3D12Device*> pendingDevice{};
std::atomic_bool probingDevice{};
HANDLE probeEvent{};

// captureLock 串行化呈现与尺寸变更；GPU 快照池另外由 Caiji.cpp 的状态锁保护。

thread_local bool inPresent{};
thread_local bool inResize{};
SRWLOCK captureLock = SRWLOCK_INIT;
SRWLOCK observationLock = SRWLOCK_INIT;

constexpr GUID queueKey = {0xa431bd19, 0x2a70, 0x4711, {0x85, 0x45, 0x53, 0x86, 0xae, 0xac, 0x20, 0x0c}};
constexpr size_t maxBuffers = DXGI_MAX_SWAP_CHAIN_BUFFERS;
struct ListMark { const void* list{}; unsigned buffers{}; };
std::array<ListMark, 512> listMarks{};
std::array<ID3D12Resource*, maxBuffers> observedBuffers{};
std::array<ComPtr<ID3D12CommandQueue>, maxBuffers> observedQueues{};
IDXGISwapChain* observedChain{};
std::atomic<LONG64> observedRequest{};
bool sawPresentBarrier{};
bool sawDirectSubmit{};

class ExclusiveLock {
public:
    explicit ExclusiveLock(SRWLOCK& lock) : lock_(lock) { AcquireSRWLockExclusive(&lock_); }
    ~ExclusiveLock() { ReleaseSRWLockExclusive(&lock_); }
private:
    SRWLOCK& lock_;
};

void ClearObservation() {
    observedRequest.store(0);
    observedChain = nullptr;
    observedBuffers.fill(nullptr);
    listMarks.fill({});
    sawPresentBarrier = false;
    sawDirectSubmit = false;
    for (auto& queue : observedQueues) queue.Reset();
}

HRESULT ProbeDeviceWhenNeeded(ID3D12Device* device) {
    if (!probeEvent) return kQueueProbeFailed;
    const auto key = *reinterpret_cast<void* const*>(device);
    bool free = false;
    for (const auto& probe : runtimeProbes) {
        const auto probed = probe.key.load();
        if (probed == key) return SUCCEEDED(probe.result) ? S_OK : kQueueProbeFailed;
        free |= !probed;
    }
    if (!free) return kQueueProbeFailed;
    if (!probingDevice.exchange(true)) {
        device->AddRef();
        pendingDevice.store(device);
        SetEvent(probeEvent);
    }
    return kQueueProbePending;
}

ComPtr<ID3D12CommandQueue> FindQueue(IDXGISwapChain* chain, ID3D12Device* device, LONG64 request,
                                    HRESULT& detail) {
    ComPtr<IDXGISwapChain3> flip;
    if (FAILED(chain->QueryInterface(IID_PPV_ARGS(&flip)))) return {};
    const UINT index = flip->GetCurrentBackBufferIndex();
    ComPtr<ID3D12Resource> back;
    if (index >= maxBuffers || FAILED(flip->GetBuffer(index, IID_PPV_ARGS(&back)))) return {};
    ComPtr<IUnknown> saved;
    UINT bytes = sizeof(IUnknown*);
    if (FAILED(back->GetPrivateData(queueKey, &bytes, saved.GetAddressOf()))) {
        bytes = sizeof(IUnknown*);
        flip->GetPrivateData(queueKey, &bytes, saved.ReleaseAndGetAddressOf());
    }
    ComPtr<ID3D12CommandQueue> queue;
    if (saved && SUCCEEDED(saved.As(&queue))) return queue;

    const HRESULT probe = ProbeDeviceWhenNeeded(device);
    if (FAILED(probe)) {
        detail = probe;
        return {};
    }
    {
        ExclusiveLock lock(observationLock);
        if (observedRequest.load() == request && observedChain == chain &&
            observedBuffers[index] == back.Get()) {
            if (!observedQueues[index]) {
                if (!sawDirectSubmit) detail = kQueueNoSubmit;
                else if (!sawPresentBarrier) detail = kQueueNoBarrier;
            }
            return observedQueues[index];
        }
    }
    DXGI_SWAP_CHAIN_DESC1 desc{};
    if (FAILED(flip->GetDesc1(&desc)) || desc.BufferCount > maxBuffers) return {};
    std::array<ID3D12Resource*, maxBuffers> buffers{};
    for (UINT i = 0; i < desc.BufferCount; ++i) {
        ComPtr<ID3D12Resource> resource;
        if (FAILED(flip->GetBuffer(i, IID_PPV_ARGS(&resource)))) return {};
        buffers[i] = resource.Get();
    }
    ExclusiveLock lock(observationLock);
    ClearObservation();
    observedBuffers = buffers;
    observedChain = chain;
    observedRequest.store(request);
    return {};
}

bool Watching() {
    const auto watching = observedRequest.load();
    return watching && !inPresent && static_cast<uint64_t>(watching) == fsdm::generation.load()
           && fsdm::target.load();
}

unsigned BufferBit(ID3D12Resource* resource) {
    if (!resource) return 0;
    for (size_t i = 0; i < observedBuffers.size(); ++i)
        if (observedBuffers[i] == resource) return 1u << i;
    return 0;
}

void Mark(const void* list, unsigned touched, unsigned presented) {
    if (!touched) return;
    sawPresentBarrier |= presented != 0;
    ListMark* free = nullptr;
    for (auto& entry : listMarks) {
        if (entry.list == list) {
            entry.buffers = (entry.buffers & ~touched) | presented;
            if (!entry.buffers) entry = {};
            return;
        }
        if (!entry.list && !free) free = &entry;
    }
    if (free && presented) *free = {list, presented};
}

template<size_t Slot>
void STDMETHODCALLTYPE OnBarrier(ID3D12GraphicsCommandList* list, UINT count,
                                 const D3D12_RESOURCE_BARRIER* barriers) {
    queueOriginal[Slot].barrier(list, count, barriers);
    if (!barriers || !Watching()) return;
    ExclusiveLock lock(observationLock);
    unsigned touched = 0, presented = 0;
    for (UINT i = 0; i < count; ++i) {
        const auto& barrier = barriers[i];
        if (barrier.Type != D3D12_RESOURCE_BARRIER_TYPE_TRANSITION ||
            barrier.Flags == D3D12_RESOURCE_BARRIER_FLAG_BEGIN_ONLY) continue;
        const unsigned bit = BufferBit(barrier.Transition.pResource);
        touched |= bit;
        presented &= ~bit;
        if (barrier.Transition.StateAfter == D3D12_RESOURCE_STATE_PRESENT) presented |= bit;
    }
    Mark(static_cast<ID3D12CommandList*>(list), touched, presented);
}

template<size_t Slot>
void STDMETHODCALLTYPE OnEnhancedBarrier(ID3D12GraphicsCommandList7* list, UINT count,
                                         const D3D12_BARRIER_GROUP* groups) {
    queueOriginal[Slot].enhanced(list, count, groups);
    if (!groups || !Watching()) return;
    ExclusiveLock lock(observationLock);
    unsigned touched = 0, presented = 0;
    for (UINT i = 0; i < count; ++i) {
        const auto& group = groups[i];
        if (group.Type != D3D12_BARRIER_TYPE_TEXTURE) continue;
        for (UINT j = 0; j < group.NumBarriers; ++j) {
            const auto& barrier = group.pTextureBarriers[j];
            if (barrier.SyncAfter == D3D12_BARRIER_SYNC_SPLIT) continue;
            const unsigned bit = BufferBit(barrier.pResource);
            touched |= bit;
            presented &= ~bit;
            if (barrier.LayoutAfter == D3D12_BARRIER_LAYOUT_PRESENT) presented |= bit;
        }
    }
    Mark(static_cast<ID3D12CommandList*>(list), touched, presented);
}

template<size_t Slot>
HRESULT STDMETHODCALLTYPE OnReset(ID3D12GraphicsCommandList* list,
                                  ID3D12CommandAllocator* allocator, ID3D12PipelineState* pso) {
    const HRESULT hr = queueOriginal[Slot].reset(list, allocator, pso);
    if (SUCCEEDED(hr) && Watching()) {
        ExclusiveLock lock(observationLock);
        for (auto& entry : listMarks) {
            if (entry.list == static_cast<ID3D12CommandList*>(list)) {
                entry = {};
                break;
            }
        }
    }
    return hr;
}

template<size_t Slot>
void STDMETHODCALLTYPE OnExecute(ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* const* lists) {
    unsigned buffers = 0;
    LONG64 watching = 0;
    if (lists && Watching() && queue->GetDesc().Type == D3D12_COMMAND_LIST_TYPE_DIRECT) {
        ExclusiveLock lock(observationLock);
        watching = observedRequest.load();
        sawDirectSubmit = true;
        for (UINT i = 0; i < count; ++i) {
            for (const auto& entry : listMarks) {
                if (entry.list && entry.list == lists[i]) {
                    buffers |= entry.buffers;
                    break;
                }
            }
        }
    }
    queueOriginal[Slot].execute(queue, count, lists);
    if (buffers) {
        ExclusiveLock lock(observationLock);
        if (observedRequest.load() != watching) return;
        for (size_t i = 0; i < observedQueues.size(); ++i)
            if (buffers & (1u << i)) observedQueues[i] = queue;
    }
}

HRESULT STDMETHODCALLTYPE OnCreateForHwnd(IDXGIFactory2* factory, IUnknown* device, HWND window,
    const DXGI_SWAP_CHAIN_DESC1* desc, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreen,
    IDXGIOutput* output, IDXGISwapChain1** chain) {
    const HRESULT hr = realCreateForHwnd(factory, device, window, desc, fullscreen, output, chain);
    ComPtr<ID3D12CommandQueue> queue;
    if (SUCCEEDED(hr) && chain && *chain && device &&
        SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&queue))))
        (*chain)->SetPrivateDataInterface(queueKey, queue.Get());
    return hr;
}

void BeforeResize(IDXGISwapChain* chain) {
    ExclusiveLock lock(observationLock);
    if (observedChain == chain) ClearObservation();
}

template<size_t Slot>
HRESULT STDMETHODCALLTYPE OnResize(IDXGISwapChain* chain, UINT count, UINT w, UINT h, DXGI_FORMAT fmt, UINT flags) {
    if (inResize) return realResize[Slot](chain, count, w, h, fmt, flags);
    ExclusiveLock lock(captureLock);
    inResize = true;
    BeforeResize(chain);
    const HRESULT hr = realResize[Slot](chain, count, w, h, fmt, flags);
    inResize = false;
    return hr;
}

HRESULT STDMETHODCALLTYPE OnResize1(IDXGISwapChain3* chain, UINT count, UINT w, UINT h, DXGI_FORMAT fmt,
                                     UINT flags, const UINT* masks, IUnknown* const* queues) {
    if (inResize) return realResize1(chain, count, w, h, fmt, flags, masks, queues);
    ExclusiveLock lock(captureLock);
    inResize = true;
    BeforeResize(chain);
    const HRESULT hr = realResize1(chain, count, w, h, fmt, flags, masks, queues);
    if (SUCCEEDED(hr) && queues && count) {
        chain->SetPrivateDataInterface(queueKey, nullptr);
        DXGI_SWAP_CHAIN_DESC1 desc{};
        if (SUCCEEDED(chain->GetDesc1(&desc))) {
            for (UINT i = 0; i < desc.BufferCount && i < count; ++i) {
                ComPtr<ID3D12Resource> back;
                if (SUCCEEDED(chain->GetBuffer(i, IID_PPV_ARGS(&back))))
                    back->SetPrivateDataInterface(queueKey, queues[i]);
            }
        }
    }
    inResize = false;
    return hr;
}

// 绑定窗口呈现时只提交 GPU 复制；按需读取由 CopyFrame 调用线程负责。
void Capture(IDXGISwapChain* chain, UINT flags) {
    if (inResize || (flags & DXGI_PRESENT_TEST) || fsdm::startup.load() != 1) return;
    // WGC 的帧来自系统合成器，不能再从 Present 提交第二份快照或探测 DX12 队列。
    if (fsdm::captureBackend.load() == FS_CAIJI_WGC) return;
    const uint64_t version = fsdm::generation.load();
    HWND window = fsdm::target.load();
    if (!window) return;
    DXGI_SWAP_CHAIN_DESC desc{};
    if (FAILED(chain->GetDesc(&desc)) || desc.OutputWindow != window) return;
    // 按跳过帧数筛选 Present；-1 固定快照时在探测设备和复制纹理前直接跳过。
    if (!fsdm::SelectInputFrame(version)) return;
    if (!TryAcquireSRWLockExclusive(&captureLock)) return;
    struct Unlock { ~Unlock() { ReleaseSRWLockExclusive(&captureLock); } } unlock;
    if (version != fsdm::generation.load() || window != fsdm::target.load()) return;
    const auto started = std::chrono::steady_clock::now();
    HRESULT hr = S_OK;
    ComPtr<ID3D12CommandQueue> queue;
    try {
        if (desc.Flags & (DXGI_SWAP_CHAIN_FLAG_RESTRICTED_CONTENT | DXGI_SWAP_CHAIN_FLAG_HW_PROTECTED |
                          DXGI_SWAP_CHAIN_FLAG_DISPLAY_ONLY)) hr = E_ACCESSDENIED;
        else {
            ComPtr<ID3D12Device> device;
            if (SUCCEEDED(chain->GetDevice(IID_PPV_ARGS(&device)))) {
                hr = kQueuePending;
                queue = FindQueue(chain, device.Get(), static_cast<LONG64>(version), hr);
                if (queue) hr = S_OK;
            }
        }
    } catch (...) { hr = E_OUTOFMEMORY; }
    const double elapsed = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - started).count();
    fsdm::Submit(version, chain, queue.Get(), hr, elapsed);
}

template<size_t Slot>
HRESULT STDMETHODCALLTYPE OnPresent(IDXGISwapChain* chain, UINT interval, UINT flags) {
    if (inPresent) return realPresent[Slot](chain, interval, flags);
    inPresent = true;
    Capture(chain, flags);
    const HRESULT hr = realPresent[Slot](chain, interval, flags);
    inPresent = false;
    return hr;
}

template<size_t Slot>
HRESULT STDMETHODCALLTYPE OnPresent1(IDXGISwapChain1* chain, UINT interval, UINT flags,
                                     const DXGI_PRESENT_PARAMETERS* params) {
    if (inPresent) return realPresent1[Slot](chain, interval, flags, params);
    inPresent = true;
    Capture(chain, flags);
    const HRESULT hr = realPresent1[Slot](chain, interval, flags, params);
    inPresent = false;
    return hr;
}

template<typename Function, typename Interface>
Function Method(Interface* object, size_t index) {
    return reinterpret_cast<Function>((*reinterpret_cast<void***>(object))[index]);
}

template<typename Function>
LONG Attach(Function& original, Function replacement) {
    return DetourAttach(reinterpret_cast<PVOID*>(&original), reinterpret_cast<PVOID>(replacement));
}

template<typename Function>
LONG AttachDistinct(Function (&original)[2], Function first, Function second) {
    const bool sameTarget = DetourCodeFromPointer(reinterpret_cast<PVOID>(original[0]), nullptr) ==
                            DetourCodeFromPointer(reinterpret_cast<PVOID>(original[1]), nullptr);
    LONG error = Attach(original[0], first);
    if (!error && !sameTarget) error = Attach(original[1], second);
    return error;
}

HRESULT ProbeHooks(const StartupLog& log) {
    log.Line(L"创建 DirectX 探测窗口");
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    HWND window = CreateWindowExW(0, L"STATIC", L"", WS_POPUP, 0, 0, 32, 32,
                                  nullptr, nullptr, instance, nullptr);
    HWND window11 = CreateWindowExW(0, L"STATIC", L"", WS_POPUP, 0, 0, 32, 32,
                                    nullptr, nullptr, instance, nullptr);
    if (!window || !window11) {
        const HRESULT hr = HRESULT_FROM_WIN32(GetLastError());
        if (window) DestroyWindow(window);
        if (window11) DestroyWindow(window11);
        return FAILED(hr) ? hr : E_FAIL;
    }
    struct Windows {
        HWND a, b;
        ~Windows() { DestroyWindow(a); DestroyWindow(b); }
    } windows{window, window11};

    log.Line(L"创建 DXGI 工厂");
    ComPtr<IDXGIFactory4> factory;
    HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    if (FAILED(hr)) return hr;
    ComPtr<IDXGIAdapter> warp;
    hr = factory->EnumWarpAdapter(IID_PPV_ARGS(&warp));
    if (FAILED(hr)) return hr;
    log.Line(L"探测 D3D12 设备");
    ComPtr<ID3D12Device> device;
    hr = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device));
    if (FAILED(hr)) {
        log.Result(L"D3D12 硬件设备不可用，改用 WARP", hr);
        hr = D3D12CreateDevice(warp.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device));
    }
    if (FAILED(hr)) return hr;
    ComPtr<ID3D12CommandQueue> queue;
    D3D12_COMMAND_QUEUE_DESC queueDesc{};
    hr = device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue));
    if (FAILED(hr)) return hr;
    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width = 32;
    desc.Height = 32;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc = {1, 0};
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    log.Line(L"探测 D3D12 交换链");
    ComPtr<IDXGISwapChain1> chain12, chain11;
    hr = factory->CreateSwapChainForHwnd(queue.Get(), window, &desc, nullptr, nullptr, &chain12);
    if (FAILED(hr)) return hr;
    ComPtr<IDXGISwapChain3> chain3;
    hr = chain12.As(&chain3);
    if (FAILED(hr)) return hr;
    log.Line(L"探测 D3D11 设备");
    ComPtr<ID3D11Device> device11;
    hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
                           D3D11_SDK_VERSION, &device11, nullptr, nullptr);
    if (FAILED(hr)) {
        log.Result(L"D3D11 硬件设备不可用，改用 WARP", hr);
        hr = D3D11CreateDevice(warp.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0,
            nullptr, 0, D3D11_SDK_VERSION, &device11, nullptr, nullptr);
    }
    if (FAILED(hr)) return hr;
    log.Line(L"探测 D3D11 交换链");
    hr = factory->CreateSwapChainForHwnd(device11.Get(), window11, &desc, nullptr, nullptr, &chain11);
    if (FAILED(hr)) return hr;
    realPresent[0] = Method<Present>(static_cast<IDXGISwapChain*>(chain12.Get()), 8);
    realPresent[1] = Method<Present>(static_cast<IDXGISwapChain*>(chain11.Get()), 8);
    realPresent1[0] = Method<Present1>(chain12.Get(), 22);
    realPresent1[1] = Method<Present1>(chain11.Get(), 22);
    realResize[0] = Method<Resize>(static_cast<IDXGISwapChain*>(chain12.Get()), 13);
    realResize[1] = Method<Resize>(static_cast<IDXGISwapChain*>(chain11.Get()), 13);
    realResize1 = Method<Resize1>(chain3.Get(), 39);
    realCreateForHwnd = Method<CreateForHwnd>(static_cast<IDXGIFactory2*>(factory.Get()), 15);
    return S_OK;
}

template<typename AttachHooks>
HRESULT CommitHooks(const StartupLog& log, AttachHooks attachHooks) {
    const DWORD pid = GetCurrentProcessId(), self = GetCurrentThreadId();
    Handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0));
    if (!snapshot) return HRESULT_FROM_WIN32(GetLastError());
    std::vector<Handle> threads;
    THREADENTRY32 entry{};
    entry.dwSize = sizeof(entry);
    for (BOOL more = Thread32First(snapshot.get(), &entry); more; more = Thread32Next(snapshot.get(), &entry)) {
        if (entry.th32OwnerProcessID != pid || entry.th32ThreadID == self) continue;
        Handle thread(OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_SET_CONTEXT |
                                 SYNCHRONIZE, FALSE, entry.th32ThreadID));
        if (thread) threads.push_back(std::move(thread));
        else if (GetLastError() != ERROR_INVALID_PARAMETER) return HRESULT_FROM_WIN32(GetLastError());
    }

    log.Line(L"开始 Detours 事务");
    LONG err = DetourTransactionBegin();
    if (err) return HRESULT_FROM_WIN32(err);
    err = attachHooks();
    if (!err) log.Line(L"提交 Detours 事务，更新目标线程");
    for (const auto& thread : threads) {
        if (err) break;
        if (WaitForSingleObject(thread.get(), 0) == WAIT_TIMEOUT) {
            err = DetourUpdateThread(thread.get());
            if (err && WaitForSingleObject(thread.get(), 0) == WAIT_OBJECT_0) err = 0;
        }
    }
    if (err) DetourTransactionAbort();
    else err = DetourTransactionCommit();
    return err ? HRESULT_FROM_WIN32(err) : S_OK;
}

HRESULT Install(const StartupLog& log) {
    fsdm::startupDetail.store(kStartupProbe);
    const HRESULT hr = ProbeHooks(log); //探测函数 获取函数地址
    if (FAILED(hr)) return hr;
    fsdm::startupDetail.store(kStartupHooks);
    log.Line(L"DirectX 探测完成，准备安装 Detours Hook");
    return CommitHooks(log, [] { //提交钩子
        LONG err = AttachDistinct(realPresent, OnPresent<0>, OnPresent<1>);
        if (!err) err = AttachDistinct(realPresent1, OnPresent1<0>, OnPresent1<1>);
        if (!err) err = AttachDistinct(realResize, OnResize<0>, OnResize<1>);
        if (!err) err = Attach(realResize1, OnResize1);
        if (!err) err = Attach(realCreateForHwnd, OnCreateForHwnd);
        return err;
    });
}

HRESULT ReadQueueEntries(ID3D12Device* device, QueueMethods& methods) {
    ComPtr<ID3D12CommandQueue> queue;
    D3D12_COMMAND_QUEUE_DESC queueDesc{};
    HRESULT hr = device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue));
    if (FAILED(hr)) return hr;
    ComPtr<ID3D12CommandAllocator> allocator;
    hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator));
    if (FAILED(hr)) return hr;
    ComPtr<ID3D12GraphicsCommandList> list;
    hr = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                   IID_PPV_ARGS(&list));
    if (FAILED(hr)) return hr;
    ComPtr<ID3D12GraphicsCommandList7> list7;
    list.As(&list7);
    methods = {Method<ResetList>(list.Get(), 10), Method<ResourceBarrier>(list.Get(), 26),
               list7 ? Method<EnhancedBarrier>(list7.Get(), 80) : nullptr, Method<Execute>(queue.Get(), 10)};
    return list->Close();
}

template<typename MethodType>
bool NeedsAttach(MethodType candidate, MethodType existing) {
    return candidate && (!existing ||
           DetourCodeFromPointer(reinterpret_cast<PVOID>(candidate), nullptr) !=
           DetourCodeFromPointer(reinterpret_cast<PVOID>(existing), nullptr));
}

template<size_t Slot>
HRESULT InstallQueueHooks(ID3D12Device* device, const StartupLog& log) {
    QueueMethods entries{};
    const HRESULT hr = ReadQueueEntries(device, entries);
    if (FAILED(hr)) return hr;
    const QueueMethods previous = Slot != 0 && SUCCEEDED(runtimeProbes[0].result) ? queueEntries[0] : QueueMethods{};
    const bool reset = NeedsAttach(entries.reset, previous.reset);
    const bool barrier = NeedsAttach(entries.barrier, previous.barrier);
    const bool enhanced = NeedsAttach(entries.enhanced, previous.enhanced);
    const bool execute = NeedsAttach(entries.execute, previous.execute);
    queueEntries[Slot] = entries;
    if (!reset && !barrier && !enhanced && !execute) return S_OK;
    auto& original = queueOriginal[Slot];
    original = entries;
    return CommitHooks(log, [&] {
        LONG err = reset ? Attach(original.reset, OnReset<Slot>) : 0;
        if (!err && barrier) err = Attach(original.barrier, OnBarrier<Slot>);
        if (!err && enhanced) err = Attach(original.enhanced, OnEnhancedBarrier<Slot>);
        if (!err && execute) err = Attach(original.execute, OnExecute<Slot>);
        return err;
    });
}

void WaitForDeviceProbes(const StartupLog& log) {
    while (true) {
        fsdm::Monitor();
        // 解绑不阻塞 GPU；在后台取得采集锁后释放旧绑定的队列引用。
        if (TryAcquireSRWLockExclusive(&captureLock)) {
            {
                ExclusiveLock lock(observationLock);
                if (!fsdm::target.load() ||
                    static_cast<uint64_t>(observedRequest.load()) != fsdm::generation.load())
                    ClearObservation();
            }
            ReleaseSRWLockExclusive(&captureLock);
        }
        if (WaitForSingleObject(probeEvent, 100) != WAIT_OBJECT_0) continue;
        ComPtr<ID3D12Device> device;
        device.Attach(pendingDevice.exchange(nullptr));
        if (device) {
            const auto key = *reinterpret_cast<void* const*>(device.Get());
            for (size_t slot = 0; slot < runtimeProbes.size(); ++slot) {
                auto& probe = runtimeProbes[slot];
                const auto known = probe.key.load();
                if (known == key) break;
                if (known) continue;
                log.Line(L"从目标交换链的实际 D3D12 设备探测命令队列接口");
                HRESULT hr = E_OUTOFMEMORY;
                try {
                    hr = slot == 0 ? InstallQueueHooks<0>(device.Get(), log) : InstallQueueHooks<1>(device.Get(), log);
                } catch (...) {}
                probe.result = hr;
                probe.key.store(key);
                if (FAILED(hr)) log.Result(L"目标 D3D12 队列 Hook 安装失败", hr);
                else log.Line(L"目标 D3D12 队列 Hook 已就绪，开始匹配后缓冲");
                break;
            }
        }
        probingDevice.store(false);
    }
}

//当前函数 会获取关键函数地址并上钩子
DWORD WINAPI Start(void*) {
    const StartupLog log; //日志对象用于输出控制台日志 
    HRESULT hr = E_OUTOFMEMORY;
    try {
        probeEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr); //创建事件
        if (!probeEvent) hr = HRESULT_FROM_WIN32(GetLastError()); //创建失败则获取错误码
        else hr = Install(log);
    } catch (...) {}
    fsdm::startupDetail.store(hr);
    fsdm::startup.store(FAILED(hr) ? -1 : 1);
    if (FAILED(hr)) { log.Result(L"初始化失败", hr); return 0; }
    log.Line(L"[ZhuaTu] 拦截已就绪；绑定之后才开始采集，键鼠操作尚未实现。");
    WaitForDeviceProbes(log);
    return 0;
}
} // namespace

BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        // 静态 C 运行库和线程局部变量可能需要线程通知，保留系统默认行为。
        // 拦截函数必须在进程生命周期内保持有效；不支持中途卸载 DLL。
        HMODULE pinned{};
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,//固定模块
                               reinterpret_cast<LPCWSTR>(&Start), &pinned)) return FALSE;
        HANDLE worker = CreateThread(nullptr, 0, Start, nullptr, 0, nullptr);
        if (!worker) return FALSE;
        CloseHandle(worker);
    }
    return TRUE;
}
