// 端到端测试：在真实 DX10/11/12 WARP 设备上呈现已知纯色，检查 BGRA 和绑定生命周期。
#include "../Gonggong/Jiekou.h"
#include "../ZhaoTu/ZhaoTu.h"
#include <type_traits>
// 在不实际调用 DLL 的情况下检查 C++ 声明允许省略截图模式。
static_assert(std::is_same_v<decltype(Capture(0, 0, 1, 1, "x")), int>);
static_assert(std::is_same_v<decltype(CaptureJpg(0, 0, 1, 1, "x")), int>);
static_assert(std::is_same_v<decltype(CapturePng(0, 0, 1, 1, "x")), int>);
#include "Xuanran.h"
#include "XianKaYanzheng.h"
#include <atomic>
#include <thread>
#include <vector>
#include <cmath>
#include <chrono>
#include <filesystem>
#include <cstring>
#include <dxgi1_4.h>
#include <wincodec.h>

namespace {
bool testWorkbenchImages{};
FsBindFn bindWindow{};
FsUnbindFn unbindWindow{};
using IntervalFn = int (WINAPI*)(int);
IntervalFn setInterval{};
FsCopyFn copyFrame{};
FsStatusFn getStatus{};
FsMapFn mapPoint{};
FsGpuAcquireFn acquireGpu{};
FsGpuReleaseFn releaseGpu{};
FsCaptureStatusFn captureStatus{};
void Require(bool condition, const char* message) {
    if (!condition) { std::fprintf(stderr, "FAIL: %s\n", message); throw Failure{}; }
}
FsZhuangtai Status() {
    FsZhuangtai s{}; s.size = sizeof(s);
    Require(getStatus(&s) == FS_CHENGGONG, "get status"); return s;
}
FsZhen Copy(std::vector<BYTE>& pixels) {
    FsZhen frame{}; frame.size = sizeof(frame);
    Require(copyFrame(pixels.data(), pixels.size(), &frame) == FS_CHENGGONG, "copy full frame");
    return frame;
}
void CheckColor(const std::vector<BYTE>& pixels, const FsZhen& frame, bool red) {
    Require(frame.stride == frame.width * 4 && frame.requiredBytes == uint64_t{frame.stride} * frame.height,
        "packed top-down BGRA dimensions");
    for (size_t i = 0; i < frame.requiredBytes; i += 4)
        Require(pixels[i] == (red ? 0 : 255) && pixels[i + 1] == 0 &&
                pixels[i + 2] == (red ? 255 : 0) && pixels[i + 3] == 255, "complete frame color");
    static bool checked[13]{};
    if(frame.backend<13 && !checked[frame.backend]) {
        YanzhengXianKaChazhao(GetModuleHandleW(L"ZhaoTu.dll"),red?"ff0000":"0000ff");
        checked[frame.backend]=true;
    }
}
constexpr float red[] = {1, 0, 0, 1};
constexpr float blue[] = {0, 0, 1, 1};

// 模拟向工作台输入完整命令，检查生成的图片能否由系统解码器读取。
void CheckWorkbenchImages(const FsZhen& captured) {
    wchar_t temp[MAX_PATH]{};
    Require(GetTempPathW(MAX_PATH, temp) != 0, "temporary directory");
    const auto folder = std::filesystem::path(temp) /
        (L"FsDmCeshi_" + std::to_wstring(GetCurrentProcessId()));
    Require(std::filesystem::create_directory(folder), "create test folder");
    const HANDLE input = CreateFileW(L"CONIN$", GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    Require(input != INVALID_HANDLE_VALUE, "open workbench input");
    auto send = [&](const std::wstring& command) {
        std::vector<INPUT_RECORD> records;
        for (const wchar_t c : command + L"\r") {
            INPUT_RECORD record{}; record.EventType = KEY_EVENT;
            record.Event.KeyEvent.bKeyDown = TRUE;
            record.Event.KeyEvent.wRepeatCount = 1;
            record.Event.KeyEvent.wVirtualKeyCode = c == L'\r' ? VK_RETURN : 0;
            record.Event.KeyEvent.uChar.UnicodeChar = c;
            records.push_back(record);
        }
        DWORD written{};
        Require(WriteConsoleInputW(input, records.data(), static_cast<DWORD>(records.size()), &written) &&
            written == records.size(), "send workbench command");
    };
    const HRESULT apartment = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    Require(SUCCEEDED(apartment) || apartment == RPC_E_CHANGED_MODE, "COM initialization");
    ComPtr<IWICImagingFactory> factory;
    Check(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&factory)), "WIC factory");
    struct Case { const wchar_t* function; const wchar_t* filename; int frames; };
    const Case cases[] = {{L"Capture", L"截图.bmp", 1}, {L"CaptureJpg", L"截图.jpg", 1},
        {L"CapturePng", L"截图.png", 1}, {L"Capture", L"显式.bmp", 1},
        {L"CaptureJpg", L"显式.jpg", 1}, {L"CapturePng", L"显式.png", 1}, {L"CaptureGif", L"静态.gif", 1},
        {L"CaptureGif", L"动画.gif", 3}};
    for (const auto& item : cases) {
        const auto path = folder / item.filename;
        std::wstring command = std::wstring(item.function) + L"(0, 0, " +
            std::to_wstring(captured.clientWidth) + L", " + std::to_wstring(captured.clientHeight) +
            L", \"" + path.wstring() + L"\"";
        if (wcsstr(item.filename, L"显式")) command += L", 2";
        if (!wcscmp(item.function, L"CaptureGif"))
            command += item.frames == 1 ? L", 0, 0" : L", 100, 300";
        send(command + L")");
        const ULONGLONG deadline = GetTickCount64() + 8000;
        while (!std::filesystem::exists(path) && GetTickCount64() < deadline) Sleep(20);
        Require(std::filesystem::exists(path), "workbench created image");
        ComPtr<IWICBitmapDecoder> decoder;
        Check(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
            WICDecodeMetadataCacheOnLoad, &decoder), "decode image");
        UINT count{}; Check(decoder->GetFrameCount(&count), "frame count");
        Require(count == static_cast<UINT>(item.frames), "image frame count");
        ComPtr<IWICBitmapFrameDecode> frame;
        Check(decoder->GetFrame(0, &frame), "image frame");
        if (item.frames > 1) {
            ComPtr<IWICMetadataQueryReader> metadata;
            Check(frame->GetMetadataQueryReader(&metadata), "GIF metadata");
            PROPVARIANT delay{}; PropVariantInit(&delay);
            Check(metadata->GetMetadataByName(L"/grctlext/Delay", &delay), "GIF delay");
            Require(delay.vt == VT_UI2 && delay.uiVal == 10, "GIF 100ms delay");
            PropVariantClear(&delay);
        }
        UINT width{}, height{}; Check(frame->GetSize(&width, &height), "image size");
        Require(width == captured.width && height == captured.height, "image dimensions");
        if (!wcscmp(item.function, L"Capture")) {
            WICPixelFormatGUID format{}; Check(frame->GetPixelFormat(&format), "BMP format");
            Require(format == GUID_WICPixelFormat24bppBGR, "24-bit BMP");
        }
        ComPtr<IWICFormatConverter> converter;
        Check(factory->CreateFormatConverter(&converter), "converter");
        Check(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppBGRA,
            WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom), "convert image");
        BYTE pixel[4]{}; WICRect rect{0, 0, 1, 1};
        Check(converter->CopyPixels(&rect, 4, 4, pixel), "image pixel");
        Require(pixel[2] > 200 && pixel[1] < 40 && pixel[0] < 40, "image red pixel");
        converter.Reset(); frame.Reset(); decoder.Reset();
        Require(DeleteFileW(path.c_str()), "remove image");
    }
    factory.Reset();
    if (SUCCEEDED(apartment)) CoUninitialize();
    CloseHandle(input);
    Require(RemoveDirectoryW(folder.c_str()), "remove test folder");
}

