// 复用配套 OBS hook 和偏移助手；本模块不注入其他进程、不读取 CPU 像素、不保存图片。
#include "ObsCaiji.h"
#include "../third_party/obs/graphics_hook_protocol.h"
#include <filesystem>
#include <sstream>
#include <string>
#include <map>
#include <algorithm>

namespace fsdm::obs {
namespace {
HMODULE hook{};
HANDLE infoMap{}, keepalive{}, initialize{}, ready{}, restart{}, stop{}, textureMap{};
hook_info* info{};
shtex_data* texture{};
uint32_t currentMap{};
bool receivedReady{};
graphics_offsets cachedOffsets{};
bool offsetsReady{};
replica::SharedSource source;

std::wstring Name(const wchar_t* base) { return std::wstring(base) + std::to_wstring(GetCurrentProcessId()); }
void Close(HANDLE& handle) { if (handle) CloseHandle(handle); handle = nullptr; }
void ClearTexture() {
    source={}; // 先释放已打开的纹理，再解除协议映射，防止映射/句柄复用后误读旧源。
    if (texture) UnmapViewOfFile(texture);
    texture = nullptr; Close(textureMap); currentMap = 0;
}
std::filesystem::path Directory() {
    HMODULE module{};
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&Begin), &module);
    wchar_t filename[32768]{};
    const DWORD n = GetModuleFileNameW(module, filename, static_cast<DWORD>(std::size(filename)));
    if (!n || n >= std::size(filename)) return {};
    return std::filesystem::path(filename).parent_path();
}

// 隐藏助手窗口，收集其 INI 输出；总等待受绑定的七秒截止时间限制。
HRESULT Offsets(const std::filesystem::path& executable, graphics_offsets& offsets, ULONGLONG deadline) {
    SECURITY_ATTRIBUTES attributes{sizeof(attributes), nullptr, TRUE};
    HANDLE read{}, write{};
    if (!CreatePipe(&read, &write, &attributes, 0)) return HRESULT_FROM_WIN32(GetLastError());
    replica::Handle readGuard(read), writeGuard(write);
    SetHandleInformation(read, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si{}; si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW; si.wShowWindow = SW_HIDE;
    si.hStdOutput = si.hStdError = write;
    // 不继承宿主工作台输入。助手无交互输入。
    replica::Handle nullInput(CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
        &attributes, OPEN_EXISTING, 0, nullptr));
    si.hStdInput = nullInput.get();
    PROCESS_INFORMATION process{};
    std::wstring command = L"\"" + executable.wstring() + L"\"";
    if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
        nullptr, executable.parent_path().c_str(), &si, &process)) return HRESULT_FROM_WIN32(GetLastError());
    replica::Handle processGuard(process.hProcess), threadGuard(process.hThread);
    std::string text;
    for (;;) {
        DWORD available{};
        if (!PeekNamedPipe(read, nullptr, 0, nullptr, &available, nullptr)) break;
        if (available) {
            char buffer[4096]; DWORD count{};
            if (!ReadFile(read, buffer, std::min(available, DWORD{sizeof(buffer)}), &count, nullptr)) break;
            text.append(buffer, count);
            if (text.size() > 65536) { TerminateProcess(process.hProcess, 1); return E_FAIL; }
            continue;
        }
        if (WaitForSingleObject(process.hProcess, 0) == WAIT_OBJECT_0) break;
        if (GetTickCount64() >= deadline) {
            TerminateProcess(process.hProcess, 1);
            return HRESULT_FROM_WIN32(WAIT_TIMEOUT);
        }
        Sleep(5);
    }
    DWORD exitCode{};
    if (!GetExitCodeProcess(process.hProcess, &exitCode) || exitCode) return E_FAIL;
    std::map<std::string, uint32_t> values;
    std::istringstream input(text);
    std::string line, section;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.size() > 2 && line.front() == '[' && line.back() == ']')
            section = line.substr(1, line.size() - 2);
        else if (const auto pos = line.find('='); pos != std::string::npos) {
            size_t end{};
            auto number = std::stoull(line.substr(pos + 1), &end, 0);
            if (end != line.size() - pos - 1 || number > UINT32_MAX) return E_INVALIDARG;
            values[section + "." + line.substr(0, pos)] = static_cast<uint32_t>(number);
        }
    }
    offsets = {};
    offsets.dxgi.present = values["dxgi.present"];
    offsets.dxgi.present1 = values["dxgi.present1"];
    offsets.dxgi.resize = values["dxgi.resize"];
    offsets.dxgi2.release = values["dxgi.release"];
    // 配套旧版助手可能不输出此段；保留 0，让 OBS 自行探测，不能用错误模块基址推算。
    offsets.d3d12.execute_command_lists = values["d3d12.execute_command_lists"];
    // 本期只接 DXGI 窗口，故不启用无关的 D3D8/9、DDraw 钩子。
    return offsets.dxgi.present && offsets.dxgi.resize ? S_OK : E_FAIL;
}
}

void End() {
    if (stop && keepalive) SetEvent(stop);
    Close(keepalive);
    ClearTexture();
    receivedReady = false;
}

