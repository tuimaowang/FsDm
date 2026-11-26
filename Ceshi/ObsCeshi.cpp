// 使用真实硬件和配套 OBS 二进制做端到端测试；错误场景通过独立进程隔离会话。
#include "../Gonggong/Jiekou.h"
#include "Xuanran.h"
#include "XianKaYanzheng.h"
#include "../third_party/obs/graphics_hook_protocol.h"
#include <filesystem>
#include <vector>
#include <cstring>
#include <memory>

namespace {
FsBindFn bindWindow{};
FsUnbindFn unbindWindow{};
FsCopyFn copyFrame{};
FsStatusFn getStatus{};
FsCaptureStatusFn getExtra{};
FsGpuAcquireFn acquireGpu{};
FsGpuReleaseFn releaseGpu{};
std::filesystem::path deploymentDirectory;
void Require(bool ok, const char* message) {
    if (!ok) { std::fprintf(stderr, "FAIL OBS: %s\n", message); throw Failure{}; }
}
// 从复制后的目录启动测试时，确认实际模块也来自该目录，防止开发目录回退掩盖依赖遗漏。
void RequireLocalModule(const wchar_t* name) {
    HMODULE module = GetModuleHandleW(name);
    Require(module != nullptr, "dependency loaded");
    wchar_t filename[32768]{};
    const DWORD count = GetModuleFileNameW(module, filename, static_cast<DWORD>(std::size(filename)));
    Require(count && count < std::size(filename), "dependency filename");
    Require(std::filesystem::equivalent(filename, deploymentDirectory / name), "dependency from copied directory");
}
FsZhuangtai Status() {
    FsZhuangtai s{}; s.size = sizeof(s); Require(!getStatus(&s), "status"); return s;
}
FsCaijiZhuangtai Extra() {
    FsCaijiZhuangtai s{}; s.size = sizeof(s); Require(!getExtra(&s), "extra"); return s;
}
struct Renderer {
    Window window;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<IDXGISwapChain> chain;
    Renderer() {
        auto desc = LegacyDesc(window.hwnd);
        Check(D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
            nullptr, 0, D3D11_SDK_VERSION, &desc, &chain, &device, nullptr, &context), "hardware DX11");
    }
    void Render(bool blue = false, bool present1 = false) {
        ComPtr<ID3D11Texture2D> buffer; ComPtr<ID3D11RenderTargetView> view;
        Check(chain->GetBuffer(0, IID_PPV_ARGS(&buffer)), "OBS buffer");
        Check(device->CreateRenderTargetView(buffer.Get(), nullptr, &view), "OBS view");
        const float color[] = {blue ? 0.f : 1.f, 0, blue ? 1.f : 0.f, 1};
        context->ClearRenderTargetView(view.Get(), color);
        if (present1) {
            ComPtr<IDXGISwapChain1> one; Check(chain.As(&one), "OBS chain1");
            DXGI_PRESENT_PARAMETERS params{}; Check(one->Present1(0, 0, &params), "OBS present1");
        } else Check(chain->Present(0, 0), "OBS present");
        Pump();
    }
};
void WaitFrame(Renderer& renderer, bool blue = false) {
    const auto deadline = GetTickCount64() + 12000;
    do {
        renderer.Render(blue);
        if (Status().hasFrame && Status().result == FS_CHENGGONG) return;
        Sleep(10);
    } while (GetTickCount64() < deadline);
    const auto s = Status();
    std::fprintf(stderr, "OBS wait: result=%d detail=%08X snapshots=%llu\n",
        s.result, static_cast<unsigned>(s.detail), s.capturedFrames);
    Require(false, "waiting real OBS snapshot");
}
void Color(bool blue) {
    RequireLocalModule(L"graphics-hook64.dll");
    std::vector<BYTE> pixels(1024 * 1024);
    FsZhen f{}; f.size = sizeof(f);
    Require(copyFrame(pixels.data(), pixels.size(), &f) == FS_CHENGGONG, "OBS demand readback");
    for (size_t i = 0; i < f.requiredBytes; i += 4)
        Require(pixels[i] == (blue ? 255 : 0) && pixels[i+1] == 0 &&
            pixels[i+2] == (blue ? 0 : 255) && pixels[i+3] == 255, "OBS BGRA pixels");
    YanzhengXianKaChazhao(GetModuleHandleW(L"ZhaoTu.dll"),blue?"0000ff":"ff0000");
}
}
int wmain(int argc, wchar_t** argv) {
    try {
        const std::wstring mode = argc > 1 ? argv[1] : L"";
        std::unique_ptr<Renderer12> late12;
        const float red[] = {1, 0, 0, 1}, blue[] = {0, 0, 1, 1};
        if (mode == L"--dx12") {
            late12 = std::make_unique<Renderer12>(true);
            late12->Render(red, false);
        }
        Renderer renderer; // 在加载两个 DLL 前创建交换链，覆盖晚加载。
        renderer.Render();
        wchar_t executable[32768]{};
        GetModuleFileNameW(nullptr, executable, static_cast<DWORD>(std::size(executable)));
        const auto directory = std::filesystem::path(executable).parent_path();
        deploymentDirectory = directory;
        HMODULE module = LoadLibraryW((directory / L"ZhaoTu.dll").c_str());
        Require(module != nullptr, "load");
        bindWindow = reinterpret_cast<FsBindFn>(GetProcAddress(module, "BindWindow"));
        unbindWindow = reinterpret_cast<FsUnbindFn>(GetProcAddress(module, "UnBindWindow"));
        copyFrame = reinterpret_cast<FsCopyFn>(GetProcAddress(module, "CopyFrame"));
        getStatus = reinterpret_cast<FsStatusFn>(GetProcAddress(module, "GetStatus"));
        getExtra = reinterpret_cast<FsCaptureStatusFn>(GetProcAddress(module, "GetCaptureStatus"));
        acquireGpu = reinterpret_cast<FsGpuAcquireFn>(GetProcAddress(module, "AcquireGpuFrame"));
        releaseGpu = reinterpret_cast<FsGpuReleaseFn>(GetProcAddress(module, "ReleaseGpuFrame"));
        Require(bindWindow && unbindWindow && copyFrame && getStatus && getExtra &&
            acquireGpu && releaseGpu, "exports");
        if (mode == L"--dx12") {
            Require(bindWindow(late12->window.hwnd, "obs", "normal", "normal", 0), "OBS DX12 bind");
            RequireLocalModule(L"ZhuaTu.dll");
            const auto deadline = GetTickCount64() + 12000;
            do { late12->Render(red, false); Pump(); Sleep(10); }
            while (!Status().hasFrame && GetTickCount64() < deadline);
            Require(Status().hasFrame && Status().frame.backend == 12 && !Extra().cpuReadbacks, "OBS DX12 first snapshot");
            Color(false);
            for (int i = 0; i < 4; ++i) late12->Render(blue, true);
            Color(true);
            Check(late12->chain->ResizeBuffers(3, 85, 63, DXGI_FORMAT_R8G8B8A8_UNORM, 0), "OBS DX12 resize");
            for (int i = 0; i < 8; ++i) { late12->Render(red, false); Sleep(5); }
            Require(Status().frame.width == 85 && Status().frame.height == 63, "OBS DX12 resize snapshot");
            Color(false); unbindWindow();
            std::puts("PASS OBS DX12 hardware, late load, Present/Present1, resize."); return 0;
        }
        if (mode == L"--dx10") {
            Window window;
            auto desc = LegacyDesc(window.hwnd);
            ComPtr<ID3D10Device> device; ComPtr<IDXGISwapChain> chain;
            Check(D3D10CreateDeviceAndSwapChain(nullptr, D3D10_DRIVER_TYPE_HARDWARE, nullptr, 0,
                D3D10_SDK_VERSION, &desc, &chain, &device), "OBS DX10 hardware");
            Require(bindWindow(window.hwnd, "obs", "normal", "normal", 0), "OBS DX10 bind");
            auto render = [&](const float* color) {
                ComPtr<ID3D10Texture2D> back; ComPtr<ID3D10RenderTargetView> rtv;
                Check(chain->GetBuffer(0, IID_PPV_ARGS(&back)), "OBS DX10 buffer");
                Check(device->CreateRenderTargetView(back.Get(), nullptr, &rtv), "OBS DX10 view");
                device->ClearRenderTargetView(rtv.Get(), color);
                Check(chain->Present(0, 0), "OBS DX10 present"); Pump();
            };
            const auto deadline = GetTickCount64() + 12000;
            do { render(red); Sleep(10); } while (!Status().hasFrame && GetTickCount64() < deadline);
            Require(Status().hasFrame && Status().frame.backend == 10 && !Extra().cpuReadbacks, "OBS DX10 first snapshot");
            Color(false); for (int i = 0; i < 4; ++i) render(blue); Color(true);
            unbindWindow(); std::puts("PASS OBS DX10 hardware BGRA."); return 0;
        }
        if (mode == L"--missing") {
            Require(!bindWindow(renderer.window.hwnd, "obs", "normal", "normal", 0) &&
                Status().result == FS_JIAZAI_SHIBAI &&
                Status().detail == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND), "missing dependencies diagnosis");
            Require(!Status().bound && !Status().hasFrame && Extra().captureBackend == FS_CAIJI_DX,
                "failed OBS initialization preserves unbound capture mode");
            Require(bindWindow(renderer.window.hwnd, "dx", "normal", "normal", 0),
                "DX can bind after OBS dependency failure");
            renderer.Render();
            Require(Status().hasFrame && !strcmp(Status().display, "dx"), "DX recovered after failed OBS bind");
            unbindWindow();
            return 0;
        }
        if (mode == L"--conflict") {
            const auto name = std::wstring(SHMEM_HOOK_INFO) + std::to_wstring(GetCurrentProcessId());
            replica::Handle occupied(CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
                0, sizeof(hook_info), name.c_str()));
            Require(!!occupied, "fake existing session");
            Require(!bindWindow(renderer.window.hwnd, "obs", "normal", "normal", 0) &&
                Status().result == FS_OBS_CHONGTU, "reject occupied session");
            return 0;
        }
        Require(bindWindow(renderer.window.hwnd, "obs", "normal", "normal", 0), "OBS bind");
        RequireLocalModule(L"ZhuaTu.dll");
        const auto version = Status().bindingVersion;
        Require(!strcmp(Status().display, "obs") &&
            bindWindow(renderer.window.hwnd, "obs", "normal", "normal", 0) &&
            Status().bindingVersion == version, "OBS display and idempotent binding");
        Require(!bindWindow(renderer.window.hwnd, "dx", "normal", "normal", 0) &&
            Status().bindingVersion == version && Extra().captureBackend == FS_CAIJI_OBS &&
            !strcmp(Status().display, "obs"), "bound OBS rejects DX without switching");
        Require(bindWindow(renderer.window.hwnd, "obs", "normal", "normal", 0), "restore OBS diagnosis");
        if (mode == L"--mismatch") {
            Renderer other;
            const auto deadline = GetTickCount64() + 12000;
            do {
                other.Render(true); renderer.Render();
                if (Status().result == FS_CHUANGKOU_BUPIPEI) break;
                Sleep(10);
            } while (GetTickCount64() < deadline);
            Require(Status().result == FS_CHUANGKOU_BUPIPEI && !Status().hasFrame,
                "reject different OBS window");
            unbindWindow(); return 0;
        }
        WaitFrame(renderer);
        Require(Extra().cpuReadbacks == 0 && Extra().captureBackend == FS_CAIJI_OBS, "OBS GPU only");
        FsGpuZhen gpu{}; gpu.size = sizeof(gpu);
        Require(acquireGpu(&gpu) == FS_CHENGGONG && gpu.sharedHandle, "OBS GPU lease");
        Require(releaseGpu(gpu.token) == FS_CHENGGONG, "OBS release");
        Color(false);
        const auto paused = Status().frame.frameNumber;
        Sleep(50); Require(Status().frame.frameNumber == paused, "OBS pause keeps snapshot");
        for (int i = 0; i < 5; ++i) renderer.Render(true, true);
        Color(true);
        Check(renderer.chain->ResizeBuffers(1, 93, 67, DXGI_FORMAT_R8G8B8A8_UNORM, 0), "OBS resize");
        for (int i = 0; i < 8; ++i) { renderer.Render(true); Sleep(5); }
        Require(Status().frame.width == 93 && Status().frame.height == 67, "OBS resized snapshot");
        Color(true);
        for (int i = 0; i < 3; ++i) {
            Require(unbindWindow(), "OBS unbind");
            renderer.Render(); Require(!Status().hasFrame && Extra().cpuReadbacks == 0, "OBS unbound");
            Require(bindWindow(renderer.window.hwnd, "dx", "normal", "normal", 0), "DX bind after OBS");
            renderer.Render(true); Color(true);
            Require(unbindWindow(), "unbind DX before OBS");
            Require(bindWindow(renderer.window.hwnd, "obs", "normal", "normal", 0), "OBS rebind");
            WaitFrame(renderer); Color(false);
        }
        unbindWindow();
        std::puts("PASS OBS hardware: BGRA, GPU leases, Present/Present1, resize, pause, DX/OBS rebind.");
        return 0;
    } catch (...) { return 1; }
}
