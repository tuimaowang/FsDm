#pragma once
#include "Xieyi.h"
#include "../Gonggong/Jiekou.h"
#include <d3d10_1.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <d3d11.h>
#include <memory>
#include <mutex>
#include <wrl/client.h>

namespace replica {
struct Frame {
    DWORD width{};
    DWORD height{};
    DWORD backend{};
};
// 同一显卡适配器共用一个 CPU 读回设备。mutex 保护该设备的立即上下文，
// 包括 Copy、Query、Map/Unmap，避免不同槽位并发读回时交错使用上下文。
struct ReadDevice {
    LUID luid{};
    Microsoft::WRL::ComPtr<ID3D11Device> device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
    std::mutex lock;
    uint64_t serial{};
};
// OBS 源纹理独立于快照槽位缓存；协议映射或会话变化时由 OBS 控制面清空，
// 即使系统复用了同一个句柄值，也不会误用上一个会话的源纹理。
struct SharedSource {
    HANDLE shared{};
    Microsoft::WRL::ComPtr<ID3D10Device> device10;
    Microsoft::WRL::ComPtr<ID3D10Texture2D> texture10;
    Microsoft::WRL::ComPtr<ID3D11Device> device11;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture11;
};

// 一个槽位保存一份独立 GPU 纹理。租约存在时不得重新写入或销毁。
struct GpuSlot {
    Microsoft::WRL::ComPtr<ID3D10Device> device10;
    Microsoft::WRL::ComPtr<ID3D10Texture2D> texture10;
    Microsoft::WRL::ComPtr<ID3D10Query> query10;
    Microsoft::WRL::ComPtr<ID3D11Device> device11;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
    std::shared_ptr<ReadDevice> reader;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture11, opened, staging;
    Microsoft::WRL::ComPtr<ID3D11Query> query11, readerQuery;
    HANDLE shared{};
    DXGI_FORMAT format{};
    LUID luid{};
    uint32_t width{}, height{}, backend{}, users{}, flip{};
    FsZhen frame{};
    std::mutex readLock;
};
// 仅 GPU 复制，呈现线程不 Map、不等待完成；Wait/Read 由读取线程调用。
HRESULT Snapshot(IDXGISwapChain*, ID3D12CommandQueue*, GpuSlot&);
HRESULT SnapshotTexture(ID3D11Texture2D*, GpuSlot&, uint32_t flip);
// WGC 的源纹理包含非客户区；直接在 GPU 上复制客户区矩形，不分配中间 CPU 图像。
HRESULT SnapshotRegion(ID3D11Texture2D*, const D3D11_BOX&, GpuSlot&);
HRESULT SnapshotShared(IDXGISwapChain*, ID3D12CommandQueue*, HANDLE, SharedSource&, GpuSlot&, uint32_t flip);
HRESULT Wait(GpuSlot&);
HRESULT Read(GpuSlot&, BYTE*);
void ResetBridge();
}
