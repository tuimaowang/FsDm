// Direct3D 11 动画演示，使用 DXGI 交换链提交画面，便于验证 Present 帧采集。
// 场景由网格、投影立方体、轨道光点、波形柱和 GDI 生成的中文纹理组成。
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi.h>
extern "C" {
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
}
#include <wrl/client.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <memory>
#include <new>
#include <process.h>
#include <string>
#include <vector>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")

namespace {

using Microsoft::WRL::ComPtr;

constexpr int kSceneWidth = 960;
constexpr int kSceneHeight = 640;
constexpr float kPi = 3.14159265358979323846f;
constexpr UINT kToggleFrameCommand = 1001;
constexpr UINT kRunLuaTestCommand = 1002;
constexpr UINT kLuaTestFinishedMessage = WM_APP + 11;
constexpr UINT kVertexCapacity = 8192;

struct Vertex {
    float x, y;
    float r, g, b, a;
    float u, v;
};

struct ScreenConstants {
    float width, height;
    float padding[2];
};

HWND g_window{};
HMENU g_testMenu{};
HANDLE g_luaTestThread{};
ComPtr<ID3D11Device> g_device;
ComPtr<ID3D11DeviceContext> g_context;
ComPtr<IDXGISwapChain> g_swapChain;
ComPtr<ID3D11RenderTargetView> g_renderTarget;
ComPtr<ID3D11VertexShader> g_vertexShader;
ComPtr<ID3D11PixelShader> g_colorPixelShader;
ComPtr<ID3D11PixelShader> g_texturePixelShader;
ComPtr<ID3D11InputLayout> g_inputLayout;
ComPtr<ID3D11Buffer> g_vertices;
ComPtr<ID3D11Buffer> g_screenConstants;
ComPtr<ID3D11ShaderResourceView> g_textView;
ComPtr<ID3D11SamplerState> g_textSampler;
ComPtr<ID3D11BlendState> g_alphaBlend;

int g_width = kSceneWidth;
int g_height = kSceneHeight;
bool g_paused{};
bool g_frameStopped{};
bool g_resizePending{};
bool g_renderFailed{};
float g_angle{};
float g_speed = 1.0f;
double g_elapsedSeconds{};
LARGE_INTEGER g_clockFrequency{};
LARGE_INTEGER g_previousTime{};
wchar_t g_graphicsStatus[128] = L"DX11 正常";

// 桥接 DLL 由本测试程序在普通启动流程中加载，不涉及远程注入。
// 初始化函数只负责让桥接层加载 jiaofu 目录中的原版 ZhaoTu.dll。
using QjInitializeFn = int(WINAPI*)(const char*);
using QjShutdownFn = int(WINAPI*)(void);

HMODULE g_bridgeModule{};
QjInitializeFn g_qjInitialize{};
QjShutdownFn g_qjShutdown{};

void UpdateWindowTitle();

std::wstring GetExecutableDirectory() {
    std::vector<wchar_t> path(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, path.data(),
                                            static_cast<DWORD>(path.size()));
    if (length == 0 || length >= path.size()) return {};
    std::wstring executablePath(path.data(), length);

    const size_t separator = executablePath.find_last_of(L"\\/");
    if (separator == std::wstring::npos) return {};
    executablePath.resize(separator + 1);
    return executablePath;
}

bool WideToUtf8(const std::wstring& value, std::string& result) {
    if (value.empty()) return false;
    const int required = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, value.c_str(), -1,
        nullptr, 0, nullptr, nullptr);
    if (required <= 0) return false;

    std::vector<char> buffer(static_cast<size_t>(required), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.c_str(), -1,
                            buffer.data(), required, nullptr, nullptr) <= 0) {
        return false;
    }
    result.assign(buffer.data());
    return true;
}

bool Utf8ToWide(const char* value, std::wstring& result) {
    if (!value || !*value) return false;
    const int required = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                              value, -1, nullptr, 0);
    if (required <= 0) return false;
    std::vector<wchar_t> buffer(static_cast<size_t>(required), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value, -1,
                            buffer.data(), required) <= 0) {
        return false;
    }
    result.assign(buffer.data());
    return true;
}

std::wstring ResolvePath(const std::wstring& directory, const wchar_t* relativePath) {
    const std::wstring combined = directory + relativePath;
    std::vector<wchar_t> normalized(32768, L'\0');
    const DWORD length = GetFullPathNameW(
        combined.c_str(), static_cast<DWORD>(normalized.size()),
        normalized.data(), nullptr);
    if (length == 0 || length >= normalized.size()) return {};
    return std::wstring(normalized.data(), length);
}

