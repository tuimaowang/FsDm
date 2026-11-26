// 找图模块当前仅负责绑定和转发帧数据；像素始终由采集模块持有。
#include "ZhaoTu.h"
#include "Peizhi.h"
#include "ZhenhuanCun.h"
#include <atomic>
#include <filesystem>
#include <cstring>

namespace {
HMODULE self{};
std::atomic<HMODULE> capture{};
INIT_ONCE once = INIT_ONCE_STATIC_INIT;
std::atomic<int> lastResult{FS_CHENGGONG};
std::atomic<LONG> loadError{};
using BeginFn = FsBindFn;
using EndFn = int (WINAPI*)(void);
BeginFn begin{};
EndFn end{};
using IntervalFn = int (WINAPI*)(int);
IntervalFn setCaptureInterval{};
FsCopyFn copy{};
FsStatusFn status{};
FsMapFn map{};
FsGpuAcquireFn acquire{};
FsGpuReleaseFn release{};
FsCaptureStatusFn captureStatus{};
FsProcessorAcquireFn processorAcquire{};
FsProcessorReleaseFn processorRelease{};

BOOL CALLBACK LoadCapture(PINIT_ONCE, PVOID, PVOID*) {
    try {
        wchar_t filename[32768];
        const DWORD length = GetModuleFileNameW(self, filename, static_cast<DWORD>(std::size(filename)));
        if (!length || length >= std::size(filename)) { loadError = ERROR_BAD_PATHNAME; return FALSE; }
        const auto directory = std::filesystem::path(filename).parent_path();
        // 首选同目录部署；开发目录使用 ZhaoTu/dist/bin 到 ZhuaTu/dist/bin 的固定相对路径。
        const std::filesystem::path candidates[] = {
            directory / L"ZhuaTu.dll",
            directory.parent_path().parent_path().parent_path() / L"ZhuaTu/dist/bin/ZhuaTu.dll"
        };
        HMODULE module{};
        for (const auto& path : candidates) {
            module = LoadLibraryExW(path.c_str(), nullptr,
                LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
            if (module) break;
            loadError = static_cast<LONG>(GetLastError());
        }
        if (!module) return FALSE;
        begin = reinterpret_cast<BeginFn>(GetProcAddress(module, "ZhuaTuBegin"));
        end = reinterpret_cast<EndFn>(GetProcAddress(module, "ZhuaTuEnd"));
        setCaptureInterval = reinterpret_cast<IntervalFn>(GetProcAddress(module, "ZhuaTuSetCaptureFrameInterval"));
        copy = reinterpret_cast<FsCopyFn>(GetProcAddress(module, "ZhuaTuCopyFrame"));
        status = reinterpret_cast<FsStatusFn>(GetProcAddress(module, "ZhuaTuGetStatus"));
        map = reinterpret_cast<FsMapFn>(GetProcAddress(module, "ZhuaTuMapPoint"));
        acquire = reinterpret_cast<FsGpuAcquireFn>(GetProcAddress(module, "ZhuaTuAcquireGpuFrame"));
        release = reinterpret_cast<FsGpuReleaseFn>(GetProcAddress(module, "ZhuaTuReleaseGpuFrame"));
        captureStatus = reinterpret_cast<FsCaptureStatusFn>(GetProcAddress(module, "ZhuaTuGetCaptureStatus"));
        processorAcquire = reinterpret_cast<FsProcessorAcquireFn>(GetProcAddress(module, "ZhuaTuAcquireProcessor"));
        processorRelease = reinterpret_cast<FsProcessorReleaseFn>(GetProcAddress(module, "ZhuaTuReleaseProcessor"));
        if (!begin || !end || !setCaptureInterval || !copy || !status || !map ||
            !acquire || !release || !captureStatus || !processorAcquire || !processorRelease) {
            loadError = ERROR_PROC_NOT_FOUND;
            return FALSE;
        }
        loadError = 0;
        capture.store(module); // 最后发布，其他线程看到句柄时函数表已经完整。
        return TRUE;
    } catch (...) { loadError = ERROR_NOT_ENOUGH_MEMORY; return FALSE; }
}
bool ValidInput(const char* value) {
    return value && (!strcmp(value, "normal") || !strcmp(value, "windows") || !strcmp(value, "dx"));
}
int Fail(int result) { lastResult.store(result); return 0; }
}
DWORD WINAPI FsWorkbench(void*);
// 工作台内部的编号输出控制；关闭操作会等待已经开始的编号输出结束。
bool FsSetFrameOutput(int enabled, const FsZhuangtai& current);

int WINAPI BindWindow(HWND hwnd, const char* display, const char* mouse, const char* keypad, int mode) {
    if (!hwnd || !display || !ValidInput(mouse) || !ValidInput(keypad)) return Fail(FS_CANSHU_CUOWU);
    if ((strcmp(display, "dx") && strcmp(display, "obs") && strcmp(display, "wgc")) ||
        (mode != 0 && mode != 1)) return Fail(FS_BUZHICHI);
    DWORD pid{};
    if (!IsWindow(hwnd) || !GetWindowThreadProcessId(hwnd, &pid) || pid != GetCurrentProcessId())
        return Fail(FS_CANSHU_CUOWU);
    if (!InitOnceExecuteOnce(&once, LoadCapture, nullptr, nullptr)) return Fail(FS_JIAZAI_SHIBAI);
    // 采集方式和绑定一次提交到底层，在同一把锁内检查并生效；
    // 不先切换全局方式，避免失败绑定或并发调用改变已有绑定的采集来源。
    const int result = begin(hwnd, display, mouse, keypad, mode);
    lastResult.store(result);
    return result == FS_CHENGGONG ? 1 : 0;
}
int WINAPI UnBindWindow(void) {
    FsSetFrameOutput(0, {});
    const int result = capture.load() ? end() : FS_CHENGGONG;
    lastResult.store(result);
    return result == FS_CHENGGONG ? 1 : 0;
}
int WINAPI SetCaptureFrameInterval(int interval) {
    if (interval < -1) return Fail(FS_CANSHU_CUOWU);
    if (!capture.load()) return Fail(FS_WEIBANGDING);
    const int result = setCaptureInterval(interval);
    lastResult.store(result);
    return result == FS_CHENGGONG ? 1 : 0;
}
int WINAPI SetFrameResultCache(int bypass) {
    if (bypass != 0 && bypass != 1) return Fail(FS_CANSHU_CUOWU);
    fsdm::zhenhuancun::SetResultReuse(bypass == 0);
    lastResult.store(FS_CHENGGONG);
    return 1;
}
int WINAPI CopyFrame(void* pixels, uint64_t capacity, FsZhen* frame) {
    if (!frame || frame->size != sizeof(FsZhen)) return FS_CANSHU_CUOWU;
    if (capture.load()) return copy(pixels, capacity, frame);
    *frame = {};
    frame->size = sizeof(*frame);
    frame->version = FSDM_BANBEN;
    return FS_WEIBANGDING;
}
int WINAPI GetStatus(FsZhuangtai* output) {
    if (!output || output->size != sizeof(FsZhuangtai)) return FS_CANSHU_CUOWU;
    if (capture.load()) {
        const int result = status(output);
        if (result != FS_CHENGGONG) return result;
    } else {
        *output = {};
        output->size = sizeof(*output);
        output->version = FSDM_BANBEN;
        output->result = FS_WEIBANGDING;
    }
    // 失败绑定不覆盖当前绑定配置，仍然公开最近一次绑定错误，便于诊断。
    if (const int recent = lastResult.load(); recent != FS_CHENGGONG) {
        output->result = recent;
        if (recent == FS_JIAZAI_SHIBAI && loadError.load()) output->detail = loadError.load();
    }
    return FS_CHENGGONG;
}
uint64_t WINAPI GetFrameNumber(int output) {
    if (output != 0 && output != 1) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return 0;
    }
    // 状态查询只读取快照元数据。即使后续采集暂时失败，也能返回已有快照的编号。
    // 输出控制与监视线程同步，传入 0 后不会继续打印先前任务的编号。
    FsZhuangtai current{};
    current.size = sizeof(current);
    const int result = GetStatus(&current);
    if (!FsSetFrameOutput(output, current)) return 0;
    if (result != FS_CHENGGONG || !current.bound || !current.hasFrame) return 0;
    return current.frame.frameNumber;
}
int WINAPI MapPoint(double x, double y, double* outX, double* outY, int direction) {
    if (!outX || !outY || (direction != 0 && direction != 1)) return FS_CANSHU_CUOWU;
    return capture.load() ? map(x, y, outX, outY, direction) : FS_WEIBANGDING;
}

