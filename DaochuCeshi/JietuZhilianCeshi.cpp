// 外部 EXE 验证：只包含交付头文件，直接链接交付的两个 LIB。
// 本程序自己创建 DX11 测试窗口，不绑定其他程序；固定图案用来核对截图和找图坐标。
// 使用无控制台入口，报告写在 EXE 同目录，便于同时检查 DLL 工作台确实关闭。
#include "FsDmJiekou.h"
#include <d3d11.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <array>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

extern "C" {
__declspec(dllimport) int WINAPI ZhuaTuBegin(HWND hwnd, const char* display, const char* mouse, const char* keypad, int mode);
__declspec(dllimport) int WINAPI ZhuaTuEnd(void);
__declspec(dllimport) int WINAPI ZhuaTuSetCaptureFrameInterval(int interval);
__declspec(dllimport) int WINAPI ZhuaTuCopyFrame(void* pixels, uint64_t capacity, FsZhen* frame);
__declspec(dllimport) int WINAPI ZhuaTuGetStatus(FsZhuangtai* status);
__declspec(dllimport) int WINAPI ZhuaTuAcquireGpuFrame(FsGpuZhen* frame);
__declspec(dllimport) int WINAPI ZhuaTuReleaseGpuFrame(uint64_t token);
__declspec(dllimport) int WINAPI ZhuaTuGetCaptureStatus(FsCaijiZhuangtai* status);
__declspec(dllimport) HRESULT WINAPI ZhuaTuAcquireProcessor(uint32_t low, int32_t high, FsGpuChuli* processor);
__declspec(dllimport) HRESULT WINAPI ZhuaTuReleaseProcessor(uint64_t token);
__declspec(dllimport) int WINAPI ZhuaTuMapPoint(double x, double y, double* outX, double* outY, int direction);
}

