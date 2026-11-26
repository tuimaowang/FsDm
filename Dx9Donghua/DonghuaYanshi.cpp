// DirectX 9 图形与 GDI 中文文字叠加演示。
// 静态装饰由固定图元组成，旋转立方体、轨道光点和波形按高精度计时器更新。
// 中文文字先由 Windows GDI 绘制到内存位图，再转换为带透明度的 Direct3D 纹理。
#include <windows.h>
#include <d3d9.h>
#include <cmath>
#include <cstdint>
#include <vector>

#pragma comment(lib, "d3d9.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")

namespace {

constexpr int kSceneWidth = 960;
constexpr int kSceneHeight = 640;
constexpr float kPi = 3.14159265358979323846f;
constexpr DWORD kVertexFormat = D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1;
constexpr UINT kToggleFrameCommand = 1001;

struct Vertex {
    float x;
    float y;
    float z;
    float rhw;
    DWORD color;
    float u;
    float v;
};

HWND g_window = nullptr;
IDirect3D9* g_direct3D = nullptr;
IDirect3DDevice9* g_device = nullptr;
IDirect3DTexture9* g_textTexture = nullptr;
int g_width = kSceneWidth;
int g_height = kSceneHeight;
bool g_paused = false;
// 停止提交时冻结整个场景，主循环只处理窗口消息。
// 缩放请求保留到恢复后再重置设备，避免暂停期间清空正在观察的画面。
bool g_frameStopped = false;
bool g_resizePending = false;
float g_angle = 0.0f;
float g_speed = 1.0f;
LARGE_INTEGER g_clockFrequency = {};
LARGE_INTEGER g_previousTime = {};
double g_elapsedSeconds = 0.0;
bool g_renderFailed = false;
wchar_t g_graphicsStatus[128] = L"DX9 正常";

void UpdateWindowTitle();

void SetFrameSubmissionStopped(bool stopped) {
    g_frameStopped = stopped;
    CheckMenuItem(GetSubMenu(GetMenu(g_window), 0), kToggleFrameCommand,
                  MF_BYCOMMAND | (stopped ? MF_CHECKED : MF_UNCHECKED));
    // 从切换时刻重新计时，恢复时不会把等待期间的时长计入动画。
    QueryPerformanceCounter(&g_previousTime);
    UpdateWindowTitle();
}

void ReportGraphicsFailure(const wchar_t* operation, HRESULT result) {
    if (g_renderFailed) {
        return;
    }
    g_renderFailed = true;
    swprintf_s(g_graphicsStatus, L"%s 失败 (0x%08X)", operation,
               static_cast<unsigned int>(result));
    UpdateWindowTitle();
}

// Reset 会恢复 DX9 的默认状态，其中源/目标混合因子默认是 ONE/ZERO。
// 只重新启用透明混合还不够：ONE/ZERO 会让文字纹理的白色覆盖整个窗口，
// 即使该处的透明度为零。每帧完整恢复本程序依赖的状态，同时覆盖设备重置路径。
bool RestoreRenderStates() {
    const struct {
        D3DRENDERSTATETYPE name;
        DWORD value;
    } states[] = {
        {D3DRS_LIGHTING, FALSE},
        {D3DRS_CULLMODE, D3DCULL_NONE},
        {D3DRS_ZENABLE, D3DZB_FALSE},
        {D3DRS_ALPHATESTENABLE, FALSE},
        {D3DRS_ALPHABLENDENABLE, TRUE},
        {D3DRS_SRCBLEND, D3DBLEND_SRCALPHA},
        {D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA},
        {D3DRS_BLENDOP, D3DBLENDOP_ADD},
        {D3DRS_SEPARATEALPHABLENDENABLE, FALSE},
    };
    for (const auto& state : states) {
        const HRESULT result = g_device->SetRenderState(state.name, state.value);
        if (FAILED(result)) {
            ReportGraphicsFailure(L"恢复渲染状态", result);
            return false;
        }
    }
    const D3DSAMPLERSTATETYPE samplers[] = {D3DSAMP_MINFILTER, D3DSAMP_MAGFILTER};
    for (const auto sampler : samplers) {
        const HRESULT result = g_device->SetSamplerState(0, sampler, D3DTEXF_LINEAR);
        if (FAILED(result)) {
            ReportGraphicsFailure(L"恢复文字采样状态", result);
            return false;
        }
    }
    return true;
}

float ScaleX(float value) {
    return value * static_cast<float>(g_width) / kSceneWidth;
}

float ScaleY(float value) {
    return value * static_cast<float>(g_height) / kSceneHeight;
}

Vertex MakeVertex(float x, float y, DWORD color, float u = 0.0f, float v = 0.0f) {
    return {ScaleX(x), ScaleY(y), 0.0f, 1.0f, color, u, v};
}

void DrawVertices(D3DPRIMITIVETYPE type, const std::vector<Vertex>& vertices,
                  UINT primitiveCount) {
    if (g_device == nullptr || vertices.empty() || g_renderFailed) {
        return;
    }
    HRESULT result = g_device->SetFVF(kVertexFormat);
    if (FAILED(result)) {
        ReportGraphicsFailure(L"设置顶点格式", result);
        return;
    }
    result = g_device->DrawPrimitiveUP(type, primitiveCount, vertices.data(), sizeof(Vertex));
    if (FAILED(result)) {
        ReportGraphicsFailure(L"绘制图元", result);
    }
}

void DrawRectangle(float left, float top, float right, float bottom, DWORD color) {
    const std::vector<Vertex> vertices = {
        MakeVertex(left, top, color), MakeVertex(right, top, color),
        MakeVertex(left, bottom, color), MakeVertex(left, bottom, color),
        MakeVertex(right, top, color), MakeVertex(right, bottom, color),
    };
    DrawVertices(D3DPT_TRIANGLELIST, vertices, 2);
}

void DrawLine(float x1, float y1, float x2, float y2, DWORD color) {
    DrawVertices(D3DPT_LINELIST,
                 {MakeVertex(x1, y1, color), MakeVertex(x2, y2, color)}, 1);
}

void DrawCircle(float centerX, float centerY, float radius, DWORD color, int segments = 32) {
    std::vector<Vertex> vertices;
    vertices.reserve(static_cast<size_t>(segments) + 2);
    vertices.push_back(MakeVertex(centerX, centerY, color));
    for (int i = 0; i <= segments; ++i) {
        const float angle = 2.0f * kPi * static_cast<float>(i) / segments;
        vertices.push_back(MakeVertex(centerX + std::cos(angle) * radius,
                                      centerY + std::sin(angle) * radius, color));
    }
    DrawVertices(D3DPT_TRIANGLEFAN, vertices, static_cast<UINT>(segments));
}

void DrawCircleOutline(float centerX, float centerY, float radius, DWORD color,
                       int segments = 64) {
    std::vector<Vertex> vertices;
    vertices.reserve(static_cast<size_t>(segments) * 2);
    for (int i = 0; i < segments; ++i) {
        const float first = 2.0f * kPi * static_cast<float>(i) / segments;
        const float second = 2.0f * kPi * static_cast<float>(i + 1) / segments;
        vertices.push_back(MakeVertex(centerX + std::cos(first) * radius,
                                      centerY + std::sin(first) * radius, color));
        vertices.push_back(MakeVertex(centerX + std::cos(second) * radius,
                                      centerY + std::sin(second) * radius, color));
    }
    DrawVertices(D3DPT_LINELIST, vertices, static_cast<UINT>(segments));
}

void DrawTextLine(HDC dc, HFONT font, const wchar_t* text, int x, int y, COLORREF color) {
    const HFONT previousFont = static_cast<HFONT>(SelectObject(dc, font));
    SetTextColor(dc, color);
    RECT bounds = {x, y, kSceneWidth - 36, y + 48};
    DrawTextW(dc, text, -1, &bounds, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(dc, previousFont);
}

bool CreateTextTexture() {
    BITMAPINFO bitmapInfo = {};
    bitmapInfo.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bitmapInfo.bmiHeader.biWidth = kSceneWidth;
    bitmapInfo.bmiHeader.biHeight = -kSceneHeight;
    bitmapInfo.bmiHeader.biPlanes = 1;
    bitmapInfo.bmiHeader.biBitCount = 32;
    bitmapInfo.bmiHeader.biCompression = BI_RGB;

    HDC screenDc = GetDC(g_window);
    HDC memoryDc = CreateCompatibleDC(screenDc);
    void* dibPixels = nullptr;
    HBITMAP bitmap = CreateDIBSection(screenDc, &bitmapInfo, DIB_RGB_COLORS,
                                      &dibPixels, nullptr, 0);
    ReleaseDC(g_window, screenDc);
    if (memoryDc == nullptr || bitmap == nullptr || dibPixels == nullptr) {
        if (bitmap != nullptr) DeleteObject(bitmap);
        if (memoryDc != nullptr) DeleteDC(memoryDc);
        return false;
    }

    const HGDIOBJ previousBitmap = SelectObject(memoryDc, bitmap);
    RECT fullBounds = {0, 0, kSceneWidth, kSceneHeight};
    FillRect(memoryDc, &fullBounds, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
    SetBkMode(memoryDc, TRANSPARENT);

    HFONT titleFont = CreateFontW(34, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                                  DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                  CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
    HFONT labelFont = CreateFontW(17, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                                  DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                  CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
    HFONT bodyFont = CreateFontW(15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                 DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                 CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
    if (titleFont == nullptr || labelFont == nullptr || bodyFont == nullptr) {
        if (titleFont != nullptr) DeleteObject(titleFont);
        if (labelFont != nullptr) DeleteObject(labelFont);
        if (bodyFont != nullptr) DeleteObject(bodyFont);
        SelectObject(memoryDc, previousBitmap);
        DeleteObject(bitmap);
        DeleteDC(memoryDc);
        return false;
    }

    DrawTextLine(memoryDc, titleFont, L"DirectX 9   /   动画演示", 48, 34,
                 RGB(239, 245, 250));
    DrawTextLine(memoryDc, bodyFont, L"静态构图 · 实时动画 · 中文文字", 50, 82,
                 RGB(133, 169, 182));
    DrawTextLine(memoryDc, labelFont, L"01  静态场景", 52, 152, RGB(80, 218, 193));
    DrawTextLine(memoryDc, labelFont, L"02  实时动画", 368, 152, RGB(255, 192, 100));
    DrawTextLine(memoryDc, labelFont, L"03  操作说明", 686, 152, RGB(116, 180, 255));
    DrawTextLine(memoryDc, bodyFont, L"网格与外框固定呈现", 52, 188, RGB(196, 210, 220));
    DrawTextLine(memoryDc, bodyFont, L"立方体、轨道、波形持续运动", 368, 188,
                 RGB(196, 210, 220));
    DrawTextLine(memoryDc, bodyFont, L"空格 暂停 / 继续", 686, 188, RGB(196, 210, 220));
    DrawTextLine(memoryDc, bodyFont, L"F8 停止 / 恢复帧提交", 686, 217, RGB(196, 210, 220));
    DrawTextLine(memoryDc, bodyFont, L"方向键 调整速度", 686, 246, RGB(196, 210, 220));
    DrawTextLine(memoryDc, bodyFont, L"R 恢复初始状态   Esc 退出", 686, 275,
                 RGB(196, 210, 220));
    DrawTextLine(memoryDc, labelFont, L"空间坐标 / 静态网格", 52, 541, RGB(80, 218, 193));
    DrawTextLine(memoryDc, labelFont, L"动画状态 / 帧率", 368, 541, RGB(255, 192, 100));

    DeleteObject(titleFont);
    DeleteObject(labelFont);
    DeleteObject(bodyFont);

    std::vector<DWORD> texturePixels(static_cast<size_t>(kSceneWidth) * kSceneHeight);
    const auto* sourcePixels = static_cast<const BYTE*>(dibPixels);
    for (size_t i = 0; i < texturePixels.size(); ++i) {
        const BYTE* pixel = sourcePixels + i * 4;
        const BYTE alpha = (static_cast<unsigned int>(pixel[0]) +
                            static_cast<unsigned int>(pixel[1]) +
                            static_cast<unsigned int>(pixel[2])) / 3;
        texturePixels[i] = D3DCOLOR_ARGB(alpha, 255, 255, 255);
    }

    bool succeeded = SUCCEEDED(g_device->CreateTexture(
        kSceneWidth, kSceneHeight, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED,
        &g_textTexture, nullptr));
    if (succeeded) {
        D3DLOCKED_RECT locked = {};
        succeeded = SUCCEEDED(g_textTexture->LockRect(0, &locked, nullptr, 0));
        if (succeeded) {
            for (int row = 0; row < kSceneHeight; ++row) {
                auto* destination = reinterpret_cast<DWORD*>(
                    static_cast<BYTE*>(locked.pBits) + row * locked.Pitch);
                const DWORD* source = texturePixels.data() +
                                      static_cast<size_t>(row) * kSceneWidth;
                for (int column = 0; column < kSceneWidth; ++column) {
                    destination[column] = source[column];
                }
            }
            g_textTexture->UnlockRect(0);
        }
    }

    SelectObject(memoryDc, previousBitmap);
    DeleteObject(bitmap);
    DeleteDC(memoryDc);
    return succeeded;
}

bool CreateDevice() {
    g_direct3D = Direct3DCreate9(D3D_SDK_VERSION);
    if (g_direct3D == nullptr) {
        return false;
    }

    D3DPRESENT_PARAMETERS parameters = {};
    parameters.Windowed = TRUE;
    parameters.SwapEffect = D3DSWAPEFFECT_DISCARD;
    parameters.hDeviceWindow = g_window;
    parameters.BackBufferFormat = D3DFMT_UNKNOWN;
    parameters.BackBufferWidth = static_cast<UINT>(g_width);
    parameters.BackBufferHeight = static_cast<UINT>(g_height);
    parameters.PresentationInterval = D3DPRESENT_INTERVAL_ONE;

    HRESULT result = g_direct3D->CreateDevice(
        D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, g_window,
        D3DCREATE_HARDWARE_VERTEXPROCESSING, &parameters, &g_device);
    if (FAILED(result)) {
        result = g_direct3D->CreateDevice(
            D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, g_window,
            D3DCREATE_SOFTWARE_VERTEXPROCESSING, &parameters, &g_device);
    }
    if (FAILED(result)) {
        return false;
    }

    return RestoreRenderStates() && CreateTextTexture();
}

void DrawStaticScene() {
    const DWORD gridColor = D3DCOLOR_ARGB(42, 73, 133, 147);
    for (int x = 32; x <= 928; x += 32) {
        DrawLine(static_cast<float>(x), 142.0f, static_cast<float>(x), 522.0f,
                 gridColor);
    }
    for (int y = 158; y <= 510; y += 32) {
        DrawLine(32.0f, static_cast<float>(y), 928.0f, static_cast<float>(y),
                 gridColor);
    }

    DrawRectangle(32, 140, 928, 142, D3DCOLOR_XRGB(54, 91, 104));
    DrawRectangle(32, 520, 928, 522, D3DCOLOR_XRGB(54, 91, 104));
    DrawRectangle(344, 150, 346, 512, D3DCOLOR_XRGB(54, 91, 104));
    DrawRectangle(664, 150, 666, 512, D3DCOLOR_XRGB(54, 91, 104));
    DrawRectangle(48, 177, 58, 185, D3DCOLOR_XRGB(80, 218, 193));
    DrawRectangle(364, 177, 374, 185, D3DCOLOR_XRGB(255, 192, 100));
    DrawRectangle(682, 177, 692, 185, D3DCOLOR_XRGB(116, 180, 255));
    DrawCircleOutline(184, 354, 96, D3DCOLOR_ARGB(120, 80, 218, 193));
    DrawCircleOutline(184, 354, 70, D3DCOLOR_ARGB(70, 116, 180, 255));
    DrawLine(88, 354, 280, 354, D3DCOLOR_ARGB(90, 80, 218, 193));
    DrawLine(184, 258, 184, 450, D3DCOLOR_ARGB(90, 80, 218, 193));
}

void DrawAnimatedScene() {
    constexpr float centerX = 505.0f;
    constexpr float centerY = 342.0f;
    constexpr float half = 61.0f;
    const float cosine = std::cos(g_angle);
    const float sine = std::sin(g_angle);
    const float tilt = std::cos(g_angle * 0.63f) * 0.42f;

    const float points[8][3] = {
        {-half, -half, -half}, {half, -half, -half},
        {half, half, -half}, {-half, half, -half},
        {-half, -half, half}, {half, -half, half},
        {half, half, half}, {-half, half, half},
    };
    float projected[8][2] = {};
    for (int i = 0; i < 8; ++i) {
        const float rotatedX = points[i][0] * cosine - points[i][2] * sine;
        const float rotatedZ = points[i][0] * sine + points[i][2] * cosine;
        projected[i][0] = centerX + rotatedX;
        projected[i][1] = centerY + points[i][1] + rotatedZ * tilt;
    }

    const int edges[12][2] = {
        {0, 1}, {1, 2}, {2, 3}, {3, 0}, {4, 5}, {5, 6},
        {6, 7}, {7, 4}, {0, 4}, {1, 5}, {2, 6}, {3, 7},
    };
    for (const auto& edge : edges) {
        const DWORD color = edge[0] < 4 ? D3DCOLOR_XRGB(80, 218, 193)
                                        : D3DCOLOR_XRGB(255, 192, 100);
        DrawLine(projected[edge[0]][0], projected[edge[0]][1],
                 projected[edge[1]][0], projected[edge[1]][1], color);
    }

    const float orbitAngle = g_angle * 1.45f;
    const float orbitX = centerX + std::cos(orbitAngle) * 115.0f;
    const float orbitY = centerY + std::sin(orbitAngle * 0.82f) * 73.0f;
    DrawCircle(orbitX, orbitY, 9.0f, D3DCOLOR_XRGB(255, 192, 100));
    DrawCircle(orbitX, orbitY, 17.0f, D3DCOLOR_ARGB(44, 255, 192, 100));

    for (int bar = 0; bar < 48; ++bar) {
        const float wave = std::sin(g_angle * 2.1f + bar * 0.31f);
        const float secondary = std::cos(g_angle * 1.2f + bar * 0.17f);
        const float height = 8.0f + (wave * wave) * 28.0f +
                             (1.0f + secondary) * 7.0f;
        const float left = 379.0f + bar * 5.7f;
        const DWORD color = (bar % 3 == 0) ? D3DCOLOR_XRGB(255, 192, 100)
                                           : D3DCOLOR_XRGB(80, 218, 193);
        DrawRectangle(left, 488.0f - height, left + 3.4f, 488.0f, color);
    }
    DrawLine(376, 489, 643, 489, D3DCOLOR_ARGB(120, 133, 169, 182));

    const float markerX = 80.0f + static_cast<float>(
        std::fmod(g_elapsedSeconds * 45.0, 225.0));
    DrawCircle(markerX, 474.0f, 4.0f, D3DCOLOR_XRGB(255, 192, 100));
}

void DrawTextTexture() {
    if (g_textTexture == nullptr || g_renderFailed) {
        return;
    }
    Vertex vertices[] = {
        MakeVertex(0, 0, D3DCOLOR_XRGB(255, 255, 255), 0.0f, 0.0f),
        MakeVertex(kSceneWidth, 0, D3DCOLOR_XRGB(255, 255, 255), 1.0f, 0.0f),
        MakeVertex(0, kSceneHeight, D3DCOLOR_XRGB(255, 255, 255), 0.0f, 1.0f),
        MakeVertex(kSceneWidth, kSceneHeight, D3DCOLOR_XRGB(255, 255, 255), 1.0f, 1.0f),
    };
    HRESULT result = g_device->SetFVF(kVertexFormat);
    if (FAILED(result)) {
        ReportGraphicsFailure(L"设置文字顶点格式", result);
        return;
    }
    g_device->SetTexture(0, g_textTexture);
    g_device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    g_device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    g_device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    g_device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    g_device->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    result = g_device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, vertices, sizeof(Vertex));
    g_device->SetTexture(0, nullptr);
    // 文字绘制结束后恢复顶点颜色通道，保证下一帧的彩色几何图元不会继承纹理状态。
    g_device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    g_device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
    g_device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    g_device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
    if (FAILED(result)) {
        ReportGraphicsFailure(L"绘制文字纹理", result);
    }
}

void RenderFrame(double deltaSeconds) {
    // 同时在绘制入口保护，停止期间既不推进时间，也不调用任何绘制或 Present。
    if (g_frameStopped || g_device == nullptr || g_width <= 0 || g_height <= 0) {
        return;
    }
    g_elapsedSeconds += deltaSeconds;
    if (!g_paused) {
        g_angle += static_cast<float>(deltaSeconds) * g_speed;
    }

    const HRESULT cooperative = g_device->TestCooperativeLevel();
    if (cooperative == D3DERR_DEVICELOST) {
        ReportGraphicsFailure(L"Direct3D 设备丢失", cooperative);
        return;
    }
    if (cooperative == D3DERR_DEVICENOTRESET || (SUCCEEDED(cooperative) && g_resizePending)) {
        D3DPRESENT_PARAMETERS parameters = {};
        parameters.Windowed = TRUE;
        parameters.SwapEffect = D3DSWAPEFFECT_DISCARD;
        parameters.hDeviceWindow = g_window;
        parameters.BackBufferFormat = D3DFMT_UNKNOWN;
        parameters.BackBufferWidth = static_cast<UINT>(g_width);
        parameters.BackBufferHeight = static_cast<UINT>(g_height);
        parameters.PresentationInterval = D3DPRESENT_INTERVAL_ONE;
        const HRESULT resetResult = g_device->Reset(&parameters);
        if (FAILED(resetResult)) {
            ReportGraphicsFailure(L"恢复 Direct3D 设备", resetResult);
            return;
        }
        g_resizePending = false;
        g_renderFailed = false;
        wcscpy_s(g_graphicsStatus, L"DX9 正常");
        UpdateWindowTitle();
    } else if (FAILED(cooperative)) {
        ReportGraphicsFailure(L"检查 Direct3D 设备", cooperative);
        return;
    }

    g_renderFailed = false;
    if (!RestoreRenderStates()) {
        return;
    }
    HRESULT result = g_device->Clear(0, nullptr, D3DCLEAR_TARGET,
                                    D3DCOLOR_XRGB(13, 20, 29), 1.0f, 0);
    if (FAILED(result)) {
        ReportGraphicsFailure(L"清空画面", result);
        return;
    }
    result = g_device->BeginScene();
    if (FAILED(result)) {
        ReportGraphicsFailure(L"开始绘制场景", result);
        return;
    }
    g_renderFailed = false;
    g_device->SetTexture(0, nullptr);
    g_device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    g_device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
    g_device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    g_device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
    g_device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    DrawStaticScene();
    g_device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    DrawAnimatedScene();
    DrawTextTexture();
    result = g_device->EndScene();
    if (FAILED(result)) {
        ReportGraphicsFailure(L"结束绘制场景", result);
        return;
    }

    result = g_device->Present(nullptr, nullptr, nullptr, nullptr);
    if (FAILED(result)) {
        ReportGraphicsFailure(L"显示画面", result);
    }
}

void ReleaseGraphics() {
    if (g_textTexture != nullptr) {
        g_textTexture->Release();
        g_textTexture = nullptr;
    }
    if (g_device != nullptr) {
        g_device->Release();
        g_device = nullptr;
    }
    if (g_direct3D != nullptr) {
        g_direct3D->Release();
        g_direct3D = nullptr;
    }
}

void UpdateWindowTitle() {
    wchar_t title[256] = {};
    swprintf_s(title, L"DirectX 9 动画演示 | %s | %s | 速度 %.1fx | %s",
               g_frameStopped ? L"帧提交已停止" : L"帧提交中",
               g_paused ? L"已暂停" : L"播放中", g_speed, g_graphicsStatus);
    SetWindowTextW(g_window, title);
}

LRESULT CALLBACK WindowProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_SIZE:
        if (wParam != SIZE_MINIMIZED && g_device != nullptr) {
            g_width = LOWORD(lParam);
            g_height = HIWORD(lParam);
            // 下一次允许绘制时应用最新尺寸，暂停期间可连续调整窗口大小。
            g_resizePending = true;
        }
        return 0;
    case WM_COMMAND:
        if (LOWORD(wParam) == kToggleFrameCommand && HIWORD(wParam) == 0) {
            SetFrameSubmissionStopped(!g_frameStopped);
            return 0;
        }
        break;
    case WM_KEYDOWN:
        switch (wParam) {
        case VK_ESCAPE:
            DestroyWindow(window);
            return 0;
        case VK_F8:
            // 按住按键产生的重复消息不切换，保证一次按下只改变一次状态。
            if ((lParam & (LPARAM(1) << 30)) == 0) {
                SetFrameSubmissionStopped(!g_frameStopped);
            }
            return 0;
        case VK_SPACE:
            g_paused = !g_paused;
            UpdateWindowTitle();
            return 0;
        case VK_LEFT:
            g_speed = (g_speed <= 0.26f) ? 0.25f : g_speed - 0.25f;
            UpdateWindowTitle();
            return 0;
        case VK_RIGHT:
            g_speed = (g_speed >= 3.75f) ? 4.0f : g_speed + 0.25f;
            UpdateWindowTitle();
            return 0;
        case 'R':
            g_angle = 0.0f;
            g_elapsedSeconds = 0.0;
            g_speed = 1.0f;
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

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCommand) {
    WNDCLASSEXW windowClass = {};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.style = CS_HREDRAW | CS_VREDRAW;
    windowClass.lpfnWndProc = WindowProcedure;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    windowClass.lpszClassName = L"Dx9DonghuaWindow";
    if (!RegisterClassExW(&windowClass)) {
        return 1;
    }

    // 菜单和 F8 使用同一状态，勾选表示已经停止提交。
    HMENU menu = CreateMenu();
    HMENU controls = CreatePopupMenu();
    if (menu == nullptr || controls == nullptr) {
        if (controls != nullptr) DestroyMenu(controls);
        if (menu != nullptr) DestroyMenu(menu);
        return 1;
    }
    if (!AppendMenuW(controls, MF_STRING, kToggleFrameCommand, L"停止帧提交\tF8")) {
        DestroyMenu(controls);
        DestroyMenu(menu);
        return 1;
    }
    if (!AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(controls), L"控制")) {
        DestroyMenu(controls);
        DestroyMenu(menu);
        return 1;
    }
    RECT windowBounds = {0, 0, kSceneWidth, kSceneHeight};
    AdjustWindowRect(&windowBounds, WS_OVERLAPPEDWINDOW, TRUE);
    g_window = CreateWindowExW(
        0, windowClass.lpszClassName, L"DirectX 9 動畫演示",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
        windowBounds.right - windowBounds.left,
        windowBounds.bottom - windowBounds.top,
        nullptr, menu, instance, nullptr);
    if (g_window == nullptr) {
        DestroyMenu(menu);
        return 1;
    }

    if (!CreateDevice()) {
        ReleaseGraphics();
        MessageBoxW(g_window,
                    L"無法初始化 DirectX 9。請確認顯示卡驅動和 DirectX 9 運行環境正常。",
                    L"初始化失敗", MB_OK | MB_ICONERROR);
        DestroyWindow(g_window);
        return 2;
    }

    QueryPerformanceFrequency(&g_clockFrequency);
    QueryPerformanceCounter(&g_previousTime);
    ShowWindow(g_window, showCommand);
    UpdateWindow(g_window);
    UpdateWindowTitle();

    MSG message = {};
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
        if (!running) {
            break;
        }

        if (g_frameStopped) {
            // 等待新的输入或窗口消息，停止时也保持菜单、快捷键和关闭操作可用。
            // 消息到来后先重新处理开关，再决定是否继续提交下一帧。
            MsgWaitForMultipleObjectsEx(0, nullptr, INFINITE, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
            continue;
        }

        LARGE_INTEGER currentTime = {};
        QueryPerformanceCounter(&currentTime);
        double deltaSeconds = static_cast<double>(currentTime.QuadPart -
                                                   g_previousTime.QuadPart) /
                              g_clockFrequency.QuadPart;
        g_previousTime = currentTime;
        if (deltaSeconds > 0.1) {
            deltaSeconds = 0.1;
        }
        RenderFrame(deltaSeconds);
        Sleep(1);
    }

    ReleaseGraphics();
    return static_cast<int>(message.wParam);
}
