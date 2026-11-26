// 管理绑定、三个 GPU 快照槽位及租约；CPU 像素仅由 CopyFrame 按需读取。
#include "Neibu.h"
#include "Xieyi.h"
#include "Huichuan.h"
#include "ObsCaiji.h"
#include "WgcCaiji.h"
#include <array>
#include <cstring>
#include <cmath>
#include <chrono>

namespace fsdm {
std::atomic<uint64_t> generation{1};
std::atomic<HWND> target{};
std::atomic<LONG> startup{};
std::atomic<LONG> startupDetail{};
// 未绑定时初始显示 DX；后续保留最近成功绑定的方式供诊断。
// 每次 BindWindow 都以 display 明确选择，绑定成功之前不改变此值。
std::atomic<uint32_t> captureBackend{FS_CAIJI_DX};
namespace {
SRWLOCK stateLock = SRWLOCK_INIT;
struct Lock {
    Lock() { AcquireSRWLockExclusive(&stateLock); }
    ~Lock() { ReleaseSRWLockExclusive(&stateLock); }
};
std::array<std::unique_ptr<replica::GpuSlot>, 3> slots;
int latest = -1;
// 节奏锁仅保护两个整数，避免呈现线程为 CPU 读回或 GPU 复制等待。
SRWLOCK intervalLock = SRWLOCK_INIT;
int captureInterval = 0;
int inputsToSkip = 0;
void ResetInterval(int interval) {
    AcquireSRWLockExclusive(&intervalLock);
    captureInterval = interval;
    inputsToSkip = 0;
    ReleaseSRWLockExclusive(&intervalLock);
}
bool CapturePaused() {
    AcquireSRWLockShared(&intervalLock);
    const bool paused = captureInterval == -1;
    ReleaseSRWLockShared(&intervalLock);
    return paused;
}
FsCaijiZhuangtai statistics{};
struct Lease { uint64_t token{}; int slot{}; };
std::array<Lease, 64> leases{};
uint64_t nextToken{};
uint64_t lockedFrameToken{};
int lockedFrameSlot = -1;
// 租约同时用于 GPU 外部读取和 CPU 内部读取，解绑不能销毁仍在使用的槽位。
void ReleaseLocked(uint64_t token) {
    for (auto& lease : leases) if (lease.token == token) {
        auto& slot = slots[lease.slot];
        --slot->users;
        if (!slot->users && slot->frame.bindingVersion != generation.load()) slot.reset();
        lease.token = 0;
        return;
    }
}
uint64_t ReserveLocked(int index) {
    for (auto& lease : leases) if (!lease.token) {
        if (++nextToken == 0) ++nextToken;
        lease = {nextToken, index};
        ++slots[index]->users;
        return nextToken;
    }
    return 0;
}
FsZhuangtai state{};
HANDLE owner{};
DWORD windowThread{};
// 窗口属性跟随窗口销毁，避免同一个 HWND 数字被系统重用后继续采集。
constexpr wchar_t property[] = L"FsDm.ZhuaTu.Binding.a431bd19";
void EndLocked() {
    HWND hwnd = target.exchange(nullptr);
    if (hwnd && GetPropW(hwnd, property) == reinterpret_cast<HANDLE>(generation.load()))
        RemovePropW(hwnd, property);
    if (lockedFrameToken) {
        ReleaseLocked(lockedFrameToken);
        lockedFrameToken = 0;
        lockedFrameSlot = -1;
    }
    ResetInterval(0);
    generation.fetch_add(1);
    if (owner) CloseHandle(owner);
    owner = nullptr;
    state = {};
    state.result = FS_WEIBANGDING;
    obs::End();
    wgc::End(); // 只请求停止，不等待工作线程；旧帧仍须通过绑定版本检查。
    latest = -1;
    for (auto& slot : slots) if (slot && !slot->users) slot.reset();
    replica::ResetBridge();
    statistics = {};
}
bool AliveLocked() {
    HWND hwnd = target.load();
    if (!hwnd) return false;
    DWORD pid{};
    const DWORD tid = GetWindowThreadProcessId(hwnd, &pid);
    if (!IsWindow(hwnd) || pid != GetCurrentProcessId() || tid != windowThread ||
        GetPropW(hwnd, property) != reinterpret_cast<HANDLE>(generation.load()) ||
        !owner || WaitForSingleObject(owner, 0) != WAIT_TIMEOUT) {
        EndLocked();
        return false;
    }
    return true;
}
void ClientSize(HWND hwnd, uint32_t& width, uint32_t& height) {
    // 临时使用物理像素坐标，随后恢复宿主线程原本的 DPI 设置。
    DPI_AWARENESS_CONTEXT previous = SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    RECT rect{};
    if (GetClientRect(hwnd, &rect)) {
        width = static_cast<uint32_t>(rect.right - rect.left);
        height = static_cast<uint32_t>(rect.bottom - rect.top);
    }
    if (previous) SetThreadDpiAwarenessContext(previous);
}
bool InputValid(const char* value) {
    return value && (!strcmp(value, "normal") || !strcmp(value, "windows") || !strcmp(value, "dx"));
}
}

int SelectInputFrame(uint64_t version) {
    AcquireSRWLockExclusive(&intervalLock);
    bool selected = false;
    // -1 固定当前快照；其他值表示每次采集后需要跳过的输入帧数。
    // 迟到的旧绑定帧不消耗新绑定的间隔计数。
    if (version == generation.load() && captureInterval >= 0) {
        if (inputsToSkip == 0) {
            selected = true;
            inputsToSkip = captureInterval;
        } else {
            --inputsToSkip;
        }
    }
    ReleaseSRWLockExclusive(&intervalLock);
    return selected ? 1 : 0;
}

// 呈现线程只尝试取得状态锁；读取/控制忙碌时跳过，绝不等待 CPU 读回。
static bool SubmitImpl(uint64_t version, IDXGISwapChain* chain, ID3D12CommandQueue* queue,
                       ID3D11Texture2D* texture, const D3D11_BOX* region, bool fromWgc,
                       HRESULT hr, double elapsedMs) {
    if (!TryAcquireSRWLockExclusive(&stateLock)) return false;
    struct Unlock { ~Unlock() { ReleaseSRWLockExclusive(&stateLock); } } unlock;
    if (version != generation.load() || !AliveLocked()) return true;
    if (fromWgc != (captureBackend.load() == FS_CAIJI_WGC)) return true;
    // 锁帧与发布共用状态锁；此处再次核对，避免采集线程在锁帧切换时发布竞态帧。
    if (CapturePaused()) return true;
    const auto started = std::chrono::steady_clock::now();
    int index = -1;
    for (int i = 0; i < 3; ++i)
        if (i != latest && (!slots[i] || !slots[i]->users)) { index = i; break; }
    // 两个旧快照被持有时，第三槽可能正好是最新帧；无人持有即可原地更新。
    // 状态锁阻止新租约进入，已有 GPU/CPU 读取均计入 users，不会覆盖调用方正在使用的纹理。
    if (index < 0 && latest >= 0 && !slots[latest]->users) index = latest;
    if (index < 0 && SUCCEEDED(hr)) { ++statistics.skippedFrames; return true; }
    try {
        if (SUCCEEDED(hr)) {
            if (!slots[index]) slots[index] = std::make_unique<replica::GpuSlot>();
            if (fromWgc) hr = texture && region
                ? replica::SnapshotRegion(texture, *region, *slots[index]) : E_INVALIDARG;
            else hr = captureBackend.load() == FS_CAIJI_OBS
                ? obs::Snapshot(chain, queue, *slots[index], target.load())
                : replica::Snapshot(chain, queue, *slots[index]);
        }
    } catch (...) { hr = E_OUTOFMEMORY; }
    state.detail = hr;
    if (FAILED(hr)) {
        // 原地更新失败后不能再宣称旧内容有效，资源可能已为新尺寸重建。
        if (index >= 0 && index == latest) {
            latest = -1;
            state.hasFrame = 0;
            state.frame = {};
        }
        const bool pending = hr == replica::kQueuePending || hr == replica::kQueueProbePending ||
            hr == replica::kQueueNoSubmit || hr == replica::kQueueNoBarrier || hr == obs::kWaiting;
        if (!pending) ++state.failedFrames;
        state.result = pending ? (state.hasFrame ? FS_CHENGGONG : FS_DENGDAISHOUZHEN) :
            hr == obs::kWindowMismatch ? FS_CHUANGKOU_BUPIPEI :
            hr == obs::kMemoryCapture || hr == DXGI_ERROR_UNSUPPORTED ? FS_BUZHICHI : FS_CAIJI_SHIBAI;
        return true;
    }
    auto& slot = *slots[index];
    FsZhen frame{};
    frame.size = sizeof(frame); frame.version = FSDM_BANBEN;
    frame.width = slot.width; frame.height = slot.height; frame.stride = slot.width * 4;
    frame.backend = slot.backend;
    ClientSize(target.load(), frame.clientWidth, frame.clientHeight);
    frame.requiredBytes = uint64_t{slot.width} * slot.height * 4;
    frame.frameNumber = ++state.capturedFrames;
    frame.bindingVersion = version;
    LARGE_INTEGER qpc{}, frequency{};
    QueryPerformanceCounter(&qpc); QueryPerformanceFrequency(&frequency);
    frame.captureQpc = static_cast<uint64_t>(qpc.QuadPart);
    frame.qpcFrequency = static_cast<uint64_t>(frequency.QuadPart);
    frame.captureMs = elapsedMs + std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - started).count();
    slot.frame = frame; latest = index;
    state.frame = frame; state.hasFrame = 1; state.result = FS_CHENGGONG;
    return true;
}

