// Windows Graphics Capture 产生窗口纹理，GPU 裁剪客户区后交给共用快照池。
// 此文件不读取 CPU 像素、不使用 OBS 依赖，也不在宿主 UI 线程等待首帧。
#include "WgcCaiji.h"
#include "Neibu.h"
#include <d3d11.h>
#include <d3d11_4.h>
#include <dwmapi.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <wrl/client.h>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>

using Microsoft::WRL::ComPtr;
using namespace winrt::Windows::Graphics;
using namespace winrt::Windows::Graphics::Capture;
using namespace winrt::Windows::Graphics::DirectX;
using namespace winrt::Windows::Graphics::DirectX::Direct3D11;

namespace fsdm::wgc {
namespace {
INIT_ONCE workerOnce = INIT_ONCE_STATIC_INIT;
HANDLE wake{};
std::mutex commandLock;
std::condition_variable completed;
struct Command { HWND window{}; uint64_t version{}, serial{}; };
Command requested;
uint64_t consumed{}, finished{};
HRESULT initializedResult = E_PENDING;

// 同一工作线程复用设备，关闭/重绑只重建系统采集会话。设备移除后才丢弃设备。
struct Device {
    ComPtr<ID3D11Device> native;
    IDirect3DDevice projected{nullptr};
    void Ensure() {
        if (native && SUCCEEDED(native->GetDeviceRemovedReason())) return;
        projected = nullptr; native.Reset();
        winrt::check_hresult(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
            &native, nullptr, nullptr));
        ComPtr<ID3D11DeviceContext> context;
        native->GetImmediateContext(&context);
        ComPtr<ID3D11Multithread> protection;
        winrt::check_hresult(context.As(&protection));
        protection->SetMultithreadProtected(TRUE);
        ComPtr<IDXGIDevice> dxgi;
        winrt::check_hresult(native.As(&dxgi));
        winrt::com_ptr<IInspectable> inspectable;
        winrt::check_hresult(CreateDirect3D11DeviceFromDXGIDevice(dxgi.Get(), inspectable.put()));
        projected = inspectable.as<IDirect3DDevice>();
    }
};

// 采集对象包含完整窗口；按 DWM 可见边界定位客户区，防止将标题栏当成客户区坐标。
// 调整尺寸的过渡帧可能与当前窗口边界不符，跳过它并保留已有快照。
bool ClientRegion(HWND window, SizeInt32 content, D3D11_BOX& box) {
    if (IsIconic(window) || content.Width <= 0 || content.Height <= 0) return false;
    RECT outer{};
    const auto matches = [&](const RECT& r) {
        return r.right - r.left == content.Width && r.bottom - r.top == content.Height;
    };
    if (FAILED(DwmGetWindowAttribute(window, DWMWA_EXTENDED_FRAME_BOUNDS, &outer, sizeof(outer))) ||
        !matches(outer)) {
        if (!GetWindowRect(window, &outer) || !matches(outer)) return false;
    }
    RECT client{};
    POINT origin{};
    if (!GetClientRect(window, &client) || !ClientToScreen(window, &origin)) return false;
    const LONG left = origin.x - outer.left, top = origin.y - outer.top;
    const LONG right = left + client.right - client.left, bottom = top + client.bottom - client.top;
    if (left < 0 || top < 0 || right <= left || bottom <= top ||
        right > content.Width || bottom > content.Height) return false;
    box = {static_cast<UINT>(left), static_cast<UINT>(top), 0,
        static_cast<UINT>(right), static_cast<UINT>(bottom), 1};
    return true;
}

struct Session {
    HWND window{};
    uint64_t version{};
    GraphicsCaptureItem item{nullptr};
    Direct3D11CaptureFramePool pool{nullptr};
    GraphicsCaptureSession capture{nullptr};
    SizeInt32 size{};
    std::shared_ptr<std::atomic_bool> closed = std::make_shared<std::atomic_bool>();
    winrt::event_token closedToken{};
    winrt::event_token frameArrivedToken{};

