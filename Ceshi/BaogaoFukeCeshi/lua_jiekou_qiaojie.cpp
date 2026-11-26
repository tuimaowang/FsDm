// 本文件实现供目标进程内 LuaJIT 调用的独立桥接 DLL。
// 桥接层只保存并调用现有 ZhaoTu.dll 的导出函数，不包含或改写 ZhaoTu/ZhuaTu 的实现。
// 所有实际加载工作都由 Lua 显式调用 QjInitialize 完成，DllMain 不做初始化，
// 避免在 Windows 装载器锁定期间加载其他 DLL 或执行复杂逻辑。
#include <Windows.h>

#include <cstdint>
#include <string>

namespace {

HMODULE g_zhaoTuModule = nullptr;
bool g_windowBound = false;

using BindWindowFn = int(WINAPI*)(HWND, const char*, const char*, const char*, int);
using UnbindWindowFn = int(WINAPI*)(void);
using SetPathFn = int(WINAPI*)(const char*);
using SetFindBackendFn = int(WINAPI*)(const char*);
using SetOcrEngineFn = int(WINAPI*)(const char*);
using SetOcrBackendFn = int(WINAPI*)(const char*);
using SetOcrModelFn = int(WINAPI*)(int, const char*);
using UseOcrModelFn = int(WINAPI*)(int);
using FindColorExFn = const void*(WINAPI*)(int, int, int, int, const char*, double, int);
using FindPicFn = const void*(WINAPI*)(int, int, int, int, const char*, const char*, double, int);
using OcrFn = const void*(WINAPI*)(int, int, int, int, const char*, double);
using FreeResultFn = int(WINAPI*)(const void*);

BindWindowFn g_bindWindow = nullptr;
UnbindWindowFn g_unbindWindow = nullptr;
SetPathFn g_setPath = nullptr;
SetFindBackendFn g_setFindBackend = nullptr;
SetOcrEngineFn g_setOcrEngine = nullptr;
SetOcrBackendFn g_setOcrBackend = nullptr;
SetOcrModelFn g_setOcrModel = nullptr;
UseOcrModelFn g_useOcrModel = nullptr;
FindColorExFn g_findColorEx = nullptr;
FindPicFn g_findPic = nullptr;
OcrFn g_ocr = nullptr;
FreeResultFn g_freeResult = nullptr;

std::string g_lastError;

void SetError(const char* message) {
    g_lastError = message ? message : "未知错误";
}

std::wstring Utf8ToWide(const char* text) {
    if (!text || !*text) {
        return {};
    }
    const int required = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, nullptr, 0);
    if (required <= 0) {
        return {};
    }
    std::wstring result(static_cast<size_t>(required), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1,
                            result.data(), required) <= 0) {
        return {};
    }
    result.resize(static_cast<size_t>(required - 1));
    return result;
}

template <typename T>
bool LoadExport(T& target, const char* name) {
    target = reinterpret_cast<T>(GetProcAddress(g_zhaoTuModule, name));
    if (!target) {
        g_lastError = std::string("ZhaoTu.dll 缺少导出函数：") + name;
        return false;
    }
    return true;
}

void ClearFunctionPointers() {
    g_bindWindow = nullptr;
    g_unbindWindow = nullptr;
    g_setPath = nullptr;
    g_setFindBackend = nullptr;
    g_setOcrEngine = nullptr;
    g_setOcrBackend = nullptr;
    g_setOcrModel = nullptr;
    g_useOcrModel = nullptr;
    g_findColorEx = nullptr;
    g_findPic = nullptr;
    g_ocr = nullptr;
    g_freeResult = nullptr;
}

bool ApiReady() {
    return g_zhaoTuModule && g_bindWindow && g_unbindWindow && g_setPath &&
        g_setFindBackend && g_setOcrEngine && g_setOcrBackend && g_setOcrModel &&
        g_useOcrModel && g_findColorEx && g_findPic && g_ocr && g_freeResult;
}

} // namespace