int WINAPI AcquireGpuFrame(FsGpuZhen* frame) {
    if (!frame || frame->size != sizeof(*frame)) return FS_CANSHU_CUOWU;
    if (capture.load()) return acquire(frame);
    *frame = {}; frame->size = sizeof(*frame); frame->version = FSDM_BANBEN;
    return FS_WEIBANGDING;
}
int WINAPI ReleaseGpuFrame(uint64_t token) {
    return capture.load() ? release(token) : FS_CANSHU_CUOWU;
}
int WINAPI GetCaptureStatus(FsCaijiZhuangtai* output) {
    if (!output || output->size != sizeof(*output)) return FS_CANSHU_CUOWU;
    if (capture.load()) return captureStatus(output);
    *output = {}; output->size = sizeof(*output); output->version = FSDM_BANBEN;
    // 采集 DLL 尚未按需加载时也显示同样的 DX 默认值；查询本身不启动采集。
    output->captureBackend = FS_CAIJI_DX;
    return FS_CHENGGONG;
}

// 仅在模块内部链接使用，不导出处理设备指针给普通调用者。
HRESULT FsAcquireProcessor(uint32_t low,int32_t high,FsGpuChuli* output) {
    return capture.load()?processorAcquire(low,high,output):E_UNEXPECTED;
}
HRESULT FsReleaseProcessor(uint64_t token) {
    return capture.load()?processorRelease(token):E_UNEXPECTED;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        self = instance;
        // 静态 C 运行库可能需要线程通知，保留系统默认行为。
        HMODULE pinned{};
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                               reinterpret_cast<LPCWSTR>(&BindWindow), &pinned)) return FALSE;
        if constexpr (kGongzuotaiKaiqi) {
            HANDLE worker = CreateThread(nullptr, 0, FsWorkbench, nullptr, 0, nullptr);
            if (!worker) return FALSE;
            CloseHandle(worker);
        }
    }
    return TRUE;
}