struct Renderer11 {
    Window window;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<IDXGISwapChain> chain;
    explicit Renderer11(UINT samples = 1) {
        auto desc = LegacyDesc(window.hwnd, samples);
        Check(D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0,
            nullptr, 0, D3D11_SDK_VERSION, &desc, &chain, &device, nullptr, &context), "DX11 create");
    }
    void Render(bool isRed = true, bool present1 = false) {
        ComPtr<ID3D11Texture2D> back;
        ComPtr<ID3D11RenderTargetView> rtv;
        Check(chain->GetBuffer(0, IID_PPV_ARGS(&back)), "DX11 buffer");
        Check(device->CreateRenderTargetView(back.Get(), nullptr, &rtv), "DX11 RTV");
        context->ClearRenderTargetView(rtv.Get(), isRed ? red : blue);
        DXGI_PRESENT_PARAMETERS params{};
        if (present1) {
            ComPtr<IDXGISwapChain1> chain1; Check(chain.As(&chain1), "DX11 chain1");
            Check(chain1->Present1(0, 0, &params), "DX11 Present1");
        } else Check(chain->Present(0, 0), "DX11 Present");
    }
};
void WaitUnbound() {
    const auto deadline = GetTickCount64() + 2500;
    while (Status().bound && GetTickCount64() < deadline) { Pump(); Sleep(10); }
    Require(!Status().bound, "automatic unbind");
}