extern "C" {

// 显式从 UTF-8 路径加载指定交付目录中的 ZhaoTu.dll。
// 使用 LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR 让 Windows 从 ZhaoTu.dll 所在目录查找其
// 配套依赖，不更改宿主进程的全局 DLL 搜索路径。重复初始化同一模块直接成功。
__declspec(dllexport) int WINAPI QjInitialize(const char* jiaofuDirectoryUtf8) {
    if (g_zhaoTuModule) {
        return ApiReady() ? 1 : 0;
    }

    const std::wstring directory = Utf8ToWide(jiaofuDirectoryUtf8);
    if (directory.empty()) {
        SetError("交付目录路径为空，或不是有效的 UTF-8 路径。");
        return 0;
    }

    std::wstring dllPath = directory;
    if (dllPath.back() != L'\\' && dllPath.back() != L'/') {
        dllPath.push_back(L'\\');
    }
    dllPath += L"ZhaoTu.dll";

    g_zhaoTuModule = LoadLibraryExW(
        dllPath.c_str(), nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!g_zhaoTuModule) {
        g_lastError = "加载交付目录中的 ZhaoTu.dll 失败，Windows 错误码：" +
            std::to_string(GetLastError());
        return 0;
    }

#define QJ_LOAD(member, exportName) \
    if (!LoadExport(g_##member, exportName)) { \
        FreeLibrary(g_zhaoTuModule); \
        g_zhaoTuModule = nullptr; \
        ClearFunctionPointers(); \
        return 0; \
    }
    QJ_LOAD(bindWindow, "BindWindow");
    QJ_LOAD(unbindWindow, "UnBindWindow");
    QJ_LOAD(setPath, "SetPath");
    QJ_LOAD(setFindBackend, "SetFindBackend");
    QJ_LOAD(setOcrEngine, "SetOcrEngine");
    QJ_LOAD(setOcrBackend, "SetOcrBackend");
    QJ_LOAD(setOcrModel, "SetOcrModel");
    QJ_LOAD(useOcrModel, "UseOcrModel");
    QJ_LOAD(findColorEx, "FindColorEx");
    QJ_LOAD(findPic, "FindPic");
    QJ_LOAD(ocr, "Ocr");
    QJ_LOAD(freeResult, "FreeResult");
#undef QJ_LOAD

    g_lastError.clear();
    return 1;
}

// 解绑并释放桥接层持有的 ZhaoTu.dll 模块引用。
// 调用前 Lua 应先释放所有识别结果；不要在仍有正在执行的识别时关闭桥接层。
__declspec(dllexport) int WINAPI QjShutdown(void) {
    if (!g_zhaoTuModule) {
        return 1;
    }
    if (g_windowBound && g_unbindWindow) {
        g_unbindWindow();
        g_windowBound = false;
    }
    const BOOL released = FreeLibrary(g_zhaoTuModule);
    g_zhaoTuModule = nullptr;
    ClearFunctionPointers();
    if (!released) {
        SetError("释放 ZhaoTu.dll 模块引用失败。");
        return 0;
    }
    return 1;
}

__declspec(dllexport) const char* WINAPI QjGetLastError(void) {
    return g_lastError.c_str();
}

__declspec(dllexport) int WINAPI QjBindWindow(
    uintptr_t hwndValue, const char* displayUtf8) {
    if (!ApiReady()) {
        SetError("请先成功调用 QjInitialize。");
        return 0;
    }
    HWND hwnd = reinterpret_cast<HWND>(hwndValue);
    if (!g_bindWindow(hwnd, displayUtf8, "normal", "normal", 0)) {
        SetError("ZhaoTu.dll 的 BindWindow 调用失败。");
        return 0;
    }
    g_windowBound = true;
    return 1;
}

__declspec(dllexport) int WINAPI QjUnbindWindow(void) {
    if (!ApiReady()) {
        SetError("请先成功调用 QjInitialize。");
        return 0;
    }
    const int result = g_unbindWindow();
    if (result) {
        g_windowBound = false;
    }
    return result;
}

__declspec(dllexport) int WINAPI QjSetFindBackend(const char* backendUtf8) {
    if (!ApiReady()) {
        SetError("请先成功调用 QjInitialize。");
        return 0;
    }
    return g_setFindBackend(backendUtf8);
}

__declspec(dllexport) int WINAPI QjSetPath(const char* pathUtf8) {
    if (!ApiReady()) {
        SetError("请先成功调用 QjInitialize。");
        return 0;
    }
    return g_setPath(pathUtf8);
}

__declspec(dllexport) const void* WINAPI QjFindColorEx(
    int x1, int y1, int x2, int y2, const char* colorUtf8, double similarity, int direction) {
    if (!ApiReady()) {
        SetError("请先成功调用 QjInitialize。");
        return nullptr;
    }
    return g_findColorEx(x1, y1, x2, y2, colorUtf8, similarity, direction);
}

__declspec(dllexport) const void* WINAPI QjFindPic(
    int x1, int y1, int x2, int y2, const char* templateUtf8,
    const char* deltaColorUtf8, double similarity, int direction) {
    if (!ApiReady()) {
        SetError("请先成功调用 QjInitialize。");
        return nullptr;
    }
    return g_findPic(x1, y1, x2, y2, templateUtf8, deltaColorUtf8, similarity, direction);
}

// 绑定目标窗口的客户区区域 OCR；这和直接传离线图片做 OCR 不同，运行结果受
// 注入目标当前显示画面及 OCR 会话在目标进程中的运行情况影响。
__declspec(dllexport) const void* WINAPI QjOcr(
    int x1, int y1, int x2, int y2, const char* colorFormatUtf8, double similarity) {
    if (!ApiReady()) {
        SetError("请先成功调用 QjInitialize。");
        return nullptr;
    }
    return g_ocr(x1, y1, x2, y2, colorFormatUtf8, similarity);
}

__declspec(dllexport) int WINAPI QjSetOcrEngine(const char* engineUtf8) {
    if (!ApiReady()) {
        SetError("请先成功调用 QjInitialize。");
        return 0;
    }
    return g_setOcrEngine(engineUtf8);
}

__declspec(dllexport) int WINAPI QjSetOcrBackend(const char* backendUtf8) {
    if (!ApiReady()) {
        SetError("请先成功调用 QjInitialize。");
        return 0;
    }
    return g_setOcrBackend(backendUtf8);
}

__declspec(dllexport) int WINAPI QjSetOcrModel(int index, const char* modelDirectoryUtf8) {
    if (!ApiReady()) {
        SetError("请先成功调用 QjInitialize。");
        return 0;
    }
    return g_setOcrModel(index, modelDirectoryUtf8);
}

__declspec(dllexport) int WINAPI QjUseOcrModel(int index) {
    if (!ApiReady()) {
        SetError("请先成功调用 QjInitialize。");
        return 0;
    }
    return g_useOcrModel(index);
}

__declspec(dllexport) int WINAPI QjFreeResult(const void* result) {
    if (!ApiReady()) {
        SetError("请先成功调用 QjInitialize。");
        return 0;
    }
    return g_freeResult(result);
}

// 读取当前宿主进程累计 CPU 时间（用户态 + 内核态，单位为 100 纳秒）。
// Lua 对某段测试前后各读取一次并求差，就能测到目标进程本身的 CPU 消耗，
// 而不是另起一个 LuaJIT 进程后测量错对象。
__declspec(dllexport) int WINAPI QjGetProcessCpuTime100ns(uint64_t* cpuTime) {
    if (!cpuTime) {
        SetError("CPU 时间输出地址为空。");
        return 0;
    }

    FILETIME creation{}, exit{}, kernel{}, user{};
    if (!GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user)) {
        g_lastError = "读取目标进程 CPU 时间失败，Windows 错误码：" +
            std::to_string(GetLastError());
        return 0;
    }

    ULARGE_INTEGER kernelValue{};
    ULARGE_INTEGER userValue{};
    kernelValue.LowPart = kernel.dwLowDateTime;
    kernelValue.HighPart = kernel.dwHighDateTime;
    userValue.LowPart = user.dwLowDateTime;
    userValue.HighPart = user.dwHighDateTime;
    *cpuTime = kernelValue.QuadPart + userValue.QuadPart;
    return 1;
}

__declspec(dllexport) uint32_t WINAPI QjGetLogicalProcessorCount(void) {
    const DWORD count = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    return count ? count : 1;
}

} // extern "C"

// DllMain 刻意保持为空：初始化、加载 ZhaoTu.dll 和窗口绑定均由 Lua 显式控制。
BOOL WINAPI DllMain(HINSTANCE, DWORD, LPVOID) {
    return TRUE;
}