    void Start(Device& device, const Command& command) {
        // 记录本次绑定的窗口和版本号；版本号用于阻止解绑或重绑后到达的旧帧进入新绑定。
        window = command.window; version = command.version;
        // 通过 Windows 图形捕获接口，把指定 HWND 转成 WGC 会话使用的窗口捕获项。
        const auto interop = winrt::get_activation_factory<GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
        winrt::check_hresult(interop->CreateForWindow(window, winrt::guid_of<GraphicsCaptureItem>(),
            winrt::put_abi(item)));
        // 读取窗口当前捕获尺寸；无有效尺寸时无法创建帧池，因此直接报告参数错误。
        size = item.Size();
        if (size.Width <= 0 || size.Height <= 0) winrt::throw_hresult(E_INVALIDARG);
        // 准备可供 WGC 使用的 D3D11 设备；同一工作线程上的设备会在重绑时复用。
        device.Ensure();
        // FreeThreaded 不要求宿主创建消息分发器；工作线程只在拿到真实帧时提交快照。
        // 建立两个缓冲位的帧池，帧格式为常见的 BGRA 32 位；系统新帧先进入这里。
        pool = Direct3D11CaptureFramePool::CreateFreeThreaded(device.projected,
            DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, size);
        // 系统回调可能运行在任意线程；这里只唤醒专用采集线程，不在回调里访问 GPU 资源。
        frameArrivedToken = pool.FrameArrived([signal = wake](const auto&, const auto&) {
            if (signal) SetEvent(signal);
        });
        // 把帧池和目标窗口组成实际捕获会话，后续由系统持续提供窗口画面。
        capture = pool.CreateCaptureSession(item);
        // 若当前系统接口支持该选项，则不把鼠标指针合成进捕获画面。
        if (const auto cursor = capture.try_as<IGraphicsCaptureSession2>())
            cursor.IsCursorCaptureEnabled(false);
        // 记录窗口关闭状态，后续取帧时可据此结束失效会话。
        const auto flag = closed;
        closedToken = item.Closed([flag](const auto&, const auto&) { flag->store(true); });
        // 启动系统采集；后续帧到达事件会唤醒工作线程进行处理。
        capture.StartCapture();
    }
    ~Session() {
        // 关闭系统对象不持有状态锁，解绑只改变版本，不会与帧发布互相等待。
        try { if (item && closedToken.value) item.Closed(closedToken); } catch (...) {}
        try { if (pool && frameArrivedToken.value) pool.FrameArrived(frameArrivedToken); } catch (...) {}
        try { if (capture) capture.Close(); } catch (...) {}
        try { if (pool) pool.Close(); } catch (...) {}
    }
    void Poll(Device& device) {
        if (closed->load()) winrt::throw_hresult(RO_E_CLOSED);
        auto frame = pool.TryGetNextFrame();
        if (!frame) return;
        bool sizeChanged = false;
        bool hasSampledFrame = false;
        Direct3D11CaptureFrame sampledFrame{nullptr};
        SizeInt32 latestContent{};
        for (;;) {
            const auto content = frame.ContentSize();
            latestContent = content;
            sizeChanged |= content.Width != size.Width || content.Height != size.Height;
            if (SelectInputFrame(version)) {
                // 队列有积压时只保留最后一张符合跳帧节奏的帧，其他帧及时归还。
                if (hasSampledFrame) sampledFrame.Close();
                sampledFrame = std::move(frame);
                hasSampledFrame = true;
            } else {
                frame.Close();
            }
            auto newer = pool.TryGetNextFrame();
            if (!newer) break;
            frame = std::move(newer);
        }
        const auto started = std::chrono::steady_clock::now();
        if (sizeChanged || latestContent.Width != size.Width || latestContent.Height != size.Height) {
            if (hasSampledFrame) sampledFrame.Close();
            // 任一积压帧尺寸变化都重建帧池，避免遗漏中间尺寸变化；新尺寸首帧到来前
            // 继续保留上一个有效快照。
            if (latestContent.Width > 0 && latestContent.Height > 0) {
                pool.Recreate(device.projected, DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, latestContent);
                size = latestContent;
            }
            return;
        }
        if (!hasSampledFrame) return;
        const auto content = sampledFrame.ContentSize();
        D3D11_BOX region{};
        if (ClientRegion(window, content, region)) {
            const auto access = sampledFrame.Surface().as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
            ComPtr<ID3D11Texture2D> texture;
            winrt::check_hresult(access->GetInterface(IID_PPV_ARGS(&texture)));
            const double elapsed = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - started).count();
            SubmitWgc(version, texture.Get(), &region, S_OK, elapsed);
        }
        sampledFrame.Close();
    }
};