void Submit(uint64_t version, IDXGISwapChain* chain, ID3D12CommandQueue* queue,
            HRESULT hr, double elapsedMs) {
    SubmitImpl(version, chain, queue, nullptr, nullptr, false, hr, elapsedMs);
}

bool SubmitWgc(uint64_t version, ID3D11Texture2D* texture, const D3D11_BOX* region,
               HRESULT hr, double elapsedMs) {
    return SubmitImpl(version, nullptr, nullptr, texture, region, true, hr, elapsedMs);
}

void Monitor() {
    // 后台只检查绑定生命周期。状态输出由找图工作台的 log 命令按次数控制，
    // 避免这里持续打印，打断用户正在输入的命令。
    Lock lock;
    AliveLocked();
}
}

int WINAPI ZhuaTuBegin(HWND hwnd, const char* display, const char* mouse, const char* keypad, int mode) {
    using namespace fsdm;
    if (!hwnd || !display || !InputValid(mouse) || !InputValid(keypad)) return FS_CANSHU_CUOWU;
    const uint32_t selected = !strcmp(display, "dx") ? FS_CAIJI_DX :
        !strcmp(display, "obs") ? FS_CAIJI_OBS :
        !strcmp(display, "wgc") ? FS_CAIJI_WGC : FS_CAIJI_WEIXUAN;
    if (!selected || (mode != 0 && mode != 1)) return FS_BUZHICHI;
    DWORD pid{};
    DWORD tid = GetWindowThreadProcessId(hwnd, &pid);
    if (!IsWindow(hwnd) || pid != GetCurrentProcessId()) return FS_CANSHU_CUOWU;
    // 等待仅针对拦截初始化，不等待首帧；给没有呈现的窗口保留“等待首帧”状态。
    const ULONGLONG deadline = GetTickCount64() + 7000;
    while (!startup.load() && GetTickCount64() < deadline) Sleep(1);
    if (!startup.load()) return FS_CHUSHIHUA_CHAOSHI;
    if (startup.load() != 1) return FS_CHUSHIHUA_SHIBAI;
    Lock lock;
    AliveLocked();
    if (target.load()) {
        if (target.load() == hwnd && state.mode == static_cast<uint32_t>(mode) &&
            !strcmp(display, state.display) &&
            !strcmp(mouse, state.mouse) && !strcmp(keypad, state.keypad)) return FS_CHENGGONG;
        return FS_YIBANGDING;
    }
    HANDLE thread = OpenThread(SYNCHRONIZE, FALSE, GetCurrentThreadId());
    if (!thread) { state.detail = HRESULT_FROM_WIN32(GetLastError()); return FS_CAIJI_SHIBAI; }
    const uint64_t version = generation.fetch_add(1) + 1;
    if (!SetPropW(hwnd, property, reinterpret_cast<HANDLE>(version))) {
        state.detail = HRESULT_FROM_WIN32(GetLastError());
        CloseHandle(thread);
        return FS_CAIJI_SHIBAI;
    }
    if (selected == FS_CAIJI_OBS || selected == FS_CAIJI_WGC) {
        HRESULT detail{};
        int result{};
        try { result = selected == FS_CAIJI_OBS
            ? obs::Begin(hwnd, detail, deadline) : wgc::Begin(hwnd, version, detail, deadline); }
        catch (...) {
            obs::End(); wgc::End(); result = FS_CHUSHIHUA_SHIBAI; detail = E_FAIL;
        }
        if (result != FS_CHENGGONG) {
            RemovePropW(hwnd, property); CloseHandle(thread);
            state.detail = detail; return result;
        }
    }
    owner = thread;
    windowThread = tid;
    state = {};
    state.result = FS_DENGDAISHOUZHEN;
    state.bound = 1;
    state.hwnd = reinterpret_cast<uint64_t>(hwnd);
    state.bindingVersion = version;
    state.ownerThreadId = GetCurrentThreadId();
    state.mode = static_cast<uint32_t>(mode);
    strcpy_s(state.display, display);
    strcpy_s(state.mouse, mouse);
    strcpy_s(state.keypad, keypad);
    captureBackend.store(selected);
    ResetInterval(0);
    target.store(hwnd); // 采集方式、配置和版本准备完毕，最后开启采集。
    return FS_CHENGGONG;
}