void Check12(Renderer12& renderer, bool late) {
    Require(bindWindow(renderer.window.hwnd, "dx", "normal", "windows", 0) == 1, "DX12 bind");
    const auto deadline = GetTickCount64() + 10000;
    do {
        renderer.Render(red, true);
        Pump(); Sleep(10);
    } while (!Status().hasFrame && GetTickCount64() < deadline);
    Require(Status().hasFrame && Status().frame.backend == 12, "DX12 queue detection");
    std::vector<BYTE> pixels(static_cast<size_t>(FSDM_ZUIDA_ZIJIE));
    auto frame = Copy(pixels); CheckColor(pixels, frame, true);
    if (testWorkbenchImages) CheckWorkbenchImages(frame);
    renderer.Render(blue, false);
    frame = Copy(pixels); CheckColor(pixels, frame, false);
    Require(frame.frameNumber >= 2 && frame.qpcFrequency && frame.captureQpc, "DX12 frame clocks");
    Check(renderer.chain->ResizeBuffers(3, 83, 57, DXGI_FORMAT_R8G8B8A8_UNORM, 0), "DX12 resize");
    for (int i = 0; i < 8; ++i) { renderer.Render(red, false); Sleep(5); }
    frame = Copy(pixels);
    Require(frame.width == 83 && frame.height == 57, "DX12 resized frame");
    UINT masks[] = {0, 0, 0};
    IUnknown* queues[] = {renderer.queue.Get(), renderer.queue.Get(), renderer.queue.Get()};
    Check(renderer.chain->ResizeBuffers1(3, 73, 53, DXGI_FORMAT_R8G8B8A8_UNORM, 0, masks, queues), "Resize1");
    for (int i = 0; i < 4; ++i) renderer.Render(blue, true);
    frame = Copy(pixels); CheckColor(pixels, frame, false);
    Require(frame.width == 73 && frame.height == 53, "Resize1 dimensions");
    if (renderer.SupportsEnhancedBarriers()) {
        renderer.Render(red, true, true);
        CheckColor(pixels, Copy(pixels), true);
    } else std::puts("SKIP enhanced barriers: device does not support them.");
    std::printf("DX12 %s: readback %.3f ms, capture %.3f ms (%ux%u)\n",
        late ? "late-load" : "new-chain", frame.readbackMs, frame.captureMs, frame.width, frame.height);
    Require(unbindWindow() == 1, "DX12 unbind");
}

