// 测试专用：创建真实的 WARP 渲染窗口和命令队列。
#pragma once
#include "Xieyi.h"
#include <d3d10_1.h>
#include <d3d11.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <cstdio>
#include <future>
#include <string>

using Microsoft::WRL::ComPtr;

namespace {
struct Failure {};
void Check(HRESULT hr, const char* operation) {
    if (SUCCEEDED(hr)) return;
    std::fprintf(stderr, "%s failed: %08lX\n", operation, hr);
    throw Failure{};
}

struct Window {
    HWND hwnd = CreateWindowExW(0, L"STATIC", L"DmDxImageSearch internal test", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        80, 80, 240, 190, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    Window() { if (!hwnd) Check(E_FAIL, "CreateWindowEx"); }
    ~Window() { DestroyWindow(hwnd); }
};

void Pump() {
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
}

DXGI_SWAP_CHAIN_DESC LegacyDesc(HWND hwnd, UINT samples = 1) {
    DXGI_SWAP_CHAIN_DESC desc{};
    desc.BufferDesc.Width = 64;
    desc.BufferDesc.Height = 48;
    desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc = {samples, 0};
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 1;
    desc.OutputWindow = hwnd;
    desc.Windowed = TRUE;
    desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    return desc;
}

struct Renderer12 {
    Window window;
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue, unrelatedQueue;
    ComPtr<ID3D12CommandAllocator> allocator, unrelatedAllocator;
    ComPtr<ID3D12GraphicsCommandList> list, unrelatedList;
    ComPtr<ID3D12DescriptorHeap> heap;
    ComPtr<ID3D12Fence> fence, unrelatedFence;
    ComPtr<IDXGISwapChain3> chain;
    replica::Handle event{CreateEventW(nullptr, FALSE, FALSE, nullptr)};
    UINT64 nextFence{};

    explicit Renderer12(bool hardware = false) {
        ComPtr<IDXGIFactory4> factory;
        ComPtr<IDXGIAdapter> warp;
        Check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "factory");
        if (!hardware) Check(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp)), "WARP adapter");
        Check(D3D12CreateDevice(warp.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)), "DX12 device");
        D3D12_COMMAND_QUEUE_DESC queueDesc{};
        Check(device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue)), "DX12 queue");
        Check(device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&unrelatedQueue)), "second DIRECT queue");
        Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)), "allocator");
        Check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(),
            nullptr, IID_PPV_ARGS(&list)), "command list");
        Check(list->Close(), "initial command list close");
        Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&unrelatedAllocator)),
            "unrelated allocator");
        Check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, unrelatedAllocator.Get(),
            nullptr, IID_PPV_ARGS(&unrelatedList)), "unrelated command list");
        Check(unrelatedList->Close(), "unrelated command list close");
        D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
        heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        heapDesc.NumDescriptors = 1;
        Check(device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&heap)), "RTV heap");
        Check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "fence");
        Check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&unrelatedFence)), "unrelated fence");
        DXGI_SWAP_CHAIN_DESC1 desc{};
        desc.Width = 64;
        desc.Height = 48;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc = {1, 0};
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = 3;
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        ComPtr<IDXGISwapChain1> initial;
        Check(factory->CreateSwapChainForHwnd(queue.Get(), window.hwnd, &desc, nullptr, nullptr, &initial),
            "DX12 swap chain");
        Check(initial.As(&chain), "swap chain3");
    }

    bool SupportsEnhancedBarriers() const {
        D3D12_FEATURE_DATA_D3D12_OPTIONS12 options{};
        return SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS12, &options, sizeof(options)))
            && options.EnhancedBarriersSupported;
    }

    void Render(const float (&rgba)[4], bool present1, bool enhanced = false) {
        ComPtr<ID3D12Resource> buffer;
        Check(chain->GetBuffer(chain->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&buffer)), "DX12 buffer");
        const auto rtv = heap->GetCPUDescriptorHandleForHeapStart();
        device->CreateRenderTargetView(buffer.Get(), nullptr, rtv);
        Check(allocator->Reset(), "allocator reset");
        Check(list->Reset(allocator.Get(), nullptr), "list reset");
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition = {buffer.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
            D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET};
        if (enhanced) {
            ComPtr<ID3D12GraphicsCommandList7> list7;
            Check(list.As(&list7), "enhanced list");
            D3D12_TEXTURE_BARRIER transition{};
            transition.SyncBefore = D3D12_BARRIER_SYNC_NONE;
            transition.SyncAfter = D3D12_BARRIER_SYNC_RENDER_TARGET;
            transition.AccessBefore = D3D12_BARRIER_ACCESS_NO_ACCESS;
            transition.AccessAfter = D3D12_BARRIER_ACCESS_RENDER_TARGET;
            transition.LayoutBefore = D3D12_BARRIER_LAYOUT_PRESENT;
            transition.LayoutAfter = D3D12_BARRIER_LAYOUT_RENDER_TARGET;
            transition.pResource = buffer.Get();
            transition.Subresources.IndexOrFirstMipLevel = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            D3D12_BARRIER_GROUP group{};
            group.Type = D3D12_BARRIER_TYPE_TEXTURE;
            group.NumBarriers = 1;
            group.pTextureBarriers = &transition;
            list7->Barrier(1, &group);
            list->ClearRenderTargetView(rtv, rgba, 0, nullptr);
            std::swap(transition.SyncBefore, transition.SyncAfter);
            std::swap(transition.AccessBefore, transition.AccessAfter);
            std::swap(transition.LayoutBefore, transition.LayoutAfter);
            list7->Barrier(1, &group);
        } else {
            list->ResourceBarrier(1, &barrier);
            list->ClearRenderTargetView(rtv, rgba, 0, nullptr);
            std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
            list->ResourceBarrier(1, &barrier);
        }
        Check(list->Close(), "list close");
        ID3D12CommandList* lists[] = {list.Get()};
        queue->ExecuteCommandLists(1, lists);
        // 无关 DIRECT 队列不能抢占已识别的后缓冲所属队列。
        ID3D12CommandList* unrelatedLists[] = {unrelatedList.Get()};
        unrelatedQueue->ExecuteCommandLists(1, unrelatedLists);
        DXGI_PRESENT_PARAMETERS params{};
        Check(present1 ? chain->Present1(0, 0, &params) : chain->Present(0, 0), "DX12 Present");
        Check(queue->Signal(fence.Get(), ++nextFence), "signal");
        Check(unrelatedQueue->Signal(unrelatedFence.Get(), nextFence), "unrelated signal");
        Check(fence->SetEventOnCompletion(nextFence, event.get()), "fence event");
        if (WaitForSingleObject(event.get(), 3000) != WAIT_OBJECT_0) Check(E_FAIL, "fence wait");
        Check(unrelatedFence->SetEventOnCompletion(nextFence, event.get()), "unrelated fence event");
        if (WaitForSingleObject(event.get(), 3000) != WAIT_OBJECT_0) Check(E_FAIL, "unrelated fence wait");
    }
};
} // namespace