DWORD WINAPI Run(void*) {
    HRESULT apartment = S_OK;
    try { winrt::init_apartment(winrt::apartment_type::multi_threaded); }
    catch (const winrt::hresult_error& error) { apartment = error.code(); }
    // 所有窗口边界与客户区运算均用物理像素；此线程专属于本模块。
    SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    Device device;
    std::unique_ptr<Session> session;
    uint64_t failedVersion{};
    HRESULT failedResult{};
    for (;;) {
        Command command{};
        bool changed{};
        {
            std::lock_guard lock(commandLock);
            changed = requested.serial != consumed;
            if (changed) { command = requested; consumed = command.serial; }
        }
        if (changed) {
            session.reset();
            failedVersion = 0;
            HRESULT result = apartment;
            if (command.window && SUCCEEDED(result)) {
                try {
                    if (!GraphicsCaptureSession::IsSupported()) result = E_NOTIMPL;
                    else {
                        // 候选对象完全就绪后才交给主循环，初始化异常也会执行关闭逻辑。
                        auto candidate = std::make_unique<Session>();
                        candidate->Start(device, command);
                        session = std::move(candidate);
                    }
                } catch (const winrt::hresult_error& error) { result = error.code(); }
                catch (...) { result = E_OUTOFMEMORY; }
            }
            {
                std::lock_guard lock(commandLock);
                if (requested.serial == command.serial) {
                    initializedResult = result; finished = command.serial;
                    completed.notify_all();
                }
            }
        }
        if (session && target.load() == session->window && generation.load() == session->version) {
            HRESULT result = S_OK;
            try { session->Poll(device); }
            catch (const winrt::hresult_error& error) { result = error.code(); }
            catch (...) { result = E_OUTOFMEMORY; }
            if (FAILED(result)) {
                failedVersion = session->version; failedResult = result;
                session.reset();
            }
        }
        // 状态锁短暂忙碌不能吞掉终止错误；只有成功报告或请求改变后才取消诊断。
        if (failedVersion && SubmitWgc(failedVersion, nullptr, nullptr, failedResult, 0))
            failedVersion = 0;
        // 命令请求和 FrameArrived 共用自动重置事件；空闲时休眠，直到有命令或新帧。
        WaitForSingleObject(wake, INFINITE);
    }
}

BOOL CALLBACK StartWorker(PINIT_ONCE, PVOID, PVOID*) {
    wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!wake) return FALSE;
    HANDLE thread = CreateThread(nullptr, 0, Run, nullptr, 0, nullptr);
    if (!thread) {
        const DWORD error = GetLastError();
        CloseHandle(wake); wake = nullptr; SetLastError(error);
        return FALSE;
    }
    CloseHandle(thread);
    return TRUE;
}
}

int Begin(HWND window, uint64_t version, HRESULT& detail, ULONGLONG deadline) {
    if (!InitOnceExecuteOnce(&workerOnce, StartWorker, nullptr, nullptr)) {
        detail = HRESULT_FROM_WIN32(GetLastError()); return FS_CHUSHIHUA_SHIBAI;
    }
    std::unique_lock lock(commandLock);
    const uint64_t serial = requested.serial + 1;
    requested = {window, version, serial};
    SetEvent(wake);
    const ULONGLONG now = GetTickCount64();
    const auto timeout = std::chrono::milliseconds(deadline > now ? deadline - now : 0);
    if (!completed.wait_for(lock, timeout, [&] { return finished == serial; })) {
        requested = {nullptr, 0, serial + 1}; SetEvent(wake);
        detail = HRESULT_FROM_WIN32(WAIT_TIMEOUT); return FS_CHUSHIHUA_CHAOSHI;
    }
    detail = initializedResult;
    if (SUCCEEDED(detail)) return FS_CHENGGONG;
    return detail == E_NOTIMPL || detail == REGDB_E_CLASSNOTREG || detail == E_NOINTERFACE
        ? FS_BUZHICHI : FS_CHUSHIHUA_SHIBAI;
}

void End() {
    std::lock_guard lock(commandLock);
    if (!wake) return;
    requested = {nullptr, 0, requested.serial + 1};
    SetEvent(wake);
}
}