namespace {
namespace fs = std::filesystem;
using Microsoft::WRL::ComPtr;
constexpr int kuan = 128, gao = 96, zuo = 20, shang = 24, tuKuan = 23, tuGao = 17;

std::string Utf8(const fs::path& path) {
    const auto text = path.u8string();
    return {text.begin(), text.end()};
}
fs::path Chengxu() {
    wchar_t path[32768]{};
    const DWORD n = GetModuleFileNameW(nullptr, path, 32768);
    if (!n || n == 32768) throw std::runtime_error("无法取得 EXE 路径");
    return path;
}
struct Jilu {
    std::ofstream file;
    unsigned passed{}, failed{};
    explicit Jilu(const fs::path& path) : file(path, std::ios::binary | std::ios::trunc) {
        if (!file) throw std::runtime_error("无法创建报告");
        file << "\xef\xbb\xbf";
    }
    void Line(const std::string& text) { file << text << '\n'; file.flush(); }
    void Check(bool ok, const std::string& text) {
        ok ? ++passed : ++failed;
        Line(std::string(ok ? "[PASS] " : "[FAIL] ") + text);
    }
    // 前置条件失败时停止后续依赖测试，避免把“没有采集帧”误报为所有算法错误。
    void Need(bool ok, const std::string& text) {
        Check(ok, text);
        if (!ok) throw std::runtime_error(text);
    }
};
void Hr(HRESULT hr, const char* text) {
    if (FAILED(hr)) throw std::runtime_error(std::string(text) + " HRESULT=" +
        std::to_string(static_cast<unsigned long>(hr)));
}
// COM 是 Windows 图像组件要求的线程初始化；DLL 内部的初始化不替代调用方，
// 因此 EXE 自己使用 WIC 解码前也必须初始化，并在全部组件释放后配对结束。
struct ComXiancheng {
    ComXiancheng() { Hr(CoInitializeEx(nullptr, COINIT_MULTITHREADED), "初始化 EXE 图像组件线程"); }
    ~ComXiancheng() { CoUninitialize(); }
};
bool YouKongzhitai() { return GetConsoleWindow() != nullptr || GetConsoleCP() != 0; }
void Xiaoxi() {
    MSG msg{};
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

// WARP 是 Windows 的软件图形设备，仍然走真实的 DX11 呈现和采集流程。
// 非均匀且不对称的图案可防止纯色模板在许多位置同时命中，掩盖坐标错误。
struct Xuanran {
    HWND hwnd{};
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<IDXGISwapChain> chain;
    std::vector<uint32_t> pixels;
    Xuanran() : pixels(size_t(kuan) * gao) {
        WNDCLASSW wc{};
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpfnWndProc = DefWindowProcW;
        wc.lpszClassName = L"FsDmJietuZhilianCeshi";
        if (!RegisterClassW(&wc)) throw std::runtime_error("注册测试窗口失败");
        hwnd = CreateWindowExW(WS_EX_TOOLWINDOW, wc.lpszClassName, L"FsDm 截图接口测试",
            WS_POPUP, 40, 40, kuan, gao, nullptr, nullptr, wc.hInstance, nullptr);
        if (!hwnd) throw std::runtime_error("创建测试窗口失败");
        ShowWindow(hwnd, SW_SHOWNOACTIVATE);
        DXGI_SWAP_CHAIN_DESC desc{};
        desc.BufferDesc.Width = kuan;
        desc.BufferDesc.Height = gao;
        desc.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = 1;
        desc.OutputWindow = hwnd;
        desc.Windowed = TRUE;
        desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
        Hr(D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0,
            nullptr, 0, D3D11_SDK_VERSION, &desc, &chain, &device, nullptr, &context),
            "创建 DX11 WARP 设备");
        for (int y = 0; y < gao; ++y) for (int x = 0; x < kuan; ++x) {
            const unsigned r = (x * 37 + y * 13 + 17) & 255;
            const unsigned g = (x * 11 + y * 47 + 29) & 255;
            const unsigned b = (x * 53 + y * 19 + 83) & 255;
            pixels[size_t(y) * kuan + x] = 0xff000000u | (r << 16) | (g << 8) | b;
        }
    }
    ~Xuanran() {
        // 先解除呈现拦截所依赖的绑定，再销毁设备和窗口。
        UnBindWindow();
        chain.Reset(); context.Reset(); device.Reset();
        if (hwnd) DestroyWindow(hwnd);
    }
    void Present() {
        ComPtr<ID3D11Texture2D> back, upload;
        Hr(chain->GetBuffer(0, IID_PPV_ARGS(&back)), "获取交换链纹理");
        D3D11_TEXTURE2D_DESC desc{};
        back->GetDesc(&desc);
        desc.BindFlags = 0; desc.MiscFlags = 0;
        D3D11_SUBRESOURCE_DATA data{pixels.data(), kuan * 4, 0};
        Hr(device->CreateTexture2D(&desc, &data, &upload), "创建图案纹理");
        context->CopyResource(back.Get(), upload.Get());
        Hr(chain->Present(0, 0), "呈现图案");
        Xiaoxi();
    }
};

// 结果的释放函数与返回结果的 DLL 相同；异常退出时也不能留下结果内存。
struct Jieguo {
    const FsTongyongJieguo* p;
    ~Jieguo() { FreeResult(p); }
};
void Chazhao(Jilu& log, FsDm_FindPicFn find, const char* function,
    const std::string& alias, const char* backend, bool expected) {
    log.Need(SetFindBackend(backend) == 1, std::string("设置找图后端 ") + backend);
    Jieguo result{find(0, 0, kuan, gao, alias.c_str(), "000000", 1.0, 0)};
    const auto* p = result.p;
    bool ok = p && p->size == sizeof(*p) && p->version == FSDM_BANBEN;
    if (expected) {
        ok = ok && p->status == FS_CHENGGONG && p->count == 1 && p->items &&
            (p->items[0].fields & (FS_JG_ID | FS_JG_XY | FS_JG_FILE)) ==
            (FS_JG_ID | FS_JG_XY | FS_JG_FILE) &&
            p->items[0].id == 0 && p->items[0].x == zuo && p->items[0].y == shang &&
            p->items[0].file && alias == p->items[0].file;
    } else {
        ok = ok && p->status == FS_JIAZAI_SHIBAI && p->count == 0;
    }
    std::string detail = p ? " status=" + std::to_string(p->status) +
        " count=" + std::to_string(p->count) : " 空指针";
    if (p && p->count && p->items)
        detail += " x=" + std::to_string(p->items[0].x) + " y=" + std::to_string(p->items[0].y);
    log.Check(ok, std::string(function) + "/" + backend + " " + alias +
        (expected ? " 精确命中 (20,24)" : " 无文件及内存模板时加载失败") + detail);
}

// 用 Windows 图像解码器独立读回文件，检查容器、尺寸及像素。
// JPG 经过有损压缩，只检查解码和尺寸；BMP/PNG 逐像素核对原始颜色。
void YanTu(Jilu& log, const fs::path& path, REFGUID container, const Xuanran& render) {
    ComPtr<IWICImagingFactory> factory;
    Hr(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&factory)), "创建文件解码器");
    ComPtr<IWICBitmapDecoder> decoder;
    Hr(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
        WICDecodeMetadataCacheOnLoad, &decoder), "打开截图文件");
    GUID actual{};
    Hr(decoder->GetContainerFormat(&actual), "读取文件容器");
    log.Check(IsEqualGUID(actual, container), Utf8(path.filename()) + " 文件格式正确");
    ComPtr<IWICBitmapFrameDecode> frame;
    Hr(decoder->GetFrame(0, &frame), "读取截图帧");
    UINT w{}, h{};
    Hr(frame->GetSize(&w, &h), "读取截图尺寸");
    log.Need(w == tuKuan && h == tuGao, Utf8(path.filename()) + " 尺寸 23x17");
    if (IsEqualGUID(container, GUID_ContainerFormatJpeg)) return;
    ComPtr<IWICFormatConverter> converter;
    Hr(factory->CreateFormatConverter(&converter), "创建像素转换器");
    Hr(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppBGRA,
        WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom), "转换截图像素");
    std::vector<uint32_t> pixels(size_t(w) * h);
    Hr(converter->CopyPixels(nullptr, w * 4, static_cast<UINT>(pixels.size() * 4),
        reinterpret_cast<BYTE*>(pixels.data())), "读回截图像素");
    bool same = true;
    for (UINT y = 0; y < h; ++y) for (UINT x = 0; x < w; ++x)
        same &= (pixels[size_t(y) * w + x] & 0xffffff) ==
            (render.pixels[size_t(y + shang) * kuan + x + zuo] & 0xffffff);
    log.Check(same, Utf8(path.filename()) + " 裁剪像素逐点一致");
}

