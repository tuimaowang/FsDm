// 用普通 GDI 窗口验证 WGC：目标没有 DirectX 交换链，仍应得到客户区 GPU 纹理。
#include "../Gonggong/Jiekou.h"
#include "Xuanran.h"
#include "XianKaYanzheng.h"
#include <dwmapi.h>
#include <filesystem>
#include <vector>
#include <thread>
#include <cstring>

namespace {
FsBindFn bindWindow{};
FsUnbindFn unbindWindow{};
using IntervalFn = int (WINAPI*)(int);
IntervalFn setInterval{};
FsCopyFn copyFrame{};
FsStatusFn getStatus{};
FsCaptureStatusFn getExtra{};
FsGpuAcquireFn acquireGpu{};
FsGpuReleaseFn releaseGpu{};
FsMapFn mapPoint{};
bool swapped{};
void Require(bool ok, const char* message) {
    if (!ok) { std::fprintf(stderr, "FAIL WGC: %s\n", message); throw Failure{}; }
}
LRESULT CALLBACK PaintWindow(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_PAINT) {
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(window, &paint);
        RECT client{}; GetClientRect(window, &client);
        RECT upper = client, lower = client;
        upper.bottom = lower.top = client.bottom / 2;
        HBRUSH red = CreateSolidBrush(RGB(255, 0, 0)), blue = CreateSolidBrush(RGB(0, 0, 255));
        FillRect(dc, &upper, swapped ? blue : red);
        FillRect(dc, &lower, swapped ? red : blue);
        DeleteObject(red); DeleteObject(blue);
        EndPaint(window, &paint);
        return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}
struct PaintTarget {
    HWND window{};
    PaintTarget() {
        WNDCLASSW type{};
        type.lpfnWndProc = PaintWindow; type.hInstance = GetModuleHandleW(nullptr);
        type.lpszClassName = L"FsDm.Wgc.Ceshi";
        Require(RegisterClassW(&type) != 0, "register paint class");
        window = CreateWindowExW(0, type.lpszClassName, L"FsDm WGC internal test",
            WS_OVERLAPPEDWINDOW | WS_VISIBLE, 480, 280, 264, 217,
            nullptr, nullptr, type.hInstance, nullptr);
        Require(window != nullptr, "create paint window");
        SetWindowPos(window, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        Paint(false);
    }
    ~PaintTarget() { if (window) DestroyWindow(window); }
    void Paint(bool value) {
        swapped = value;
        InvalidateRect(window, nullptr, FALSE); UpdateWindow(window); Pump(); DwmFlush();
    }
};
FsZhuangtai Status() {
    FsZhuangtai s{}; s.size = sizeof(s);
    Require(getStatus(&s) == FS_CHENGGONG, "status"); return s;
}
FsCaijiZhuangtai Extra() {
    FsCaijiZhuangtai s{}; s.size = sizeof(s);
    Require(getExtra(&s) == FS_CHENGGONG, "capture status"); return s;
}
template<class Predicate>
void Wait(Predicate predicate, const char* message) {
    const auto deadline = GetTickCount64() + 10000;
    while (!predicate()) {
        Pump(); Sleep(10);
        if (GetTickCount64() >= deadline) {
            const auto s = Status();
            std::fprintf(stderr, "WGC wait result=%d detail=%08X bound=%u frame=%llu size=%ux%u\n",
                s.result, static_cast<unsigned>(s.detail), s.bound, s.frame.frameNumber,
                s.frame.width, s.frame.height);
            Require(false, message);
        }
    }
}
bool IsPattern(const BYTE* pixels, UINT pitch, UINT width, UINT height, bool reverse) {
    for (UINT y = 0; y < height; ++y) {
        const bool red = (y < height / 2) != reverse;
        for (UINT x = 0; x < width; ++x) {
            const BYTE* p = pixels + size_t{pitch} * y + size_t{x} * 4;
            if (p[0] != (red ? 0 : 255) || p[1] || p[2] != (red ? 255 : 0) || p[3] != 255)
                return false;
        }
    }
    return true;
}
FsGpuZhen Acquire() {
    FsGpuZhen gpu{}; gpu.size = sizeof(gpu);
    Require(acquireGpu(&gpu) == FS_CHENGGONG, "acquire GPU");
    Require(gpu.captureBackend == FS_CAIJI_WGC && gpu.textureFormat == DXGI_FORMAT_B8G8R8A8_UNORM,
        "WGC metadata");
    return gpu;
}
// 测试程序自己读取租约，不经 CopyFrame；检查共享纹理及持有期间的逐像素稳定性。
void CheckGpu(const FsGpuZhen& gpu, bool reverse) {
    ComPtr<IDXGIFactory4> factory; ComPtr<IDXGIAdapter> adapter;
    Check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "WGC reader factory");
    const LUID luid{gpu.adapterLuidLow, gpu.adapterLuidHigh};
    Check(factory->EnumAdapterByLuid(luid, IID_PPV_ARGS(&adapter)), "WGC reader adapter");
    ComPtr<ID3D11Device> device; ComPtr<ID3D11DeviceContext> context;
    Check(D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, nullptr, 0,
        D3D11_SDK_VERSION, &device, nullptr, &context), "WGC reader device");
    ComPtr<ID3D11Texture2D> shared, staging;
    Check(device->OpenSharedResource(reinterpret_cast<HANDLE>(gpu.sharedHandle), IID_PPV_ARGS(&shared)),
        "WGC open shared texture");
    D3D11_TEXTURE2D_DESC desc{}; shared->GetDesc(&desc);
    desc.BindFlags = desc.MiscFlags = 0; desc.Usage = D3D11_USAGE_STAGING;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    Check(device->CreateTexture2D(&desc, nullptr, &staging), "WGC test staging");
    context->CopyResource(staging.Get(), shared.Get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    Check(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped), "WGC test map");
    const bool matches = IsPattern(static_cast<BYTE*>(mapped.pData), mapped.RowPitch,
        desc.Width, desc.Height, reverse);
    context->Unmap(staging.Get(), 0);
    Require(matches, "top-down client pixels, no border, stable GPU snapshot");
}
void WaitNew(PaintTarget& target, bool reverse) {
    const auto previous = Status().frame.frameNumber;
    std::fprintf(stderr, "WGC repaint reverse=%d after=%llu leases=%u skipped=%llu\n",
        reverse, previous, Extra().activeLeases, Extra().skippedFrames);
    target.Paint(reverse);
    Wait([&] { return Status().frame.frameNumber > previous; }, "new composited frame");
    // DWM 初始帧可能仍在队列中；等待呈现稳定后再用已知色块验像素。
    for (int i = 0; i < 10; ++i) { Pump(); Sleep(10); }
}
}

