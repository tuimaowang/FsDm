#pragma once
#include "ZhuaTu.h"
#include <atomic>
struct IDXGISwapChain;
struct ID3D12CommandQueue;
struct ID3D11Texture2D;
struct D3D11_BOX;
// 原子快照只用于提前跳过无绑定的呈现。真正发布/读取必须在状态锁中再次核对版本。
namespace fsdm {
extern std::atomic<uint64_t> generation;
extern std::atomic<HWND> target;
extern std::atomic<LONG> startup;
extern std::atomic<LONG> startupDetail;
extern std::atomic<uint32_t> captureBackend;
// 每个有效输入帧调用一次；固定帧时返回 0，其他情况按跳过帧数决定是否采集。
int SelectInputFrame(uint64_t version);
void Submit(uint64_t version, IDXGISwapChain*, ID3D12CommandQueue*, HRESULT hr, double elapsedMs);
// WGC 工作线程也经过同一个版本检查和三槽池；错误时 texture/region 可以为空。
bool SubmitWgc(uint64_t version, ID3D11Texture2D*, const D3D11_BOX*, HRESULT hr, double elapsedMs);
void Monitor();
}