struct JietuHanshu {
    const char* name;
    const char* extension;
    FsDm_CaptureFn capture;
    const GUID* container;
};
void MoshiCeshi(Jilu& log, const fs::path& folder, const Xuanran& render) {
    const std::array<JietuHanshu, 3> functions{{
        {"Capture", "bmp", &Capture, &GUID_ContainerFormatBmp},
        {"CaptureJpg", "jpg", &CaptureJpg, &GUID_ContainerFormatJpeg},
        {"CapturePng", "png", &CapturePng, &GUID_ContainerFormatPng}
    }};
    for (const auto& f : functions) {
        // 默认参数必须写成真正的五参数直接调用，不能用六参数函数指针冒充验证。
        const std::string defaultName = std::string("moren_") + f.name + "." + f.extension;
        int rc{};
        if (f.capture == &Capture) rc = Capture(zuo, shang, zuo + tuKuan, shang + tuGao, defaultName.c_str());
        else if (f.capture == &CaptureJpg) rc = CaptureJpg(zuo, shang, zuo + tuKuan, shang + tuGao, defaultName.c_str());
        else rc = CapturePng(zuo, shang, zuo + tuKuan, shang + tuGao, defaultName.c_str());
        log.Need(rc == 1 && fs::exists(folder / defaultName), std::string(f.name) + " 省略 mode 保存文件");
        YanTu(log, folder / defaultName, *f.container, render);
        log.Need(fs::remove(folder / defaultName), "删除默认模式的测试文件");
        Chazhao(log, &FindPic, "FindPic", defaultName, "cpu", false);

        for (int mode = 1; mode <= 3; ++mode) {
            const std::string alias = "moshi" + std::to_string(mode) + "_" + f.name + "." + f.extension;
            log.Need(f.capture(zuo, shang, zuo + tuKuan, shang + tuGao, alias.c_str(), mode) == 1,
                std::string(f.name) + " mode=" + std::to_string(mode) + " 返回 1");
            log.Check(fs::exists(folder / alias) == (mode != 1), alias + " 文件存在状态符合模式");
            if (mode != 1) YanTu(log, folder / alias, *f.container, render);
            // 显式 mode=2 的文件也需要验证可用于找图；JPG 的有损像素不要求完全匹配。
            if (mode == 2 && f.capture != &CaptureJpg)
                for (const char* backend : {"cpu", "gpu"})
                    Chazhao(log, &FindPic, "FindPic", alias, backend, true);
            if (mode != 1) log.Need(fs::remove(folder / alias), "删除模式测试文件 " + alias);
            // mode=2 文件曾被查找时会进入普通缓存，先释放，避免缓存掩盖模式错误。
            if (mode == 2) log.Need(FreePic(alias.c_str()) == 1, "清理文件查找缓存");
            if (mode == 2) {
                Chazhao(log, &FindPic, "FindPic", alias, "cpu", false);
                continue;
            }
            for (const char* backend : {"cpu", "gpu"}) {
                Chazhao(log, &FindPic, "FindPic", alias, backend, true);
                Chazhao(log, &FindPicS, "FindPicS", alias, backend, true);
                Chazhao(log, &FindPicEx, "FindPicEx", alias, backend, true);
                Chazhao(log, &FindPicExS, "FindPicExS", alias, backend, true);
            }
            log.Need(FreePic(alias.c_str()) == 1, "释放内存模板 " + alias);
            Chazhao(log, &FindPic, "FindPic", alias, "cpu", false);
        }
    }
}
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    try {
        const fs::path exeFolder = Chengxu().parent_path();
        Jilu log(exeFolder / L"JietuZhilianJieguo.txt");
        try {
            ComXiancheng apartment;
            log.Line("交付头文件 + ZhaoTu.lib + ZhuaTu.lib 外部 EXE 测试；固定 DX11 WARP 图案。");
            log.Check(!YouKongzhitai(), "进程启动时没有控制台或工作台");
            // 读取每个直接导入地址，保证全部公开函数实际进入 EXE 的导入表。
#define ZHILIAN_HANSHU(name) { auto volatile fn = &name; log.Check(fn != nullptr, "LIB 直接导入 " #name); }
#include "ZhilianBiao.inc"
#undef ZHILIAN_HANSHU
            for (const wchar_t* name : {L"ZhaoTu.dll", L"ZhuaTu.dll"}) {
                wchar_t modulePath[32768]{};
                const HMODULE module = GetModuleHandleW(name);
                log.Need(module && GetModuleFileNameW(module, modulePath, 32768) &&
                    fs::equivalent(modulePath, exeFolder / name), "加载测试目录 DLL " + Utf8(name));
                log.Line("模块路径：" + Utf8(modulePath));
            }
            const auto deadline = GetTickCount64() + 10000;
            FsZhuangtai status{};
            do {
                status = {}; status.size = sizeof(status);
                log.Need(GetStatus(&status) == FS_CHENGGONG, "等待 DLL 初始化时查询状态");
                if (!status.initializing) break;
                Sleep(10);
            } while (GetTickCount64() < deadline);
            log.Need(!status.initializing, "DLL 初始化完成");
            log.Check(!YouKongzhitai(), "DLL 初始化后没有打开工作台");
            const fs::path folder = exeFolder / (L"JietuYangben_" +
                std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(GetTickCount64()));
            log.Need(fs::create_directory(folder), "创建独立测试样本目录");
            log.Line("样本目录：" + Utf8(folder));
            log.Need(SetPath(Utf8(folder).c_str()) == 1, "SetPath 接受 UTF-8 测试目录");
            log.Check(SetTemplateCacheLimit(0) == 0, "模板容量拒绝 0 字节");
            log.Need(SetTemplateCacheLimit(256ull * 1024 * 1024) == 1, "设置模板容量 256 MiB");
            Xuanran render;
            log.Need(BindWindow(render.hwnd, "dx", "normal", "normal", 0) == 1, "绑定本程序 DX11 窗口");
            bool ready = false;
            const auto frameDeadline = GetTickCount64() + 10000;
            do {
                render.Present();
                status = {}; status.size = sizeof(status);
                const int rc = GetStatus(&status);
                ready = rc == FS_CHENGGONG && status.hasFrame && status.result == FS_CHENGGONG;
                if (ready) break;
                Sleep(10);
            } while (GetTickCount64() < frameDeadline);
            log.Need(ready, "DX 采集取得有效帧 result=" + std::to_string(status.result) +
                " detail=" + std::to_string(status.detail));
            log.Need(status.frame.width == kuan && status.frame.height == gao &&
                status.frame.clientWidth == kuan && status.frame.clientHeight == gao, "采集和客户区尺寸均为 128x96");
            FsZhuangtai lower{}; lower.size = sizeof(lower);
            log.Check(ZhuaTuGetStatus(&lower) == FS_CHENGGONG && lower.bound &&
                lower.hwnd == reinterpret_cast<uint64_t>(render.hwnd) &&
                lower.bindingVersion == status.bindingVersion, "直接调用 ZhuaTu.lib 状态与上层绑定一致");
            // 验证整帧而非仅依靠“找到了”，独立确认采集颜色和方向没有偏移。
            std::vector<uint32_t> copy(size_t(kuan) * gao);
            FsZhen frame{}; frame.size = sizeof(frame);
            const int copyRc = CopyFrame(copy.data(), copy.size() * 4, &frame);
            log.Need(copyRc == FS_CHENGGONG && copy == render.pixels, "CopyFrame 完整像素与渲染源一致");
            MoshiCeshi(log, folder, render);
            log.Check(UnBindWindow() == 1, "测试完成后解绑");
            status = {}; status.size = sizeof(status);
            log.Check(GetStatus(&status) == FS_CHENGGONG && !status.bound, "解绑状态可查询");
            log.Check(!YouKongzhitai(), "截图和 CPU/GPU 找图后仍无工作台");
        } catch (const std::exception& e) {
            log.Check(false, std::string("测试中止：") + e.what());
            UnBindWindow();
        }
        log.Line("汇总：通过 " + std::to_string(log.passed) + "，失败 " + std::to_string(log.failed));
        log.Line("范围：本进程 DX11 WARP 固定画面；不代表其他采集后端或目标应用均已验证。");
        return log.failed ? 1 : 0;
    } catch (...) {
        return 2;
    }
}