void Check11() {
    Renderer11 renderer(4);
    Require(bindWindow(renderer.window.hwnd, "gdi", "normal", "normal", 0) == 0 &&
            Status().result == FS_BUZHICHI, "unsupported display");
    Require(bindWindow(renderer.window.hwnd, nullptr, "normal", "normal", 0) == 0 &&
            Status().result == FS_CANSHU_CUOWU, "null display");
    for (const char* display : {"", "DX", "normal", "gdi2", "dx2", "wrong"})
        Require(bindWindow(renderer.window.hwnd, display, "normal", "normal", 0) == 0 &&
            Status().result == FS_BUZHICHI && !Status().bound, "invalid display leaves unbound");
    Require(bindWindow(renderer.window.hwnd, "dx", "bad", "normal", 0) == 0, "invalid mouse");
    Require(bindWindow(renderer.window.hwnd, "dx", "normal", "bad", 0) == 0, "invalid keypad");
    Require(bindWindow(renderer.window.hwnd, "dx", "normal", "normal", 101) == 0, "unsupported mode");
    Require(bindWindow(GetDesktopWindow(), "dx", "normal", "normal", 0) == 0, "foreign process window");
    Require(bindWindow(renderer.window.hwnd, "dx", "dx", "dx", 1) == 1, "DX11 bind mode1");
    Require(Status().result == FS_DENGDAISHOUZHEN, "waiting first frame");
    const auto version = Status().bindingVersion;
    Require(bindWindow(renderer.window.hwnd, "dx", "dx", "dx", 1) == 1 &&
            Status().bindingVersion == version, "identical bind idempotent");
    // 第二参数现在是真实采集方式；拒绝切换后，窗口、版本及方式必须仍属于原绑定。
    for (const char* display : {"obs", "wgc"}) {
        Require(bindWindow(renderer.window.hwnd, display, "dx", "dx", 1) == 0 &&
            Status().result == FS_YIBANGDING && Status().bound &&
            Status().bindingVersion == version && !strcmp(Status().display, "dx"),
            "changed capture display preserves live binding");
    }
    Require(bindWindow(renderer.window.hwnd, "dx", "windows", "dx", 1) == 0 &&
            Status().result == FS_YIBANGDING, "changed bind requires unbind");
    Require(bindWindow(renderer.window.hwnd, "dx", "dx", "dx", 1) == 1, "restore last result");
    renderer.Render();
    Require(Status().frame.backend == 11 && !Status().inputImplemented, "DX11 backend and no input simulation");
    std::vector<BYTE> pixels(static_cast<size_t>(FSDM_ZUIDA_ZIJIE));
    auto frame = Copy(pixels); CheckColor(pixels, frame, true);
    FsZhen capacityInfo{}; capacityInfo.size = sizeof(capacityInfo);
    BYTE guard[4] = {7, 8, 9, 10};
    Require(copyFrame(guard, sizeof(guard), &capacityInfo) == FS_HUANCHONG_BUZU &&
            guard[0] == 7 && guard[3] == 10 && capacityInfo.requiredBytes == 64 * 48 * 4, "capacity check no write");
    capacityInfo.size = sizeof(capacityInfo);
    Require(copyFrame(nullptr, 0, &capacityInfo) == FS_HUANCHONG_BUZU, "query capacity");
    double x{}, y{}, x2{}, y2{};
    Require(mapPoint(frame.clientWidth / 2., frame.clientHeight / 2., &x, &y, 0) == FS_CHENGGONG &&
            std::abs(x - 32) < 0.001 && std::abs(y - 24) < 0.001, "client to render");
    Require(mapPoint(x, y, &x2, &y2, 1) == FS_CHENGGONG &&
            std::abs(x2 - frame.clientWidth / 2.) < 0.001, "render to client");
    Require(mapPoint(-1, 0, &x, &y, 0) == FS_CANSHU_CUOWU, "out of range point");
    Sleep(100);
    const auto paused = Copy(pixels);
    Require(paused.frameNumber == frame.frameNumber && paused.captureQpc == frame.captureQpc, "paused cache retained");
    renderer.Render(false, true);
    Require(Copy(pixels).frameNumber > paused.frameNumber, "resume updates");
    CheckColor(pixels, Copy(pixels), false);

    // 多个读取线程同时核对整帧；呈现交替红蓝，任何混合都说明发布不完整。
    std::atomic_bool stop{}, failed{};
    auto read = [&] {
        std::vector<BYTE> bytes(1024 * 1024);
        while (!stop.load()) {
            FsZhen f{}; f.size = sizeof(f);
            const int result = copyFrame(bytes.data(), bytes.size(), &f);
            if (result != FS_CHENGGONG) { failed = true; break; }
            const BYTE b = bytes[0], r = bytes[2];
            if (!((b == 255 && r == 0) || (b == 0 && r == 255))) failed = true;
            for (size_t j = 0; j < f.requiredBytes; j += 4)
                if (bytes[j] != b || bytes[j + 1] || bytes[j + 2] != r || bytes[j + 3] != 255) failed = true;
            std::this_thread::yield();
        }
    };
    std::thread reader1(read), reader2(read);
    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < 40; ++i) renderer.Render(i % 2 == 0);
    stop = true; reader1.join(); reader2.join();
    Require(!failed, "concurrent full-frame readers");
    const double wallMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    frame = Copy(pixels);
    std::printf("DX11 MSAA: 40 presents %.3f ms, last readback %.3f ms, capture %.3f ms\n",
        wallMs, frame.readbackMs, frame.captureMs);
    Check(renderer.chain->ResizeBuffers(1, 79, 51, DXGI_FORMAT_R8G8B8A8_UNORM, 0), "DX11 resize");
    renderer.Render();
    frame = Copy(pixels);
    Require(frame.width == 79 && frame.height == 51, "DX11 resized dimensions");
    Require(unbindWindow() && unbindWindow(), "repeated unbind");
    capacityInfo.size = sizeof(capacityInfo);
    Require(copyFrame(nullptr, 0, &capacityInfo) == FS_WEIBANGDING && capacityInfo.requiredBytes == 0, "unbound invalidates metadata");
    const auto count = Status().capturedFrames;
    renderer.Render();
    Require(!Status().hasFrame && Status().capturedFrames == count, "unbound presents do not capture");
    Require(bindWindow(renderer.window.hwnd, "dx", "normal", "normal", 0) == 1 &&
            Status().bindingVersion != version && !Status().hasFrame, "rebind no old frame");
    renderer.Render();
    Require(Status().hasFrame, "rebind resumed");
    // 让呈现线程和解绑/重绑交错，检查正在读回的旧帧不会越过绑定版本边界。
    std::atomic_bool rendering{true};
    std::thread presenter([&] {
        while (rendering.load()) renderer.Render();
    });
    for (int i = 0; i < 30; ++i) {
        Require(unbindWindow(), "concurrent unbind");
        FsZhen stale{}; stale.size = sizeof(stale);
        Require(copyFrame(nullptr, 0, &stale) == FS_WEIBANGDING && !stale.requiredBytes,
                "in-flight frame cannot survive unbind");
        Require(bindWindow(renderer.window.hwnd, "dx", "normal", "normal", 0), "concurrent rebind");
        auto current = Status();
        if (current.hasFrame)
            Require(current.frame.bindingVersion == current.bindingVersion, "frame binding generation");
    }
    rendering = false;
    presenter.join();
    unbindWindow();
    // 创建绑定的线程退出后，主线程仍可安全读取“未绑定”。
    std::thread bindingThread([&] { Require(bindWindow(renderer.window.hwnd, "dx", "normal", "normal", 0) == 1,
                                          "bind on short-lived thread"); });
    bindingThread.join();
    WaitUnbound();
    Require(bindWindow(renderer.window.hwnd, "dx", "normal", "normal", 0) == 1, "bind before destroy");
    DestroyWindow(renderer.window.hwnd);
    WaitUnbound();
}