int WINAPI ZhuaTuEnd(void) {
    fsdm::Lock lock;
    fsdm::EndLocked();
    return FS_CHENGGONG;
}

int WINAPI ZhuaTuSetCaptureFrameInterval(int interval) {
    using namespace fsdm;
    if (interval < -1) return FS_CANSHU_CUOWU;
    Lock lock;
    if (!AliveLocked()) return FS_WEIBANGDING;
    if (interval == -1) {
        if (lockedFrameToken) return FS_CHENGGONG;
        if (latest < 0 || !state.hasFrame) return FS_DENGDAISHOUZHEN;
        const uint64_t token = ReserveLocked(latest);
        if (!token) return FS_ZIYUAN_MANG;
        lockedFrameToken = token;
        lockedFrameSlot = latest;
    } else if (lockedFrameToken) {
        ReleaseLocked(lockedFrameToken);
        lockedFrameToken = 0;
        lockedFrameSlot = -1;
    }
    ResetInterval(interval);
    return FS_CHENGGONG;
}

int WINAPI ZhuaTuCopyFrame(void* pixels, uint64_t capacity, FsZhen* frame) {
    using namespace fsdm;
    if (!frame || frame->size != sizeof(FsZhen)) return FS_CANSHU_CUOWU;
    uint64_t token{}; replica::GpuSlot* slot{}; int index = -1;
    {
        Lock lock;
        *frame = {}; frame->size = sizeof(*frame); frame->version = FSDM_BANBEN;
        if (!AliveLocked()) return FS_WEIBANGDING;
        const bool pinned = lockedFrameToken && lockedFrameSlot >= 0 && slots[lockedFrameSlot];
        index = pinned ? lockedFrameSlot : latest;
        if (index < 0) return state.result;
        if (!pinned && !state.hasFrame) return state.result;
        *frame = slots[index]->frame;
        if (!pinned && state.result != FS_CHENGGONG) return state.result;
        // 只查容量或缓冲区不足时，不创建暂存资源，不等待 GPU，不读取像素。
        if (capacity < frame->requiredBytes) return FS_HUANCHONG_BUZU;
        if (!pixels) return FS_CANSHU_CUOWU;
        token = ReserveLocked(index);
        if (!token) return FS_ZIYUAN_MANG;
        slot = slots[index].get();
    }
    const auto started = std::chrono::steady_clock::now();
    HRESULT hr = E_FAIL;
    try { hr = replica::Read(*slot, static_cast<BYTE*>(pixels)); } catch (...) { hr = E_OUTOFMEMORY; }
    const double ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - started).count();
    Lock lock;
    const bool current = AliveLocked() && frame->bindingVersion == generation.load();
    ReleaseLocked(token);
    if (!current) return FS_WEIBANGDING;
    const int result = SUCCEEDED(hr) ? FS_CHENGGONG :
        hr == HRESULT_FROM_WIN32(WAIT_TIMEOUT) ? FS_GPU_CHAOSHI : FS_CAIJI_SHIBAI;
    ++statistics.cpuReadbacks;
    statistics.lastReadbackMs = ms; statistics.lastReadbackResult = result;
    statistics.readbackFrame = frame->frameNumber;
    statistics.readbackBindingVersion = frame->bindingVersion;
    frame->readbackMs = ms;
    if (FAILED(hr)) state.detail = hr;
    return result;
}

