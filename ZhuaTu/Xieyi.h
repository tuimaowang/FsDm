#pragma once
#include <windows.h>
#include <cstdint>
#include <cwchar>
#include <utility>
namespace replica {
constexpr size_t kPixelCapacity = 64 * 1024 * 1024;
constexpr HRESULT kQueuePending = MAKE_HRESULT(SEVERITY_ERROR, FACILITY_ITF, 0x201);
constexpr HRESULT kStartupProbe = MAKE_HRESULT(SEVERITY_ERROR, FACILITY_ITF, 0x202);
constexpr HRESULT kStartupHooks = MAKE_HRESULT(SEVERITY_ERROR, FACILITY_ITF, 0x203);
constexpr HRESULT kReadbackPending = MAKE_HRESULT(SEVERITY_ERROR, FACILITY_ITF, 0x204);
constexpr HRESULT kQueueProbePending = MAKE_HRESULT(SEVERITY_ERROR, FACILITY_ITF, 0x205);
constexpr HRESULT kQueueProbeFailed = MAKE_HRESULT(SEVERITY_ERROR, FACILITY_ITF, 0x206);
constexpr HRESULT kQueueNoSubmit = MAKE_HRESULT(SEVERITY_ERROR, FACILITY_ITF, 0x207);
constexpr HRESULT kQueueNoBarrier = MAKE_HRESULT(SEVERITY_ERROR, FACILITY_ITF, 0x208);

class Handle {
public:
    explicit Handle(HANDLE handle = nullptr) noexcept : handle_(handle) {}
    ~Handle() { if (handle_ && handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    Handle(Handle&& other) noexcept : handle_(std::exchange(other.handle_, nullptr)) {}
    HANDLE get() const noexcept { return handle_; }
    explicit operator bool() const noexcept { return handle_ && handle_ != INVALID_HANDLE_VALUE; }
private:
    HANDLE handle_;
};

}