FsCaijiZhuangtai Extra() {
    FsCaijiZhuangtai value{}; value.size = sizeof(value);
    Require(captureStatus(&value) == FS_CHENGGONG, "extended status"); return value;
}
FsGpuZhen Acquire() {
    FsGpuZhen value{}; value.size = sizeof(value);
    Require(acquireGpu(&value) == FS_CHENGGONG && value.token && value.sharedHandle,
        "acquire ready GPU snapshot"); return value;
}
// 使用调用方设备打开共享纹理并核对像素，证明接口交付的是可使用的 GPU 资源。
// 测试自身的 Map 不计入 DLL 的按需读回统计。
void CheckLease(const FsGpuZhen& lease, bool isRed) {
    ComPtr<IDXGIFactory4> factory; ComPtr<IDXGIAdapter> adapter;
    Check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "lease factory");
    const LUID luid{lease.adapterLuidLow, lease.adapterLuidHigh};
    Check(factory->EnumAdapterByLuid(luid, IID_PPV_ARGS(&adapter)), "lease adapter");
    ComPtr<ID3D11Device> device; ComPtr<ID3D11DeviceContext> context;
    Check(D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, nullptr, 0,
        D3D11_SDK_VERSION, &device, nullptr, &context), "lease reader device");
    ComPtr<ID3D11Texture2D> opened, staging;
    Check(device->OpenSharedResource(reinterpret_cast<HANDLE>(lease.sharedHandle), IID_PPV_ARGS(&opened)),
        "open shared lease");
    D3D11_TEXTURE2D_DESC desc{}; opened->GetDesc(&desc);
    Require(desc.Width == lease.frame.width && desc.Height == lease.frame.height &&
        static_cast<uint32_t>(desc.Format) == lease.textureFormat, "lease format and size");
    desc.Usage = D3D11_USAGE_STAGING; desc.BindFlags = desc.MiscFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    Check(device->CreateTexture2D(&desc, nullptr, &staging), "lease staging");
    context->CopyResource(staging.Get(), opened.Get());
    D3D11_MAPPED_SUBRESOURCE data{};
    Check(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &data), "lease map");
    for (UINT y = 0; y < desc.Height; ++y) {
        const BYTE* row = static_cast<const BYTE*>(data.pData) + size_t{data.RowPitch} * y;
        const bool rgba = desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM;
        for (UINT x = 0; x < desc.Width; ++x) {
            Require(row[4*x+(rgba ? 0 : 2)] == (isRed ? 255 : 0) &&
                row[4*x+1] == 0 && row[4*x+(rgba ? 2 : 0)] == (isRed ? 0 : 255), "stable GPU lease pixels");
        }
    }
    context->Unmap(staging.Get(), 0); // Map 已确认测试的 GPU 使用完成，此后可释放租约。
}
void CheckGpu() {
    Renderer11 renderer;
    Require(bindWindow(renderer.window.hwnd, "dx", "normal", "normal", 0), "GPU test bind");
    Require(setInterval(-1) == 0, "cannot freeze before first frame");
    Require(setInterval(-2) == 0, "reject interval below minus one");
    Require(bindWindow(renderer.window.hwnd, "obs", "normal", "normal", 0) == 0 &&
        Status().result == FS_YIBANGDING && Extra().captureBackend == FS_CAIJI_DX,
        "cannot switch bound backend");
    Require(bindWindow(renderer.window.hwnd, "dx", "normal", "normal", 0), "reset last error");
    // 每次只呈现一帧，确认参数表示采集后要跳过的输入帧数。
    Require(setInterval(0) == 1, "capture every input frame");
    const auto baseline = Status().capturedFrames;
    for (int input = 1; input <= 4; ++input) {
        renderer.Render();
        Require(Status().capturedFrames == baseline + input, "interval zero samples every input");
    }
    Require(setInterval(1) == 1, "skip one input frame");
    const auto oneSkipBaseline = Status().capturedFrames;
    for (int input = 1; input <= 4; ++input) {
        renderer.Render();
        Require(Status().capturedFrames == oneSkipBaseline + (input + 1) / 2,
            "interval one samples every other input");
    }
    Require(setInterval(2) == 1, "skip two input frames");
    const auto twoSkipBaseline = Status().capturedFrames;
    for (int input = 1; input <= 7; ++input) {
        renderer.Render();
        Require(Status().capturedFrames == twoSkipBaseline + (input + 2) / 3,
            "interval two samples every third input");
    }
    Require(setInterval(0) == 1, "restore every input frame");
    renderer.Render();
    Require(setInterval(-1) == 1, "freeze current snapshot");
    std::vector<BYTE> lockedBytes(1024 * 1024);
    const FsZhen lockedInfo = Copy(lockedBytes);
    CheckColor(lockedBytes, lockedInfo, true);
    const auto frozenNumber = Status().capturedFrames;
    for (int i = 0; i < 3; ++i) renderer.Render(false);
    Require(Status().capturedFrames == frozenNumber, "frozen snapshot stops publication");
    FsZhen lockedRead{}; lockedRead.size = sizeof(lockedRead);
    Require(copyFrame(lockedBytes.data(), lockedBytes.size(), &lockedRead) == FS_CHENGGONG &&
        lockedRead.frameNumber == lockedInfo.frameNumber, "CPU reads frozen frame");
    CheckColor(lockedBytes, lockedRead, true);
    FsGpuZhen lockedGpu = Acquire();
    Require(lockedGpu.frame.frameNumber == lockedInfo.frameNumber,
        "GPU reads same locked frame as CPU");
    CheckLease(lockedGpu, true);
    Require(releaseGpu(lockedGpu.token) == FS_CHENGGONG, "release locked GPU read");
    Require(setInterval(-2) == 0, "invalid interval preserves frozen snapshot");
    Require(setInterval(0) == 1, "unfreeze and resume every input frame");
    renderer.Render(false);
    std::vector<BYTE> freshBytes(1024 * 1024);
    const FsZhen freshInfo = Copy(freshBytes);
    Require(freshInfo.frameNumber > lockedInfo.frameNumber, "resumed capture publishes a fresh frame");
    CheckColor(freshBytes, freshInfo, false);
    renderer.Render();
    FsZhen query{}; query.size = sizeof(query);
    Require(copyFrame(nullptr, 0, &query) == FS_HUANCHONG_BUZU, "GPU capacity query");
    const uint64_t cpuReadbacksBeforeGpuLeases = Extra().cpuReadbacks;
    auto a = Acquire();
    renderer.Render(false); auto b = Acquire();
    renderer.Render(); auto c = Acquire();
    const auto number = Status().frame.frameNumber;
    for (int i = 0; i < 4; ++i) renderer.Render(false);
    const auto full = Extra();
    Require(full.gpuSlots == 3 && full.activeLeases == 3 && full.skippedFrames >= 4 &&
        full.cpuReadbacks == cpuReadbacksBeforeGpuLeases && Status().frame.frameNumber == number,
        "bounded pool skips without CPU read");
    CheckLease(a, true); CheckLease(b, false); CheckLease(c, true);
    Require(releaseGpu(b.token) == FS_CHENGGONG && releaseGpu(b.token) == FS_CANSHU_CUOWU,
        "release validates token");
    renderer.Render(false);
    Require(Status().frame.frameNumber > number &&
        Extra().cpuReadbacks == cpuReadbacksBeforeGpuLeases, "pool resumes GPU only");
    // 旧尺寸仍有租约，新尺寸不得覆盖它。
    Check(renderer.chain->ResizeBuffers(1, 87, 61, DXGI_FORMAT_R8G8B8A8_UNORM, 0), "lease resize");
    renderer.Render(false);
    // 两个旧槽持有中，第三槽无租约，应允许重建尺寸而不饿死更新。
    Require(Status().frame.width == 87, "unleased latest slot updates across resize");
    CheckLease(c, true);
    Require(releaseGpu(c.token) == FS_CHENGGONG, "release resize slot");
    renderer.Render(false); Require(Status().frame.width == 87, "resize resumes after release");
    CheckLease(a, true);
    Require(setInterval(-1) == 1, "freeze before unbind");
    Require(unbindWindow(), "unbind with GPU lease");
    FsGpuZhen invalid{}; invalid.size = sizeof(invalid);
    Require(acquireGpu(&invalid) == FS_WEIBANGDING && !invalid.token, "unbind refuses new lease");
    CheckLease(a, true);
    Require(bindWindow(renderer.window.hwnd, "dx", "normal", "normal", 0), "rebind with old lease");
    renderer.Render(false);
    Require(Extra().gpuSlots <= 3 && Extra().activeLeases == 1, "old lease counted across binding");
    CheckLease(a, true);
    Require(releaseGpu(a.token) == FS_CHENGGONG, "release old binding lease");
    std::vector<BYTE> bytes(1024 * 1024); const auto frame = Copy(bytes); CheckColor(bytes, frame, false);
    Require(Extra().cpuReadbacks == 1 && Extra().readbackFrame == frame.frameNumber &&
        Status().frame.readbackMs == 0, "separate CPU readback statistics");
    unbindWindow();
    std::puts("PASS GPU: same-adapter sharing, immutable leases, bounded pool, resize, rebind, demand readback.");
}