int wmain(int argc, wchar_t** argv) {
    try {
        const bool expectSilent = argc > 1 && !wcscmp(argv[1], L"--expect-silent");
        SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        PaintTarget target;
        wchar_t filename[32768]{};
        GetModuleFileNameW(nullptr, filename, static_cast<DWORD>(std::size(filename)));
        HMODULE module = LoadLibraryW((std::filesystem::path(filename).parent_path() / L"ZhaoTu.dll").c_str());
        Require(module != nullptr, "load ZhaoTu");
        bindWindow = reinterpret_cast<FsBindFn>(GetProcAddress(module, "BindWindow"));
        unbindWindow = reinterpret_cast<FsUnbindFn>(GetProcAddress(module, "UnBindWindow"));
        setInterval = reinterpret_cast<IntervalFn>(GetProcAddress(module, "SetCaptureFrameInterval"));
        copyFrame = reinterpret_cast<FsCopyFn>(GetProcAddress(module, "CopyFrame"));
        getStatus = reinterpret_cast<FsStatusFn>(GetProcAddress(module, "GetStatus"));
        getExtra = reinterpret_cast<FsCaptureStatusFn>(GetProcAddress(module, "GetCaptureStatus"));
        acquireGpu = reinterpret_cast<FsGpuAcquireFn>(GetProcAddress(module, "AcquireGpuFrame"));
        releaseGpu = reinterpret_cast<FsGpuReleaseFn>(GetProcAddress(module, "ReleaseGpuFrame"));
        mapPoint = reinterpret_cast<FsMapFn>(GetProcAddress(module, "MapPoint"));
        Require(bindWindow && unbindWindow && setInterval && copyFrame && getStatus &&
            getExtra && acquireGpu && releaseGpu && mapPoint, "exports");
        Require(!Status().bound && !Status().hasFrame && Extra().gpuSlots == 0, "unbound no capture");
        Require(bindWindow(target.window, "wgc", "normal", "normal", 0) == 1, "WGC bind");
        const auto version = Status().bindingVersion;
        Require(bindWindow(target.window, "wgc", "normal", "normal", 0) == 1 &&
            Status().bindingVersion == version, "idempotent WGC bind");
        Require(bindWindow(target.window, "dx", "normal", "normal", 0) == 0 &&
            Status().result == FS_YIBANGDING && Status().bindingVersion == version &&
            Extra().captureBackend == FS_CAIJI_WGC && !strcmp(Status().display, "wgc"),
            "reject backend switch while bound");
        Require(bindWindow(target.window, "wgc", "normal", "normal", 0), "clear expected bind diagnosis");
        Wait([&] { return Status().hasFrame; }, "first WGC frame");
        WaitNew(target, true);
        WaitNew(target, false);
        const auto frozenFrame = Status().frame.frameNumber;
        Require(setInterval(-1) == 1, "freeze WGC snapshot");
        target.Paint(true);
        for (int i = 0; i < 20; ++i) { Pump(); Sleep(10); }
        Require(Status().frame.frameNumber == frozenFrame, "WGC does not publish while frozen");
        Require(setInterval(0) == 1, "resume WGC capture");
        WaitNew(target, false);
        RECT client{}; GetClientRect(target.window, &client);
        const auto first = Status().frame;
        Require(first.width == static_cast<UINT>(client.right) && first.height == static_cast<UINT>(client.bottom) &&
            first.clientWidth == first.width && first.clientHeight == first.height, "physical client crop");
        Require(Extra().cpuReadbacks == 0, "status does not read CPU");
        FsZhen capacity{}; capacity.size = sizeof(capacity);
        Require(copyFrame(nullptr, 0, &capacity) == FS_HUANCHONG_BUZU &&
            Extra().cpuReadbacks == 0, "capacity query without readback");
        auto a = Acquire(); CheckGpu(a, false);
        WaitNew(target, true);
        auto b = Acquire(); CheckGpu(b, true);
        WaitNew(target, false);
        auto c = Acquire(); CheckGpu(c, false);
        const auto skipped = Extra().skippedFrames;
        target.Paint(true);
        Wait([&] { return Extra().skippedFrames > skipped; }, "full pool skips");
        CheckGpu(a, false); CheckGpu(b, true);
        Require(Extra().gpuSlots == 3 && Extra().cpuReadbacks == 0, "bounded GPU-only pool");
        Require(releaseGpu(b.token) == FS_CHENGGONG, "release middle lease");
        CheckGpu(c, false);
        Require(releaseGpu(c.token) == FS_CHENGGONG, "release latest lease");
        WaitNew(target, false);
        WaitNew(target, true);
        std::vector<BYTE> pixels(static_cast<size_t>(capacity.requiredBytes));
        FsZhen copied{}; copied.size = sizeof(copied);
        Require(copyFrame(pixels.data(), pixels.size(), &copied) == FS_CHENGGONG &&
            IsPattern(pixels.data(), copied.stride, copied.width, copied.height, true), "demand CPU BGRA");
        Require(Extra().cpuReadbacks == 1 && Extra().readbackFrame == copied.frameNumber, "CPU statistics");
        YanzhengXianKaChazhao(GetModuleHandleW(L"ZhaoTu.dll"),"0000ff");
        double x{}, y{};
        Require(mapPoint(10, 15, &x, &y, 0) == FS_CHENGGONG && x == 10 && y == 15, "WGC client coordinates");
        SetWindowPos(target.window, nullptr, 0, 0, 341, 269, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        target.Paint(false); GetClientRect(target.window, &client);
        Wait([&] {
            const auto frame = Status().frame;
            return frame.width == static_cast<UINT>(client.right) && frame.height == static_cast<UINT>(client.bottom);
        }, "WGC resize and pool recreation");
        CheckGpu(a, false);
        Require(unbindWindow() && !Status().hasFrame, "unbind invalidates");
        FsGpuZhen rejected{}; rejected.size = sizeof(rejected);
        Require(acquireGpu(&rejected) == FS_WEIBANGDING, "unbound rejects new lease");
        CheckGpu(a, false);
        Require(releaseGpu(a.token) == FS_CHENGGONG, "retired lease release");
        Require(bindWindow(target.window, "dx", "normal", "normal", 0),
            "switch WGC to DX");
        Require(unbindWindow() &&
            bindWindow(target.window, "obs", "normal", "normal", 0), "switch DX to OBS");
        Require(unbindWindow() &&
            bindWindow(target.window, "wgc", "normal", "normal", 1), "switch OBS to WGC");
        Wait([&] { return Status().hasFrame; }, "WGC rebind");
        Require(unbindWindow(), "WGC end");
        std::thread owner([&] {
            Require(bindWindow(target.window, "wgc", "normal", "normal", 0), "worker-owned WGC binding");
        });
        owner.join();
        Wait([&] { return !Status().bound; }, "owner thread auto-unbind");
        Require(bindWindow(target.window, "wgc", "normal", "normal", 0), "bind before destroy");
        DestroyWindow(target.window); target.window = nullptr;
        Wait([&] { return !Status().bound; }, "destroy window auto-unbind");
        Require(!expectSilent || GetConsoleWindow() == nullptr, "silent WGC no console");
        std::puts("PASS WGC: GDI window, client crop, BGRA, GPU leases/pool, resize, backend switch, auto-unbind.");
        return 0;
    } catch (...) { return 1; }
}