int WINAPI ZhuaTuAcquireGpuFrame(FsGpuZhen* output) {
    using namespace fsdm;
    if (!output || output->size != sizeof(*output)) return FS_CANSHU_CUOWU;
    FsGpuZhen value{}; replica::GpuSlot* slot{}; bool pinned{};
    {
        Lock lock;
        *output = {}; output->size = sizeof(*output); output->version = FSDM_BANBEN;
        if (!AliveLocked()) return FS_WEIBANGDING;
        pinned = lockedFrameToken && lockedFrameSlot >= 0 && slots[lockedFrameSlot];
        const int index = pinned ? lockedFrameSlot : latest;
        if (index < 0 || (!pinned && !state.hasFrame)) return state.result;
        if (!pinned && state.result != FS_CHENGGONG) return state.result;
        value.token = ReserveLocked(index);
        if (!value.token) return FS_ZIYUAN_MANG;
        slot = slots[index].get();
        value.size = sizeof(value); value.version = FSDM_BANBEN;
        value.sharedHandle = reinterpret_cast<uint64_t>(slot->shared);
        value.textureFormat = slot->format; value.captureBackend = captureBackend.load();
        value.adapterLuidLow = slot->luid.LowPart; value.adapterLuidHigh = slot->luid.HighPart;
        value.flip = slot->flip; value.frame = slot->frame;
    }
    const HRESULT hr = replica::Wait(*slot);
    Lock lock;
    if (!AliveLocked() || value.frame.bindingVersion != generation.load()) {
        ReleaseLocked(value.token); return FS_WEIBANGDING;
    }
    if (FAILED(hr)) {
        ReleaseLocked(value.token); state.detail = hr;
        return hr == HRESULT_FROM_WIN32(WAIT_TIMEOUT) ? FS_GPU_CHAOSHI : FS_CAIJI_SHIBAI;
    }
    *output = value;
    return FS_CHENGGONG;
}