void Check10() {
    Window window;
    auto desc = LegacyDesc(window.hwnd);
    ComPtr<ID3D10Device> device;
    ComPtr<IDXGISwapChain> chain;
    Check(D3D10CreateDeviceAndSwapChain(nullptr, D3D10_DRIVER_TYPE_WARP, nullptr, 0,
        D3D10_SDK_VERSION, &desc, &chain, &device), "DX10 create");
    Require(bindWindow(window.hwnd, "dx", "normal", "normal", 0) == 1, "DX10 bind");
    ComPtr<ID3D10Texture2D> back;
    ComPtr<ID3D10RenderTargetView> rtv;
    Check(chain->GetBuffer(0, IID_PPV_ARGS(&back)), "DX10 buffer");
    Check(device->CreateRenderTargetView(back.Get(), nullptr, &rtv), "DX10 RTV");
    device->ClearRenderTargetView(rtv.Get(), red);
    Check(chain->Present(0, 0), "DX10 Present");
    std::vector<BYTE> pixels(1024 * 1024);
    auto frame = Copy(pixels);
    Require(frame.backend == 10, "DX10 backend");
    CheckColor(pixels, frame, true);
    std::printf("DX10 readback %.3f ms, capture %.3f ms\n", frame.readbackMs, frame.captureMs);
    unbindWindow();
}
}