int Begin(HWND, HRESULT& detail, ULONGLONG deadline) {
    const auto directory = Directory();
    const auto dll = directory / L"graphics-hook64.dll";
    const auto helper = directory / L"get-graphics-offsets64.exe";
    if (!std::filesystem::is_regular_file(dll) || !std::filesystem::is_regular_file(helper)) {
        detail = HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND); return FS_JIAZAI_SHIBAI;
    }
    // 第一次接入时不附着未知会话。命名 keepalive 的已存在状态也视为冲突。
    if (!hook) {
        if (GetModuleHandleW(L"graphics-hook64.dll")) {
            detail = HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS); return FS_OBS_CHONGTU;
        }
        replica::Handle existing(OpenFileMappingW(FILE_MAP_READ, FALSE, Name(SHMEM_HOOK_INFO).c_str()));
        if (existing) { detail = HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS); return FS_OBS_CHONGTU; }
    }
    keepalive = CreateMutexW(nullptr, FALSE, Name(WINDOW_HOOK_KEEPALIVE).c_str());
    if (!keepalive) { detail = HRESULT_FROM_WIN32(GetLastError()); return FS_CHUSHIHUA_SHIBAI; }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        Close(keepalive); detail = HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS); return FS_OBS_CHONGTU;
    }
    if (!offsetsReady) {
        detail = Offsets(helper, cachedOffsets, deadline);
        if (FAILED(detail)) { End(); return detail == HRESULT_FROM_WIN32(WAIT_TIMEOUT)
            ? FS_CHUSHIHUA_CHAOSHI : FS_CHUSHIHUA_SHIBAI; }
        offsetsReady = true; // 本进程模块保持加载，解绑重绑不必反复启动助手。
    }
    if (!hook) hook = LoadLibraryExW(dll.c_str(), nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!hook) { detail = HRESULT_FROM_WIN32(GetLastError()); End(); return FS_JIAZAI_SHIBAI; }
    while (!info && GetTickCount64() < deadline) {
        infoMap = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, Name(SHMEM_HOOK_INFO).c_str());
        if (infoMap) {
            info = static_cast<hook_info*>(MapViewOfFile(infoMap, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(hook_info)));
            if (!info) Close(infoMap);
        }
        if (!info) Sleep(5);
    }
    if (!info) { detail = HRESULT_FROM_WIN32(WAIT_TIMEOUT); End(); return FS_CHUSHIHUA_CHAOSHI; }
    auto openEvent = [](const wchar_t* name) {
        return OpenEventW(SYNCHRONIZE | EVENT_MODIFY_STATE, FALSE, Name(name).c_str());
    };
    if (!initialize) initialize = openEvent(EVENT_HOOK_INIT);
    if (!ready) ready = openEvent(EVENT_HOOK_READY);
    if (!restart) restart = openEvent(EVENT_CAPTURE_RESTART);
    if (!stop) stop = openEvent(EVENT_CAPTURE_STOP);
    if (!initialize || !ready || !restart || !stop) {
        detail = HRESULT_FROM_WIN32(GetLastError()); End(); return FS_CHUSHIHUA_SHIBAI;
    }
    ClearTexture(); receivedReady = false;
    ResetEvent(ready);
    info->offsets = cachedOffsets;
    info->force_shmem = false; info->capture_overlay = false;
    info->allow_srgb_alias = true; info->frame_interval = 0;
    // 不等待 HookReady：首帧需要目标继续 Present，绑定可由同一个渲染线程发起。
    if (!SetEvent(initialize) || !SetEvent(restart)) {
        detail = HRESULT_FROM_WIN32(GetLastError()); End(); return FS_CHUSHIHUA_SHIBAI;
    }
    detail = S_OK;
    return FS_CHENGGONG;
}

HRESULT Snapshot(IDXGISwapChain* chain, ID3D12CommandQueue* queue, replica::GpuSlot& slot, HWND window) {
    if (!info || !keepalive) return kWaiting;
    if (!receivedReady) receivedReady = WaitForSingleObject(ready, 0) == WAIT_OBJECT_0;
    if (!receivedReady) return kWaiting;
    // 拒绝其他窗口的共享纹理，绝不枚举进程内任意映射来凑一个可用画面。
    const hook_info snapshot = *info;
    if (snapshot.window != static_cast<uint32_t>(reinterpret_cast<uintptr_t>(window))) return kWindowMismatch;
    if (snapshot.type != CAPTURE_TYPE_TEXTURE) return kMemoryCapture;
    if (!snapshot.cx || !snapshot.cy || !snapshot.map_id) return kWaiting;
    if (currentMap != snapshot.map_id) {
        ClearTexture();
        const HWND root = GetAncestor(window, GA_ROOT);
        const auto name = std::wstring(SHMEM_TEXTURE) + L"_" +
            std::to_wstring(reinterpret_cast<uintptr_t>(root)) + L"_" + std::to_wstring(snapshot.map_id);
        textureMap = OpenFileMappingW(FILE_MAP_READ, FALSE, name.c_str());
        if (!textureMap) return kWaiting;
        texture = static_cast<shtex_data*>(MapViewOfFile(textureMap, FILE_MAP_READ, 0, 0, sizeof(shtex_data)));
        if (!texture) { Close(textureMap); return HRESULT_FROM_WIN32(GetLastError()); }
        currentMap = snapshot.map_id;
    }
    if (!texture->tex_handle) return kWaiting;
    return replica::SnapshotShared(chain, queue,
        reinterpret_cast<HANDLE>(static_cast<uintptr_t>(texture->tex_handle)), source, slot, snapshot.flip ? 1u : 0u);
}
}
