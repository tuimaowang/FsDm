#pragma once
#include <windows.h>
#include <cstdint>

// WGC 在专用 MTA 线程上运行；MTA 是允许系统在多个线程间调用对象的 COM 运行方式。
// Begin 仅等待会话初始化，不等首帧；End 立即撤销请求，实际资源由工作线程关闭。
namespace fsdm::wgc {
int Begin(HWND window, uint64_t version, HRESULT& detail, ULONGLONG deadline);
void End();
}