int wmain(int argc, wchar_t** argv) {
    try {
        testWorkbenchImages = argc > 1 && !wcscmp(argv[1], L"--workbench-images");
        Renderer12 late; // 交换链和队列均在加载 DLL 前创建，验证晚加载队列识别。
        late.Render(red, false);
        const HWND initialConsole = GetConsoleWindow();
        wchar_t executable[32768];
        GetModuleFileNameW(nullptr, executable, static_cast<DWORD>(std::size(executable)));
        const auto dllPath = std::filesystem::path(executable).parent_path() / L"ZhaoTu.dll";
        HMODULE module = LoadLibraryW(dllPath.c_str());
        Require(module != nullptr, "load ZhaoTu");
        bindWindow = reinterpret_cast<FsBindFn>(GetProcAddress(module, "BindWindow"));
        unbindWindow = reinterpret_cast<FsUnbindFn>(GetProcAddress(module, "UnBindWindow"));
        setInterval = reinterpret_cast<IntervalFn>(GetProcAddress(module, "SetCaptureFrameInterval"));
        copyFrame = reinterpret_cast<FsCopyFn>(GetProcAddress(module, "CopyFrame"));
        getStatus = reinterpret_cast<FsStatusFn>(GetProcAddress(module, "GetStatus"));
        mapPoint = reinterpret_cast<FsMapFn>(GetProcAddress(module, "MapPoint"));
        Require(bindWindow && unbindWindow && setInterval && copyFrame &&
            getStatus && mapPoint, "all C exports");
        Require(!GetProcAddress(module, "LockFrame") && !GetProcAddress(module, "UnlockFrame"),
            "separate frame lock exports are removed");
        Require(!Status().bound && !Status().hasFrame, "initially unbound");
        acquireGpu = reinterpret_cast<FsGpuAcquireFn>(GetProcAddress(module, "AcquireGpuFrame"));
        releaseGpu = reinterpret_cast<FsGpuReleaseFn>(GetProcAddress(module, "ReleaseGpuFrame"));
        captureStatus = reinterpret_cast<FsCaptureStatusFn>(GetProcAddress(module, "GetCaptureStatus"));
        Require(acquireGpu && releaseGpu && captureStatus, "new C exports");
        Require(!GetProcAddress(module, "SetCaptureBackend"), "removed selector is not exported");
        // BindWindow 的 display 直接决定首次 DX12 采集，无需预先选择方式。
        FsCaijiZhuangtai defaults{}; defaults.size = sizeof(defaults);
        Require(captureStatus(&defaults) == FS_CHENGGONG &&
            defaults.captureBackend == FS_CAIJI_DX && !defaults.gpuSlots &&
            !defaults.cpuReadbacks, "default dx without capture");
        Check12(late, true);
        Require(captureStatus(&defaults) == FS_CHENGGONG &&
            defaults.captureBackend == FS_CAIJI_DX, "unbind preserves default dx");
        Require(!GetProcAddress(GetModuleHandleW(L"ZhuaTu.dll"), "ZhuaTuSetCaptureBackend"),
            "removed low-level selector is not exported");
        Require(!GetProcAddress(GetModuleHandleW(L"ZhuaTu.dll"), "ZhuaTuLockFrame") &&
            !GetProcAddress(GetModuleHandleW(L"ZhuaTu.dll"), "ZhuaTuUnlockFrame"),
            "separate low-level frame lock exports are removed");
        if (testWorkbenchImages) {
            std::puts("PASS: workbench image commands and decoding.");
            return 0;
        }
        Renderer12 early;
        Check12(early, false);
        Check11();
        Check10();
        CheckGpu();
        if (argc > 1 && !wcscmp(argv[1], L"--expect-silent"))
            Require(GetConsoleWindow() == initialConsole, "disabled flags create no console");
        std::puts("PASS: capture, lifecycle, capacity, coordinates, concurrency, DX10/11/12.");
        return 0;
    } catch (...) { return 1; }
}