bool InitializeBridge(std::wstring& errorMessage) {
    const std::wstring executableDirectory = GetExecutableDirectory();
    if (executableDirectory.empty()) {
        errorMessage = L"无法确定测试程序所在目录。";
        return false;
    }

    // 构建后会把桥接 DLL 复制到 EXE 同目录，因此不依赖启动时的当前目录。
    const std::wstring bridgePath =
        executableDirectory + L"LuaJiekouQiaojie.dll";
    g_bridgeModule = LoadLibraryExW(
        bridgePath.c_str(), nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!g_bridgeModule) {
        errorMessage = L"加载桥接 DLL 失败：\n" + bridgePath +
            L"\nWindows 错误码：" + std::to_wstring(GetLastError());
        return false;
    }

    g_qjInitialize = reinterpret_cast<QjInitializeFn>(
        GetProcAddress(g_bridgeModule, "QjInitialize"));
    g_qjShutdown = reinterpret_cast<QjShutdownFn>(
        GetProcAddress(g_bridgeModule, "QjShutdown"));
    if (!g_qjInitialize || !g_qjShutdown) {
        errorMessage = L"桥接 DLL 缺少 QjInitialize 或 QjShutdown 导出函数。";
        FreeLibrary(g_bridgeModule);
        g_bridgeModule = nullptr;
        g_qjInitialize = nullptr;
        g_qjShutdown = nullptr;
        return false;
    }

    // EXE 位于 Dx11Donghua\bin\<配置>，向上三级到 FsDm 后进入 linshi\jiaofu。
    std::wstring jiaofuPath = executableDirectory + L"..\\..\\..\\linshi\\jiaofu";
    std::vector<wchar_t> normalizedPath(32768, L'\0');
    const DWORD normalizedLength = GetFullPathNameW(
        jiaofuPath.c_str(), static_cast<DWORD>(normalizedPath.size()),
        normalizedPath.data(), nullptr);
    if (normalizedLength == 0 || normalizedLength >= normalizedPath.size()) {
        errorMessage = L"无法解析 jiaofu 目录路径：\n" + jiaofuPath;
        FreeLibrary(g_bridgeModule);
        g_bridgeModule = nullptr;
        g_qjInitialize = nullptr;
        g_qjShutdown = nullptr;
        return false;
    }
    jiaofuPath.assign(normalizedPath.data(), normalizedLength);

    std::string jiaofuPathUtf8;
    if (!WideToUtf8(jiaofuPath, jiaofuPathUtf8)) {
        errorMessage = L"无法将 jiaofu 目录路径转换为 UTF-8。";
        FreeLibrary(g_bridgeModule);
        g_bridgeModule = nullptr;
        g_qjInitialize = nullptr;
        g_qjShutdown = nullptr;
        return false;
    }

    if (!g_qjInitialize(jiaofuPathUtf8.c_str())) {
        using QjGetLastErrorFn = const char*(WINAPI*)(void);
        const auto getLastError = reinterpret_cast<QjGetLastErrorFn>(
            GetProcAddress(g_bridgeModule, "QjGetLastError"));
        const char* detail = getLastError ? getLastError() : nullptr;
        errorMessage = L"桥接 DLL 初始化失败。\n";
        if (detail && *detail) {
            const int required = MultiByteToWideChar(
                CP_UTF8, MB_ERR_INVALID_CHARS, detail, -1, nullptr, 0);
            if (required > 1) {
                std::vector<wchar_t> wideDetail(static_cast<size_t>(required), L'\0');
                if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, detail, -1,
                                        wideDetail.data(), required) > 0) {
                    errorMessage += wideDetail.data();
                }
            }
        }
        errorMessage += L"\njiaofu 目录：\n" + jiaofuPath;
        FreeLibrary(g_bridgeModule);
        g_bridgeModule = nullptr;
        g_qjInitialize = nullptr;
        g_qjShutdown = nullptr;
        return false;
    }
    return true;
}

void ShutdownBridge() {
    if (g_qjShutdown) {
        g_qjShutdown();
        g_qjShutdown = nullptr;
    }
    g_qjInitialize = nullptr;
    if (g_bridgeModule) {
        FreeLibrary(g_bridgeModule);
        g_bridgeModule = nullptr;
    }
}

struct LuaTestRequest {
    HWND window{};
    std::wstring scriptPath;
    std::wstring bridgePath;
};

struct LuaTestCompletion {
    int resultCode{};
    std::wstring message;
};

// 在单独的工作线程中创建 LuaJIT 状态并运行报告脚本，避免长时间识别
// 阻塞 DX11 窗口的消息循环和动画帧提交。
unsigned __stdcall LuaTestThreadProc(void* parameter) {
    std::unique_ptr<LuaTestRequest> request(
        static_cast<LuaTestRequest*>(parameter));
    auto completion = std::make_unique<LuaTestCompletion>();

    std::string scriptPathUtf8;
    std::string bridgePathUtf8;
    wchar_t hwndText[32]{};
    swprintf_s(hwndText, L"0x%llX",
        static_cast<unsigned long long>(
            reinterpret_cast<std::uintptr_t>(request->window)));
    std::string hwndUtf8;

    if (!WideToUtf8(request->scriptPath, scriptPathUtf8) ||
        !WideToUtf8(request->bridgePath, bridgePathUtf8) ||
        !WideToUtf8(hwndText, hwndUtf8)) {
        completion->resultCode = 1;
        completion->message = L"无法把脚本路径、桥接 DLL 路径或窗口句柄转换为 UTF-8。";
    } else {
        lua_State* state = luaL_newstate();
        if (!state) {
            completion->resultCode = 1;
            completion->message = L"创建 LuaJIT 状态失败。";
        } else {
            luaL_openlibs(state);

            // 由宿主程序传入当前窗口句柄和已加载桥接 DLL 的同一路径，
            // 避免脚本再次从另一个目录加载第二份桥接 DLL。
            lua_createtable(state, 0, 2);
            lua_pushstring(state, hwndUtf8.c_str());
            lua_setfield(state, -2, "hwnd");
            lua_pushstring(state, bridgePathUtf8.c_str());
            lua_setfield(state, -2, "bridge_path");
            lua_setfield(state, LUA_GLOBALSINDEX, "BAOGAO_CESHI_CONFIG");

            int status = luaL_loadfile(state, scriptPathUtf8.c_str());
            if (status == 0) {
                status = lua_pcall(state, 0, 1, 0);
            }
            if (status != 0) {
                size_t messageLength = 0;
                const char* message = lua_tolstring(state, -1, &messageLength);
                std::wstring wideMessage;
                if (message && Utf8ToWide(message, wideMessage)) {
                    completion->message = L"Lua 脚本执行失败：\n" + wideMessage;
                } else {
                    completion->message = L"Lua 脚本执行失败；详细信息请查看测试日志。";
                }
                completion->resultCode = 1;
            } else if (lua_isnumber(state, -1) && lua_tointeger(state, -1) != 0) {
                completion->resultCode = 1;
                completion->message =
                    L"Lua 脚本已运行，但测试报告中记录了错误；请查看测试日志。";
            } else {
                completion->message = L"Lua 测试完成，报告已写入测试日志。";
            }
            lua_close(state);
        }
    }

    LuaTestCompletion* result = completion.release();
    if (!PostMessageW(request->window, kLuaTestFinishedMessage,
                      reinterpret_cast<WPARAM>(result), 0)) {
        delete result;
    }
    return 0;
}