int WINAPI ZhuaTuReleaseGpuFrame(uint64_t token) {
    using namespace fsdm;
    Lock lock;
    if (token) for (const auto& lease : leases) if (lease.token == token) {
        ReleaseLocked(token); return FS_CHENGGONG;
    }
    return FS_CANSHU_CUOWU;
}

int WINAPI ZhuaTuGetCaptureStatus(FsCaijiZhuangtai* output) {
    using namespace fsdm;
    if (!output || output->size != sizeof(*output)) return FS_CANSHU_CUOWU;
    Lock lock; AliveLocked();
    *output = statistics; output->size = sizeof(*output); output->version = FSDM_BANBEN;
    output->captureBackend = captureBackend.load();
    for (const auto& slot : slots) if (slot) {
        ++output->gpuSlots; output->activeLeases += slot->users;
    }
    if (latest >= 0) output->textureFormat = slots[latest]->format;
    return FS_CHENGGONG;
}

int WINAPI ZhuaTuGetStatus(FsZhuangtai* status) {
    using namespace fsdm;
    if (!status || status->size != sizeof(FsZhuangtai)) return FS_CANSHU_CUOWU;
    Lock lock;
    AliveLocked();
    *status = state;
    status->size = sizeof(*status);
    status->version = FSDM_BANBEN;
    status->initializing = startup.load() == 0;
    if (!status->bound) {
        status->result = startup.load() == -1 ? FS_CHUSHIHUA_SHIBAI : FS_WEIBANGDING;
        // OBS 依赖错误发生在原生拦截就绪之后；不能用成功的启动结果覆盖它。
        if (!status->detail) status->detail = startupDetail.load();
    }
    return FS_CHENGGONG;
}

int WINAPI ZhuaTuMapPoint(double x, double y, double* outX, double* outY, int direction) {
    using namespace fsdm;
    if (!outX || !outY || !std::isfinite(x) || !std::isfinite(y) || (direction != 0 && direction != 1))
        return FS_CANSHU_CUOWU;
    Lock lock;
    if (!AliveLocked()) return FS_WEIBANGDING;
    if (!state.hasFrame) return state.result;
    uint32_t cw{}, ch{};
    ClientSize(target.load(), cw, ch);
    if (!cw || !ch) return FS_CAIJI_SHIBAI;
    const double fromW = direction == 0 ? cw : state.frame.width;
    const double fromH = direction == 0 ? ch : state.frame.height;
    const double toW = direction == 0 ? state.frame.width : cw;
    const double toH = direction == 0 ? state.frame.height : ch;
    if (x < 0 || y < 0 || x >= fromW || y >= fromH) return FS_CANSHU_CUOWU;
    *outX = x * toW / fromW;
    *outY = y * toH / fromH;
    return FS_CHENGGONG;
}
