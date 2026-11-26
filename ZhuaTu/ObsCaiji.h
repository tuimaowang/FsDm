#pragma once
#include "Huichuan.h"
// OBS 控制面只在绑定/解绑或呈现提交时访问，由 Caiji.cpp 状态锁串行保护。
namespace fsdm::obs {
int Begin(HWND window, HRESULT& detail, ULONGLONG deadline);
void End();
HRESULT Snapshot(IDXGISwapChain*, ID3D12CommandQueue*, replica::GpuSlot&, HWND);
constexpr HRESULT kWaiting = MAKE_HRESULT(SEVERITY_ERROR, FACILITY_ITF, 0x220);
constexpr HRESULT kWindowMismatch = MAKE_HRESULT(SEVERITY_ERROR, FACILITY_ITF, 0x221);
constexpr HRESULT kMemoryCapture = MAKE_HRESULT(SEVERITY_ERROR, FACILITY_ITF, 0x222);
}