void StartLuaTest(HWND window) {
    if (g_luaTestThread) {
        MessageBoxW(window, L"Lua 测试正在运行，请等待完成。",
                    L"Lua 测试", MB_OK | MB_ICONINFORMATION);
        return;
    }

    const std::wstring executableDirectory = GetExecutableDirectory();
    auto request = std::make_unique<LuaTestRequest>();
    request->window = window;
    request->scriptPath = ResolvePath(
        executableDirectory,
        L"..\\..\\..\\Ceshi\\BaogaoFukeCeshi\\baogao_zhuru_ceshi.lua");
    request->bridgePath = executableDirectory + L"LuaJiekouQiaojie.dll";
    if (executableDirectory.empty() || request->scriptPath.empty() ||
        GetFileAttributesW(request->scriptPath.c_str()) == INVALID_FILE_ATTRIBUTES ||
        GetFileAttributesW(request->bridgePath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        MessageBoxW(window,
            L"找不到 Lua 测试脚本或桥接 DLL。\n请确认项目目录和 Release 输出文件完整。",
            L"Lua 测试无法启动", MB_OK | MB_ICONERROR);
        return;
    }

    const uintptr_t thread = _beginthreadex(
        nullptr, 0, LuaTestThreadProc, request.get(), 0, nullptr);
    if (thread == 0) {
        MessageBoxW(window, L"无法创建 Lua 测试工作线程。",
                    L"Lua 测试无法启动", MB_OK | MB_ICONERROR);
        return;
    }
    request.release();
    g_luaTestThread = reinterpret_cast<HANDLE>(thread);
    EnableMenuItem(g_testMenu, kRunLuaTestCommand, MF_BYCOMMAND | MF_GRAYED);
    DrawMenuBar(window);
}

void ReportGraphicsFailure(const wchar_t* operation, HRESULT result) {
    if (g_renderFailed) return;
    g_renderFailed = true;
    swprintf_s(g_graphicsStatus, L"%s 失败 (0x%08X)", operation,
               static_cast<unsigned int>(result));
    UpdateWindowTitle();
}

void SetFrameSubmissionStopped(bool stopped) {
    g_frameStopped = stopped;
    CheckMenuItem(GetSubMenu(GetMenu(g_window), 0), kToggleFrameCommand,
                  MF_BYCOMMAND | (stopped ? MF_CHECKED : MF_UNCHECKED));
    QueryPerformanceCounter(&g_previousTime);
    UpdateWindowTitle();
}

bool CompileShader(const char* source, const char* entry, const char* target,
                   ComPtr<ID3DBlob>& bytecode) {
    ComPtr<ID3DBlob> errors;
    const HRESULT result = D3DCompile(source, std::strlen(source), "embedded-shader",
        nullptr, nullptr, entry, target, D3DCOMPILE_ENABLE_STRICTNESS, 0,
        &bytecode, &errors);
    if (FAILED(result)) {
        if (errors) OutputDebugStringA(static_cast<const char*>(errors->GetBufferPointer()));
        ReportGraphicsFailure(L"编译着色器", result);
        return false;
    }
    return true;
}

bool CreateTextTexture() {
    BITMAPINFO bitmapInfo{};
    bitmapInfo.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bitmapInfo.bmiHeader.biWidth = kSceneWidth;
    bitmapInfo.bmiHeader.biHeight = -kSceneHeight;
    bitmapInfo.bmiHeader.biPlanes = 1;
    bitmapInfo.bmiHeader.biBitCount = 32;
    bitmapInfo.bmiHeader.biCompression = BI_RGB;

    HDC screenDc = GetDC(g_window);
    HDC memoryDc = CreateCompatibleDC(screenDc);
    void* dibPixels{};
    HBITMAP bitmap = CreateDIBSection(screenDc, &bitmapInfo, DIB_RGB_COLORS,
                                      &dibPixels, nullptr, 0);
    ReleaseDC(g_window, screenDc);
    if (!memoryDc || !bitmap || !dibPixels) {
        if (bitmap) DeleteObject(bitmap);
        if (memoryDc) DeleteDC(memoryDc);
        return false;
    }

    const HGDIOBJ previousBitmap = SelectObject(memoryDc, bitmap);
    RECT fullBounds{0, 0, kSceneWidth, kSceneHeight};
    FillRect(memoryDc, &fullBounds, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
    SetBkMode(memoryDc, TRANSPARENT);
    HFONT titleFont = CreateFontW(34, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
    HFONT labelFont = CreateFontW(17, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
    HFONT bodyFont = CreateFontW(15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
    if (!titleFont || !labelFont || !bodyFont) {
        if (titleFont) DeleteObject(titleFont);
        if (labelFont) DeleteObject(labelFont);
        if (bodyFont) DeleteObject(bodyFont);
        SelectObject(memoryDc, previousBitmap);
        DeleteObject(bitmap);
        DeleteDC(memoryDc);
        return false;
    }

    const auto drawText = [&](HFONT font, const wchar_t* text, int x, int y, COLORREF color) {
        const HFONT previousFont = static_cast<HFONT>(SelectObject(memoryDc, font));
        SetTextColor(memoryDc, color);
        RECT bounds{x, y, kSceneWidth - 36, y + 48};
        DrawTextW(memoryDc, text, -1, &bounds, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX);
        SelectObject(memoryDc, previousFont);
    };
    drawText(titleFont, L"DirectX 11   /   动画演示", 48, 34, RGB(239, 245, 250));
    drawText(bodyFont, L"静态构图 · 实时动画 · 中文文字", 50, 82, RGB(133, 169, 182));
    drawText(labelFont, L"01  静态场景", 52, 152, RGB(80, 218, 193));
    drawText(labelFont, L"02  实时动画", 368, 152, RGB(255, 192, 100));
    drawText(labelFont, L"03  操作说明", 686, 152, RGB(116, 180, 255));
    drawText(bodyFont, L"网格与外框固定呈现", 52, 188, RGB(196, 210, 220));
    drawText(bodyFont, L"立方体、轨道、波形持续运动", 368, 188, RGB(196, 210, 220));
    drawText(bodyFont, L"空格 暂停动画", 686, 188, RGB(196, 210, 220));
    drawText(bodyFont, L"F8 停止 / 恢复帧提交", 686, 217, RGB(196, 210, 220));
    drawText(bodyFont, L"方向键 调整速度", 686, 246, RGB(196, 210, 220));
    drawText(bodyFont, L"R 恢复初始状态   Esc 退出", 686, 275, RGB(196, 210, 220));
    drawText(bodyFont, L"900, 24, 920, 44", 730, 51, RGB(235, 242, 248));
    drawText(labelFont, L"空间坐标 / 静态网格", 52, 541, RGB(80, 218, 193));
    drawText(labelFont, L"动画状态 / 帧率", 368, 541, RGB(255, 192, 100));
    DeleteObject(titleFont);
    DeleteObject(labelFont);
    DeleteObject(bodyFont);

    // 将 GDI 灰度文字转成白色 RGB 和独立 alpha，供 DX11 透明混合。
    std::vector<uint32_t> pixels(size_t(kSceneWidth) * kSceneHeight);
    const auto* source = static_cast<const BYTE*>(dibPixels);
    for (size_t i = 0; i < pixels.size(); ++i) {
        const BYTE* pixel = source + i * 4;
        const BYTE alpha = static_cast<BYTE>((unsigned(pixel[0]) +
            unsigned(pixel[1]) + unsigned(pixel[2])) / 3);
        pixels[i] = (uint32_t(alpha) << 24) | 0x00ffffffu;
    }

    D3D11_TEXTURE2D_DESC description{};
    description.Width = kSceneWidth;
    description.Height = kSceneHeight;
    description.MipLevels = 1;
    description.ArraySize = 1;
    description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    description.SampleDesc.Count = 1;
    description.Usage = D3D11_USAGE_IMMUTABLE;
    description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA initial{};
    initial.pSysMem = pixels.data();
    initial.SysMemPitch = kSceneWidth * sizeof(uint32_t);
    ComPtr<ID3D11Texture2D> texture;
    HRESULT result = g_device->CreateTexture2D(&description, &initial, &texture);
    if (SUCCEEDED(result)) result = g_device->CreateShaderResourceView(
        texture.Get(), nullptr, &g_textView);

    SelectObject(memoryDc, previousBitmap);
    DeleteObject(bitmap);
    DeleteDC(memoryDc);
    if (FAILED(result)) ReportGraphicsFailure(L"创建文字纹理", result);
    return SUCCEEDED(result);
}

bool CreateDevice() {
    DXGI_SWAP_CHAIN_DESC swapDescription{};
    swapDescription.BufferDesc.Width = static_cast<UINT>(g_width);
    swapDescription.BufferDesc.Height = static_cast<UINT>(g_height);
    swapDescription.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    swapDescription.SampleDesc.Count = 1;
    swapDescription.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapDescription.BufferCount = 1;
    swapDescription.OutputWindow = g_window;
    swapDescription.Windowed = TRUE;
    swapDescription.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    UINT flags = 0;
#if defined(_DEBUG)
    flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
    D3D_FEATURE_LEVEL selected{};
    const D3D_FEATURE_LEVEL levels[] = {
        D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0
    };
    HRESULT result = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE,
        nullptr, flags, levels, static_cast<UINT>(std::size(levels)), D3D11_SDK_VERSION,
        &swapDescription, &g_swapChain, &g_device, &selected, &g_context);
#if defined(_DEBUG)
    if (result == DXGI_ERROR_SDK_COMPONENT_MISSING) {
        flags &= ~D3D11_CREATE_DEVICE_DEBUG;
        result = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE,
            nullptr, flags, levels, static_cast<UINT>(std::size(levels)), D3D11_SDK_VERSION,
            &swapDescription, &g_swapChain, &g_device, &selected, &g_context);
    }
#endif
    if (FAILED(result)) {
        result = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP,
            nullptr, flags & ~D3D11_CREATE_DEVICE_DEBUG, levels,
            static_cast<UINT>(std::size(levels)), D3D11_SDK_VERSION,
            &swapDescription, &g_swapChain, &g_device, &selected, &g_context);
    }
    if (FAILED(result)) {
        ReportGraphicsFailure(L"创建 DX11 设备", result);
        return false;
    }

    constexpr char shader[] =
        "cbuffer Screen : register(b0) { float2 size; float2 padding; };"
        "struct Input { float2 position : POSITION; float4 color : COLOR; float2 uv : TEXCOORD; };"
        "struct Output { float4 position : SV_POSITION; float4 color : COLOR; float2 uv : TEXCOORD; };"
        "Output VSMain(Input input) { Output o;"
        "o.position=float4(input.position.x/size.x*2-1,1-input.position.y/size.y*2,0,1);"
        "o.color=input.color; o.uv=input.uv; return o; }"
        "float4 PSColor(Output input) : SV_TARGET { return input.color; }"
        "Texture2D image : register(t0); SamplerState imageSampler : register(s0);"
        "float4 PSTexture(Output input) : SV_TARGET { return image.Sample(imageSampler,input.uv)*input.color; }";
    ComPtr<ID3DBlob> vertexBytecode, colorBytecode, textureBytecode;
    if (!CompileShader(shader, "VSMain", "vs_4_0", vertexBytecode) ||
        !CompileShader(shader, "PSColor", "ps_4_0", colorBytecode) ||
        !CompileShader(shader, "PSTexture", "ps_4_0", textureBytecode)) return false;
    if (FAILED(g_device->CreateVertexShader(vertexBytecode->GetBufferPointer(),
            vertexBytecode->GetBufferSize(), nullptr, &g_vertexShader)) ||
        FAILED(g_device->CreatePixelShader(colorBytecode->GetBufferPointer(),
            colorBytecode->GetBufferSize(), nullptr, &g_colorPixelShader)) ||
        FAILED(g_device->CreatePixelShader(textureBytecode->GetBufferPointer(),
            textureBytecode->GetBufferSize(), nullptr, &g_texturePixelShader))) {
        ReportGraphicsFailure(L"创建 DX11 着色器", E_FAIL);
        return false;
    }

    const D3D11_INPUT_ELEMENT_DESC elements[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24, D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    if (FAILED(g_device->CreateInputLayout(elements, static_cast<UINT>(std::size(elements)),
            vertexBytecode->GetBufferPointer(), vertexBytecode->GetBufferSize(), &g_inputLayout))) {
        ReportGraphicsFailure(L"创建顶点布局", E_FAIL);
        return false;
    }

    D3D11_BUFFER_DESC bufferDescription{};
    bufferDescription.ByteWidth = kVertexCapacity * sizeof(Vertex);
    bufferDescription.Usage = D3D11_USAGE_DYNAMIC;
    bufferDescription.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    bufferDescription.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(g_device->CreateBuffer(&bufferDescription, nullptr, &g_vertices))) return false;
    bufferDescription = {};
    bufferDescription.ByteWidth = sizeof(ScreenConstants);
    bufferDescription.Usage = D3D11_USAGE_DEFAULT;
    bufferDescription.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    if (FAILED(g_device->CreateBuffer(&bufferDescription, nullptr, &g_screenConstants))) return false;

    D3D11_SAMPLER_DESC samplerDescription{};
    samplerDescription.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    samplerDescription.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDescription.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDescription.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDescription.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(g_device->CreateSamplerState(&samplerDescription, &g_textSampler))) return false;

    D3D11_BLEND_DESC blendDescription{};
    auto& blend = blendDescription.RenderTarget[0];
    blend.BlendEnable = TRUE;
    blend.SrcBlend = D3D11_BLEND_SRC_ALPHA;
    blend.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    blend.BlendOp = D3D11_BLEND_OP_ADD;
    blend.SrcBlendAlpha = D3D11_BLEND_ONE;
    blend.DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    blend.BlendOpAlpha = D3D11_BLEND_OP_ADD;
    blend.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (FAILED(g_device->CreateBlendState(&blendDescription, &g_alphaBlend))) return false;

    D3D11_RASTERIZER_DESC rasterDescription{};
    rasterDescription.FillMode = D3D11_FILL_SOLID;
    rasterDescription.CullMode = D3D11_CULL_NONE;
    rasterDescription.DepthClipEnable = TRUE;
    ComPtr<ID3D11RasterizerState> rasterizer;
    result = g_device->CreateRasterizerState(&rasterDescription, &rasterizer);
    if (FAILED(result)) {
        ReportGraphicsFailure(L"创建光栅化状态", result);
        return false;
    }
    g_context->RSSetState(rasterizer.Get());

    ComPtr<ID3D11Texture2D> backBuffer;
    result = g_swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
    if (SUCCEEDED(result)) result = g_device->CreateRenderTargetView(
        backBuffer.Get(), nullptr, &g_renderTarget);
    if (FAILED(result) || !CreateTextTexture()) {
        ReportGraphicsFailure(L"创建 DX11 渲染目标", FAILED(result) ? result : E_FAIL);
        return false;
    }
    g_context->OMSetRenderTargets(1, g_renderTarget.GetAddressOf(), nullptr);
    return true;
}

Vertex MakeVertex(float x, float y, float r, float g, float b, float a = 1.0f,
                  float u = 0.0f, float v = 0.0f) {
    return {x, y, r, g, b, a, u, v};
}

void AddLine(std::vector<Vertex>& vertices, float x1, float y1, float x2, float y2,
             float r, float g, float b, float a = 1.0f) {
    vertices.push_back(MakeVertex(x1, y1, r, g, b, a));
    vertices.push_back(MakeVertex(x2, y2, r, g, b, a));
}

void AddRectangle(std::vector<Vertex>& vertices, float left, float top, float right,
                  float bottom, float r, float g, float b, float a = 1.0f) {
    const auto tl = MakeVertex(left, top, r, g, b, a);
    const auto tr = MakeVertex(right, top, r, g, b, a);
    const auto bl = MakeVertex(left, bottom, r, g, b, a);
    const auto br = MakeVertex(right, bottom, r, g, b, a);
    vertices.insert(vertices.end(), {tl, tr, bl, bl, tr, br});
}

void AddCircle(std::vector<Vertex>& vertices, float centerX, float centerY, float radius,
               float r, float g, float b, float a = 1.0f, int segments = 32) {
    const auto center = MakeVertex(centerX, centerY, r, g, b, a);
    for (int i = 0; i < segments; ++i) {
        const float first = 2.0f * kPi * i / segments;
        const float second = 2.0f * kPi * (i + 1) / segments;
        vertices.push_back(center);
        vertices.push_back(MakeVertex(centerX + std::cos(first) * radius,
            centerY + std::sin(first) * radius, r, g, b, a));
        vertices.push_back(MakeVertex(centerX + std::cos(second) * radius,
            centerY + std::sin(second) * radius, r, g, b, a));
    }
}

void AddCircleOutline(std::vector<Vertex>& vertices, float centerX, float centerY,
                      float radius, float r, float g, float b, float a = 1.0f,
                      int segments = 64) {
    for (int i = 0; i < segments; ++i) {
        const float first = 2.0f * kPi * i / segments;
        const float second = 2.0f * kPi * (i + 1) / segments;
        AddLine(vertices, centerX + std::cos(first) * radius,
            centerY + std::sin(first) * radius,
            centerX + std::cos(second) * radius,
            centerY + std::sin(second) * radius, r, g, b, a);
    }
}

void DrawBatch(const std::vector<Vertex>& vertices, D3D11_PRIMITIVE_TOPOLOGY topology,
               ID3D11PixelShader* pixelShader, bool alphaBlend) {
    if (vertices.empty() || vertices.size() > kVertexCapacity) return;
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(g_context->Map(g_vertices.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        ReportGraphicsFailure(L"更新顶点数据", E_FAIL);
        return;
    }
    std::memcpy(mapped.pData, vertices.data(), vertices.size() * sizeof(Vertex));
    g_context->Unmap(g_vertices.Get(), 0);
    const UINT stride = sizeof(Vertex), offset = 0;
    ID3D11Buffer* buffer = g_vertices.Get();
    g_context->IASetVertexBuffers(0, 1, &buffer, &stride, &offset);
    g_context->IASetPrimitiveTopology(topology);
    g_context->PSSetShader(pixelShader, nullptr, 0);
    if (alphaBlend) {
        constexpr float factors[4]{};
        g_context->OMSetBlendState(g_alphaBlend.Get(), factors, 0xffffffff);
    } else {
        g_context->OMSetBlendState(nullptr, nullptr, 0xffffffff);
    }
    g_context->Draw(static_cast<UINT>(vertices.size()), 0);
}

void DrawStaticScene() {
    std::vector<Vertex> lines;
    lines.reserve(96);
    for (int x = 32; x <= 928; x += 32)
        AddLine(lines, float(x), 142, float(x), 522, .12f, .22f, .25f, .65f);
    for (int y = 158; y <= 510; y += 32)
        AddLine(lines, 32, float(y), 928, float(y), .12f, .22f, .25f, .65f);
    AddLine(lines, 32, 140, 928, 140, .21f, .36f, .41f);
    AddLine(lines, 32, 522, 928, 522, .21f, .36f, .41f);
    AddLine(lines, 344, 150, 344, 512, .21f, .36f, .41f);
    AddLine(lines, 666, 150, 666, 512, .21f, .36f, .41f);
    AddCircleOutline(lines, 184, 354, 96, .31f, .85f, .76f, .62f);
    AddCircleOutline(lines, 184, 354, 70, .45f, .71f, 1.0f, .45f);
    AddLine(lines, 88, 354, 280, 354, .31f, .85f, .76f, .55f);
    AddLine(lines, 184, 258, 184, 450, .31f, .85f, .76f, .55f);

    std::vector<Vertex> blocks;
    blocks.reserve(48);
    AddRectangle(blocks, 32, 140, 928, 142, .21f, .36f, .41f);
    AddRectangle(blocks, 32, 520, 928, 522, .21f, .36f, .41f);
    AddRectangle(blocks, 344, 150, 346, 512, .21f, .36f, .41f);
    AddRectangle(blocks, 664, 150, 666, 512, .21f, .36f, .41f);
    AddRectangle(blocks, 48, 177, 58, 185, .31f, .85f, .76f);
    AddRectangle(blocks, 364, 177, 374, 185, 1.0f, .75f, .39f);
    AddRectangle(blocks, 682, 177, 692, 185, .45f, .71f, 1.0f);
    // 在画面右上角绘制四块 10×10 像素色块，总面积为 400 像素；
    // 标注顺序为左、上、右、下边界，坐标以画面左上角为原点。
    AddRectangle(blocks, 900, 24, 910, 34, .31f, .85f, .76f);
    AddRectangle(blocks, 910, 24, 920, 34, 1.0f, .75f, .39f);
    AddRectangle(blocks, 900, 34, 910, 44, .45f, .71f, 1.0f);
    AddRectangle(blocks, 910, 34, 920, 44, .91f, .38f, .43f);
    DrawBatch(lines, D3D11_PRIMITIVE_TOPOLOGY_LINELIST, g_colorPixelShader.Get(), false);
    DrawBatch(blocks, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST, g_colorPixelShader.Get(), false);
}

void DrawAnimatedScene() {
    constexpr float centerX = 505.0f, centerY = 342.0f, half = 61.0f;
    const float cosine = std::cos(g_angle), sine = std::sin(g_angle);
    const float tilt = std::cos(g_angle * .63f) * .42f;
    const float points[8][3] = {
        {-half,-half,-half}, {half,-half,-half}, {half,half,-half}, {-half,half,-half},
        {-half,-half,half}, {half,-half,half}, {half,half,half}, {-half,half,half},
    };
    float projected[8][2]{};
    for (int i = 0; i < 8; ++i) {
        const float x = points[i][0] * cosine - points[i][2] * sine;
        const float z = points[i][0] * sine + points[i][2] * cosine;
        projected[i][0] = centerX + x;
        projected[i][1] = centerY + points[i][1] + z * tilt;
    }

    std::vector<Vertex> lines;
    lines.reserve(256);
    const int edges[12][2] = {
        {0,1},{1,2},{2,3},{3,0},{4,5},{5,6},{6,7},{7,4},{0,4},{1,5},{2,6},{3,7},
    };
    for (const auto& edge : edges) {
        const bool front = edge[0] < 4;
        AddLine(lines, projected[edge[0]][0], projected[edge[0]][1],
            projected[edge[1]][0], projected[edge[1]][1],
            front ? .31f : 1.0f, front ? .85f : .75f, front ? .76f : .39f);
    }
    const float orbitAngle = g_angle * 1.45f;
    const float orbitX = centerX + std::cos(orbitAngle) * 115.0f;
    const float orbitY = centerY + std::sin(orbitAngle * .82f) * 73.0f;
    AddLine(lines, 376, 489, 643, 489, .52f, .66f, .71f, .72f);
    DrawBatch(lines, D3D11_PRIMITIVE_TOPOLOGY_LINELIST, g_colorPixelShader.Get(), true);

    std::vector<Vertex> blocks;
    blocks.reserve(48 * 6 + 64 * 3 + 6);
    for (int bar = 0; bar < 48; ++bar) {
        const float wave = std::sin(g_angle * 2.1f + bar * .31f);
        const float secondary = std::cos(g_angle * 1.2f + bar * .17f);
        const float height = 8.0f + wave * wave * 28.0f + (1.0f + secondary) * 7.0f;
        const float left = 379.0f + bar * 5.7f;
        const bool amber = bar % 3 == 0;
        AddRectangle(blocks, left, 488.0f - height, left + 3.4f, 488.0f,
            amber ? 1.0f : .31f, amber ? .75f : .85f, amber ? .39f : .76f);
    }
    AddCircle(blocks, orbitX, orbitY, 9, 1.0f, .75f, .39f);
    AddCircle(blocks, orbitX, orbitY, 17, 1.0f, .75f, .39f, .22f);
    const float markerX = 80.0f + static_cast<float>(std::fmod(g_elapsedSeconds * 45.0, 225.0));
    AddCircle(blocks, markerX, 474, 4, 1.0f, .75f, .39f);
    DrawBatch(blocks, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST, g_colorPixelShader.Get(), true);
}

void DrawTextTexture() {
    const std::vector<Vertex> quad{
        MakeVertex(0, 0, 1, 1, 1, 1, 0, 0),
        MakeVertex(kSceneWidth, 0, 1, 1, 1, 1, 1, 0),
        MakeVertex(0, kSceneHeight, 1, 1, 1, 1, 0, 1),
        MakeVertex(0, kSceneHeight, 1, 1, 1, 1, 0, 1),
        MakeVertex(kSceneWidth, 0, 1, 1, 1, 1, 1, 0),
        MakeVertex(kSceneWidth, kSceneHeight, 1, 1, 1, 1, 1, 1),
    };
    ID3D11ShaderResourceView* view = g_textView.Get();
    ID3D11SamplerState* sampler = g_textSampler.Get();
    g_context->PSSetShaderResources(0, 1, &view);
    g_context->PSSetSamplers(0, 1, &sampler);
    DrawBatch(quad, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST,
              g_texturePixelShader.Get(), true);
}

bool ResizeSwapChain() {
    g_context->OMSetRenderTargets(0, nullptr, nullptr);
    g_renderTarget.Reset();
    HRESULT result = g_swapChain->ResizeBuffers(0, static_cast<UINT>(g_width),
        static_cast<UINT>(g_height), DXGI_FORMAT_UNKNOWN, 0);
    if (SUCCEEDED(result)) {
        ComPtr<ID3D11Texture2D> backBuffer;
        result = g_swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
        if (SUCCEEDED(result)) result = g_device->CreateRenderTargetView(
            backBuffer.Get(), nullptr, &g_renderTarget);
    }
    if (FAILED(result)) {
        ReportGraphicsFailure(L"调整交换链尺寸", result);
        return false;
    }
    g_resizePending = false;
    g_renderFailed = false;
    wcscpy_s(g_graphicsStatus, L"DX11 正常");
    return true;
}

void RenderFrame(double deltaSeconds) {
    if (g_frameStopped || !g_device || g_width <= 0 || g_height <= 0) return;
    g_elapsedSeconds += deltaSeconds;
    if (!g_paused) g_angle += static_cast<float>(deltaSeconds) * g_speed;

    if (g_resizePending && !ResizeSwapChain()) return;
    constexpr float background[4]{.051f, .078f, .114f, 1.0f};
    g_context->OMSetRenderTargets(1, g_renderTarget.GetAddressOf(), nullptr);
    g_context->ClearRenderTargetView(g_renderTarget.Get(), background);

    const D3D11_VIEWPORT viewport{0, 0, static_cast<float>(g_width),
        static_cast<float>(g_height), 0, 1};
    g_context->RSSetViewports(1, &viewport);
    const ScreenConstants screen{static_cast<float>(g_width), static_cast<float>(g_height), {0, 0}};
    g_context->UpdateSubresource(g_screenConstants.Get(), 0, nullptr, &screen, 0, 0);
    ID3D11Buffer* constants = g_screenConstants.Get();
    g_context->VSSetConstantBuffers(0, 1, &constants);
    g_context->IASetInputLayout(g_inputLayout.Get());
    g_context->VSSetShader(g_vertexShader.Get(), nullptr, 0);
    ID3D11SamplerState* sampler = g_textSampler.Get();
    g_context->PSSetSamplers(0, 1, &sampler);

    DrawStaticScene();
    DrawAnimatedScene();
    DrawTextTexture();
    // D3D11 要求传入视图指针数组；数组中放空视图才能安全解除纹理绑定。
    ID3D11ShaderResourceView* noView = nullptr;
    g_context->PSSetShaderResources(0, 1, &noView);
    const HRESULT result = g_swapChain->Present(1, 0);
    if (FAILED(result)) ReportGraphicsFailure(L"DXGI Present", result);
}

void UpdateWindowTitle() {
    wchar_t title[256]{};
    swprintf_s(title, L"DirectX 11 动画演示 | 桥接已就绪 | %s | %s | 速度 %.1fx | %s",
        g_frameStopped ? L"帧提交已停止" : L"帧提交中",
        g_paused ? L"已暂停" : L"播放中", g_speed, g_graphicsStatus);
    SetWindowTextW(g_window, title);
}

LRESULT CALLBACK WindowProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_CLOSE:
        if (g_luaTestThread) {
            MessageBoxW(window, L"Lua 测试仍在运行，请等它结束后再关闭程序。",
                        L"Lua 测试运行中", MB_OK | MB_ICONINFORMATION);
            return 0;
        }
        DestroyWindow(window);
        return 0;
    case WM_SIZE:
        if (wParam != SIZE_MINIMIZED && g_device) {
            g_width = LOWORD(lParam);
            g_height = HIWORD(lParam);
            g_resizePending = g_width > 0 && g_height > 0;
        }
        return 0;
    case WM_COMMAND:
        if (LOWORD(wParam) == kToggleFrameCommand && HIWORD(wParam) == 0) {
            SetFrameSubmissionStopped(!g_frameStopped);
            return 0;
        }
        if (LOWORD(wParam) == kRunLuaTestCommand && HIWORD(wParam) == 0) {
            StartLuaTest(window);
            return 0;
        }
        break;
    case kLuaTestFinishedMessage: {
        std::unique_ptr<LuaTestCompletion> completion(
            reinterpret_cast<LuaTestCompletion*>(wParam));
        if (g_luaTestThread) {
            // 工作线程投递完成消息后即将返回；等它彻底退出，再允许宿主
            // 释放桥接 DLL，避免测试代码仍在使用 DLL 时发生卸载。
            WaitForSingleObject(g_luaTestThread, INFINITE);
            CloseHandle(g_luaTestThread);
            g_luaTestThread = nullptr;
        }
        if (g_testMenu) {
            EnableMenuItem(g_testMenu, kRunLuaTestCommand,
                           MF_BYCOMMAND | MF_ENABLED);
            DrawMenuBar(window);
        }
        if (completion) {
            MessageBoxW(window, completion->message.c_str(),
                completion->resultCode == 0 ? L"Lua 测试完成" : L"Lua 测试结束",
                completion->resultCode == 0 ? MB_OK | MB_ICONINFORMATION
                                            : MB_OK | MB_ICONWARNING);
        }
        return 0;
    }
    case WM_KEYDOWN:
        switch (wParam) {
        case VK_ESCAPE:
            SendMessageW(window, WM_CLOSE, 0, 0);
            return 0;
        case VK_F8:
            if ((lParam & (LPARAM(1) << 30)) == 0)
                SetFrameSubmissionStopped(!g_frameStopped);
            return 0;
        case VK_SPACE:
            g_paused = !g_paused;
            UpdateWindowTitle();
            return 0;
        case VK_LEFT:
            g_speed = (g_speed <= .26f) ? .25f : g_speed - .25f;
            UpdateWindowTitle();
            return 0;
        case VK_RIGHT:
            g_speed = (g_speed >= 3.75f) ? 4.0f : g_speed + .25f;
            UpdateWindowTitle();
            return 0;
        case 'R':
            g_angle = 0;
            g_elapsedSeconds = 0;
            g_speed = 1;
            g_paused = false;
            SetFrameSubmissionStopped(false);
            return 0;
        }
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

bool CreateWindowMenu(HMENU& menu) {
    menu = CreateMenu();
    HMENU controls = CreatePopupMenu();
    HMENU tests = CreatePopupMenu();
    if (!menu || !controls || !tests) {
        if (tests) DestroyMenu(tests);
        if (controls) DestroyMenu(controls);
        if (menu) DestroyMenu(menu);
        return false;
    }
    if (!AppendMenuW(tests, MF_STRING, kRunLuaTestCommand, L"运行 Lua 报告测试")) {
        DestroyMenu(tests);
        DestroyMenu(controls);
        DestroyMenu(menu);
        return false;
    }
    if (!AppendMenuW(controls, MF_STRING, kToggleFrameCommand, L"停止帧提交\tF8")) {
        DestroyMenu(tests);
        DestroyMenu(controls);
        DestroyMenu(menu);
        return false;
    }
    if (!AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(controls), L"控制")) {
        DestroyMenu(tests);
        DestroyMenu(controls);
        DestroyMenu(menu);
        return false;
    }
    if (!AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(tests), L"测试")) {
        DestroyMenu(tests);
        DestroyMenu(menu);
        return false;
    }
    g_testMenu = tests;
    return true;
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCommand) {
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.style = CS_HREDRAW | CS_VREDRAW;
    windowClass.lpfnWndProc = WindowProcedure;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    windowClass.lpszClassName = L"Dx11DonghuaWindow";
    if (!RegisterClassExW(&windowClass)) return 1;

    HMENU menu{};
    if (!CreateWindowMenu(menu)) return 1;
    RECT bounds{0, 0, kSceneWidth, kSceneHeight};
    AdjustWindowRect(&bounds, WS_OVERLAPPEDWINDOW, TRUE);
    g_window = CreateWindowExW(0, windowClass.lpszClassName, L"DirectX 11 动画演示",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
        bounds.right - bounds.left, bounds.bottom - bounds.top,
        nullptr, menu, instance, nullptr);
    if (!g_window) {
        DestroyMenu(menu);
        return 1;
    }
    if (!CreateDevice()) {
        MessageBoxW(g_window, L"无法初始化 DirectX 11 图形设备。",
                    L"初始化失败", MB_OK | MB_ICONERROR);
        DestroyWindow(g_window);
        return 2;
    }

    std::wstring bridgeError;
    if (!InitializeBridge(bridgeError)) {
        MessageBoxW(g_window, bridgeError.c_str(), L"桥接 DLL 初始化失败",
                    MB_OK | MB_ICONERROR);
        DestroyWindow(g_window);
        return 3;
    }

    QueryPerformanceFrequency(&g_clockFrequency);
    QueryPerformanceCounter(&g_previousTime);
    ShowWindow(g_window, showCommand);
    UpdateWindow(g_window);
    UpdateWindowTitle();

    MSG message{};
    bool running = true;
    while (running) {
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            if (message.message == WM_QUIT) {
                running = false;
                break;
            }
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        if (!running) break;
        if (g_frameStopped) {
            MsgWaitForMultipleObjectsEx(0, nullptr, INFINITE, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
            continue;
        }
        LARGE_INTEGER current{};
        QueryPerformanceCounter(&current);
        double delta = static_cast<double>(current.QuadPart - g_previousTime.QuadPart) /
                       g_clockFrequency.QuadPart;
        g_previousTime = current;
        if (delta > .1) delta = .1;
        RenderFrame(delta);
        Sleep(1);
    }
    ShutdownBridge();
    return static_cast<int>(message.wParam);
}
