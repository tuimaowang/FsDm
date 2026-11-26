// 同进程仅由本模块接收命令。命令线程不退出，因此它创建的绑定可持续使用。
#include "ZhaoTu.h"
#include "Peizhi.h"
#include "Haoshi.h"
#include "ZifuchuanNeicun.h"
#include "TongyongJieguo.h"
#include "WenziShibie.h"
#include "ShebeiZhanyong.h"
#include "../Gonggong/Gongzuotai.h"
#include <algorithm>
#include <sstream>
#include <string>
#include <limits>
#include <stdexcept>
#include <vector>
#include <cwctype>
#include <chrono>
#include <cmath>
#include <locale>
#include <type_traits>
#include <initializer_list>
#include <utility>
#include <mutex>

namespace {
// 编号输出独立于控制台输入，用户尚未按回车时也能继续观察新快照。
// 同一把锁串行处理开启、关闭和输出，保证关闭返回后没有残留编号继续写出。
std::mutex frameOutputMutex;
HANDLE frameOutputWake{}, frameOutputConsole{};
bool frameOutputEnabled{};
uint64_t frameOutputBinding{}, frameOutputNumber{};

DWORD WINAPI FrameOutputWorker(void*) {
    DWORD wait = INFINITE;
    for (;;) {
        if (WaitForSingleObject(frameOutputWake, wait) == WAIT_FAILED) return 0;
        std::lock_guard guard(frameOutputMutex);
        if (frameOutputEnabled) {
            FsZhuangtai current{};
            current.size = sizeof(current);
            if (GetStatus(&current) != FS_CHENGGONG || !current.bound ||
                current.bindingVersion != frameOutputBinding) {
                frameOutputEnabled = false;
            } else if (current.hasFrame && current.frame.frameNumber != frameOutputNumber) {
                wchar_t number[32];
                swprintf_s(number, L"%llu", static_cast<unsigned long long>(current.frame.frameNumber));
                FsConsoleLine(frameOutputConsole, number);
                frameOutputNumber = current.frame.frameNumber;
            }
        }
        // 关闭时休眠至下次开启，开启时只查询状态，不读取像素或持有 GPU 快照。
        wait = frameOutputEnabled ? 10 : INFINITE;
    }
}

struct Argument { std::wstring value; bool quoted{}; };

// 引号内原样保留反斜杠，便于直接输入 Windows 路径；参数之间使用逗号。
bool ParseCall(const std::wstring& input, std::wstring& name, std::vector<Argument>& args) {
    size_t pos = input.find_first_not_of(L" \t\r\n");
    if (pos == std::wstring::npos) return false;
    const size_t begin = pos;
    while (pos < input.size() && (iswalnum(input[pos]) || input[pos] == L'_')) ++pos;
    name = input.substr(begin, pos - begin);
    while (pos < input.size() && iswspace(input[pos])) ++pos;
    if (name.empty() || pos == input.size() || input[pos++] != L'(') return false;
    for (;;) {
        while (pos < input.size() && iswspace(input[pos])) ++pos;
        if (pos == input.size()) return false;
        if (input[pos] == L')') { ++pos; break; }
        Argument arg;
        if (input[pos] == L'"') {
            arg.quoted = true;
            const size_t start = ++pos;
            pos = input.find(L'"', pos);
            if (pos == std::wstring::npos) return false;
            arg.value = input.substr(start, pos++ - start);
            while (pos < input.size() && iswspace(input[pos])) ++pos;
        } else {
            const size_t start = pos;
            while (pos < input.size() && input[pos] != L',' && input[pos] != L')') ++pos;
            arg.value = input.substr(start, pos - start);
            const auto end = arg.value.find_last_not_of(L" \t\r\n");
            if (end == std::wstring::npos) return false;
            arg.value.resize(end + 1);
        }
        args.push_back(std::move(arg));
        if (pos == input.size()) return false;
        if (input[pos] == L')') { ++pos; break; }
        if (input[pos++] != L',') return false;
        const auto next = input.find_first_not_of(L" \t\r\n", pos);
        if (next == std::wstring::npos || input[next] == L')') return false;
    }
    return input.find_first_not_of(L" \t\r\n", pos) == std::wstring::npos;
}
bool ParseText(const Argument& arg, std::string& out) {
    if (!arg.quoted) return false;
    if (arg.value.empty()) { out.clear(); return true; }
    const int n = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, arg.value.data(),
        static_cast<int>(arg.value.size()), nullptr, 0, nullptr, nullptr);
    if (!n) return false;
    out.resize(n);
    return WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, arg.value.data(),
        static_cast<int>(arg.value.size()), out.data(), n, nullptr, nullptr) == n;
}
bool ParseInt(const Argument& arg, int& out) {
    if (arg.quoted || arg.value.empty()) return false;
    try {
        size_t used{};
        const size_t offset = (arg.value[0] == L'-' || arg.value[0] == L'+') ? 1 : 0;
        const int base = arg.value.compare(offset, 2, L"0x") == 0 ||
            arg.value.compare(offset, 2, L"0X") == 0 ? 16 : 10;
        const long long n = std::stoll(arg.value, &used, base);
        if (used != arg.value.size() || n < INT_MIN || n > INT_MAX) return false;
        out = static_cast<int>(n); return true;
    } catch (...) { return false; }
}
// uintptr_t 与当前进程地址等宽；解析十进制/十六进制，禁止负数和超出指针范围的值。
// 这里只转换数值，内存是否可读由 LoadPicByte 内部检查，工作台不会直接访问此地址。
bool ParseAddress(const Argument& arg,uintptr_t& out) {
    if(arg.quoted || arg.value.empty() || arg.value[0]==L'-') return false;
    try {
        size_t used{};
        const size_t offset=arg.value[0]==L'+' ? 1 : 0;
        const int base=arg.value.compare(offset,2,L"0x")==0 || arg.value.compare(offset,2,L"0X")==0 ? 16 : 10;
        const auto n=std::stoull(arg.value,&used,base);
        if(used!=arg.value.size() || n>UINTPTR_MAX) return false;
        out=static_cast<uintptr_t>(n);return true;
    } catch(...) { return false; }
}
bool ParseHwnd(const Argument& arg, HWND& out) {
    if (arg.quoted || arg.value.empty() || arg.value[0] == L'-') return false;
    try {
        size_t used{};
        const int base = arg.value.compare(0, 2, L"0x") == 0 ||
            arg.value.compare(0, 2, L"0X") == 0 ? 16 : 10;
        const auto n = std::stoull(arg.value, &used, base);
        if (used != arg.value.size()) return false;
        out = reinterpret_cast<HWND>(static_cast<uintptr_t>(n)); return true;
    } catch (...) { return false; }
}
// 固定使用小数点，不受宿主的区域设置影响；拒绝 NaN、无穷大和尾随字符。
bool ParseDouble(const Argument& arg, double& out) {
    if (arg.quoted || arg.value.empty()) return false;
    std::wistringstream input(arg.value);
    input.imbue(std::locale::classic());
    input >> std::noskipws >> out;
    return input && input.eof() && std::isfinite(out);
}
// UTF-8仅用于实际文字与文件名；坐标、索引和RGB直接读取结构体字段。
std::wstring WideText(const char* value) {
    if(!value || !*value) return {};
    const int count=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,value,-1,nullptr,0);
    if(!count) return L"<UTF-8 无效>";
    std::wstring text(count,0);
    MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,value,-1,text.data(),count);
    text.pop_back();return text;
}
// 逐项输出完整数组，不使用省略号或固定长度结果缓冲区。
void PrintItems(HANDLE output,const FsTongyongJieguo* result) {
    if(!result) return;
    if(result->fullText && *result->fullText) {
        const auto text=L"fullText=\"" + WideText(result->fullText) + L"\"";
        FsConsoleLine(output,text.c_str());
    }
    for(uint32_t i=0;i<result->count;++i) {
        const auto& item=result->items[i];
        std::wstring text=L"items["+std::to_wstring(i)+L"]:";
        if(item.fields&FS_JG_ID) text+=L" id="+std::to_wstring(item.id);
        if(item.fields&FS_JG_XY) text+=L" x="+std::to_wstring(item.x)+L" y="+std::to_wstring(item.y);
        if(item.fields&FS_JG_COLOR) {
            wchar_t rgb[32];swprintf_s(rgb,L" color=0x%06x",item.color);text+=rgb;
        }
        if(item.fields&FS_JG_TEXT) text+=L" text=\""+WideText(item.text)+L"\"";
        if(item.fields&FS_JG_FILE) text+=L" file=\""+WideText(item.file)+L"\"";
        FsConsoleLine(output,text.c_str());
    }
}
// 本行统计整个设备在本次 OCR 调用覆盖区间内的平均负载，附加采样开销另列。
// 计数器不可读和调用过短分别说明原因，避免将测量失败显示成 0%。
void PrintCallUsage(HANDLE output, const fsdm::zhanyong::CallUsage& usage) {
    const auto percentage = [&](double value) -> std::wstring {
        if (usage.tooShort) return L"采样时间过短";
        if (value < 0.0) return L"不可用";
        wchar_t formatted[32];
        swprintf_s(formatted, L"%.2f%%", value);
        return formatted;
    };
    std::wstring text = L"本次调用期间平均占用 CPU=" + percentage(usage.cpu) +
        L"，GPU（最繁忙引擎）=" + percentage(usage.gpu);
    if (usage.tooShort) text += L"（调用不足 10 毫秒）";
    wchar_t overhead[128];
    swprintf_s(overhead, L"，采样初始化=%.3f 毫秒，采样收尾=%.3f 毫秒",
        usage.setupMs, usage.finishMs);
    text += overhead;
    FsConsoleLine(output, text.c_str());
}
// 由公开声明决定是否将整数按十进制显示，避免 x64 下编号与地址的类型别名相同。
template<bool DecimalInteger = false, class T>
void PrintCallResult(HANDLE output, T result, double elapsed,
                     std::initializer_list<std::pair<const wchar_t*, int>> coordinates,
                     const fsdm::zhanyong::CallUsage* usage = nullptr) {
    // 无论下方从哪个分段提前返回，均只回收本次输出，外部保留的同名函数结果不受影响。
    const char* stringResult{};
    if constexpr (std::is_same_v<T, const char*>) stringResult = result;
    fsdm::zifuchuan::WorkbenchResult owned(stringResult);
    const FsTongyongJieguo* nativeResult{};
    if constexpr (std::is_same_v<T,const FsTongyongJieguo*>) nativeResult=result;
    fsdm::jieguo::WorkbenchResult nativeOwned(nativeResult);
    std::wstring text = L"返回值: ";
    // 地址单独显示为十六进制；uintptr_t 与 64 位整数等宽，不能转成 int。
    if constexpr (std::is_same_v<T,const FsTongyongJieguo*>) {
        if(!result) text+=L"<内存分配失败>";
        else {
            wchar_t fields[128];
            swprintf_s(fields,L"status=%d，detail=0x%08X，count=%u",
                result->status,static_cast<unsigned>(result->detail),result->count);
            text+=fields;
        }
    }
    else if constexpr (std::is_same_v<T, uintptr_t> && !DecimalInteger) {
        wchar_t address[32]; swprintf_s(address, L"0x%llX", static_cast<unsigned long long>(result));
        text += address;
    }
    else if constexpr (std::is_integral_v<T>) text += std::to_wstring(result);
    else if constexpr (std::is_floating_point_v<T>) {
        // 百分比导出直接返回小数，固定显示两位，0 和 -1 均按数值打印。
        wchar_t value[64];
        swprintf_s(value, L"%.2f", static_cast<double>(result));
        text += value;
    }
    else if (!result) text += L"<内存分配失败>";
    else {
        text += L'"';
        if (result && *result) {
            const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, result, -1, nullptr, 0);
            if (length > 0) {
                std::wstring value(length, 0);
                MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, result, -1, value.data(), length);
                value.pop_back();
                text += value;
            } else text += L"<UTF-8 无效>";
        }
        text += L'"';
    }
    for (const auto& [label, value] : coordinates)
        text += std::wstring(L"，") + label + L"=" + std::to_wstring(value);
    const auto& timing = fsdm::haoshi::current;
    const bool detailed = timing.operation != fsdm::haoshi::Operation::none;
    wchar_t time[96];
    swprintf_s(time, detailed ? L"，总耗时: %.3f 毫秒" : L"，耗时: %.3f 毫秒", elapsed);
    text += time;
    if constexpr (std::is_same_v<T,const FsTongyongJieguo*>) {
        FsConsoleLine(output,text.c_str());
        PrintItems(output,nativeResult);
        text.clear();
        if(!detailed) {
            if (usage) PrintCallUsage(output, *usage);
            return;
        }
    }
    if (detailed) {
        // 同一次调用的各阶段互不重叠。其他包含参数检查、结果整理及资源释放等开销。
        const auto append = [&](const wchar_t* label, const fsdm::haoshi::Stage& stage) {
            text += label;
            if (!stage.executed) text += L"未执行";
            else {
                wchar_t value[64]; swprintf_s(value, L"%.3f 毫秒", stage.ms);
                text += value;
            }
        };
        text += L"\r\n";
        if(timing.operation==fsdm::haoshi::Operation::matchNames) {
            append(L"文件枚举: ",timing.prepare);
            swprintf_s(time,L"，其他: %.3f 毫秒",std::max(0.,elapsed-timing.prepare.ms));
            text+=time;FsConsoleLine(output,text.c_str());return;
        }
        append(L"采集取帧: ", timing.acquire);
        if(timing.operation==fsdm::haoshi::Operation::screenData ||
            timing.operation==fsdm::haoshi::Operation::displayDead) {
            append(timing.operation==fsdm::haoshi::Operation::screenData ?
                L"，区域整理: " : L"，区域整理及比较: ",timing.search);
            if(timing.operation==fsdm::haoshi::Operation::displayDead)
                append(L"，检测等待: ",timing.wait);
            swprintf_s(time,L"，其他: %.3f 毫秒",std::max(0.,elapsed-timing.acquire.ms-timing.search.ms-timing.wait.ms));
            text+=time;FsConsoleLine(output,text.c_str());return;
        }
        if(timing.operation==fsdm::haoshi::Operation::ocr) {
            append(L"，首次模型准备/初始化: ",timing.initialize);
            append(L"，预处理: ",timing.preprocess);
            append(L"，文字检测: ",timing.detect);
            append(L"，文字识别: ",timing.recognize);
            append(L"，结果整理/找字: ",timing.result);
            const double other=std::max(0.,elapsed-timing.acquire.ms-timing.initialize.ms-
                timing.preprocess.ms-timing.detect.ms-timing.recognize.ms-timing.result.ms);
            swprintf_s(time,L"，其他: %.3f 毫秒",other);text+=time;
            FsOcrZhuangtai ocr{};ocr.size=sizeof(ocr);GetOcrStatus(&ocr);
            text+=ocr.engine?L"\r\nOCR=dict":ocr.backend?L"\r\nOCR=model/directml（检测、识别含执行等待及数据传回）":L"\r\nOCR=model/cpu";
            if(fsdm::wenzi::OcrResultReused()) text+=L"，帧缓存=复用旧帧，结果缓存=命中";
            else if(timing.acquire.executed)
                text+=fsdm::wenzi::OcrFrameReused()?L"，帧缓存=复用旧帧，结果缓存=未命中":L"，帧缓存=新帧，结果缓存=未命中";
            if(ocr.message[0]) {
                const int n=MultiByteToWideChar(CP_UTF8,0,ocr.message,-1,nullptr,0);
                if(n>0) {std::wstring error(n,0);MultiByteToWideChar(CP_UTF8,0,ocr.message,-1,error.data(),n);
                    error.pop_back();text+=L"，诊断: "+error;}
            }
            FsConsoleLine(output,text.c_str());
            if (usage) PrintCallUsage(output, *usage);
            return;
        }
        if(timing.gpu) append(L"，GPU资源准备: ",timing.resources);
        if (timing.operation == fsdm::haoshi::Operation::findPicture)
            append(L"，模板准备: ", timing.prepare);
        const wchar_t* operation = timing.operation == fsdm::haoshi::Operation::compareColor ? L"，比色" :
            timing.operation == fsdm::haoshi::Operation::getColor ? L"，取色" :
            timing.operation == fsdm::haoshi::Operation::findColor ? L"，找色" : L"，找图";
        // GPU 阶段测量 CPU 提交命令的耗时；实际执行的等待归入后面的结果读取阶段。
        // 在字段名中说明含义，避免把提交时间误认为 GPU 已经完成计算的时间。
        const std::wstring searchLabel = std::wstring(operation) + (timing.gpu ? L"提交: " : L": ");
        append(searchLabel.c_str(), timing.search);
        if(timing.gpu) append(L"，GPU 等待及结果读取: ",timing.result);
        const double other = (std::max)(0.0, elapsed - timing.acquire.ms - timing.prepare.ms -
            timing.search.ms - timing.resources.ms - timing.result.ms);
        swprintf_s(time, L"，其他: %.3f 毫秒", other);
        text += time;
        if(timing.gpu) {
            FsChazhaoZhuangtai find{};find.size=sizeof(find);GetFindStatus(&find);
            if(find.result!=FS_CHENGGONG) {
                wchar_t error[96];
                swprintf_s(error,L"，查找状态=%d detail=0x%08X",find.result,static_cast<unsigned>(find.detail));
                text+=error;
            }
        }
    }
    FsConsoleLine(output, text.c_str());
}
bool CallExport(const std::wstring& name, const std::vector<Argument>& args, HANDLE output) {
#include "ZidongDiaoyong.inc"
    return false;
}
// 查询状态只取元数据，不调用 CopyFrame，所以显示日志不会触发 CPU 像素读回。
void ShowStatus(HANDLE output, bool periodic, DWORD remaining = 0) {
    FsZhuangtai state{}; state.size = sizeof(state); GetStatus(&state);
    FsCaijiZhuangtai extra{}; extra.size = sizeof(extra); GetCaptureStatus(&extra);
    FsChazhaoZhuangtai find{};find.size=sizeof(find);GetFindStatus(&find);
    wchar_t readback[160];
    if (!extra.cpuReadbacks) wcscpy_s(readback, L"未执行");
    else swprintf_s(readback, L"%.3fms 快照=%llu 结果=%d", extra.lastReadbackMs,
        extra.readbackFrame, extra.lastReadbackResult);
    wchar_t text[768];
    swprintf_s(text, L"[%ls] 后端=%ls 绑定=%u HWND=0x%llX 状态=%d detail=0x%08X %ls "
        L"快照=%llu 画面=%ux%u 客户区=%ux%u 提交=%.3fms CPU读回=%ls "
        L"槽位=%u 租约=%u 跳过=%llu 剩余=%lu 查找=%ls 查找状态=%d/0x%08X "
        L"处理设备=%llu 创建=%llu 着色器=%llu 资源=%llu 模板上传=%llu 结果读回=%lluB 键鼠=尚未实现",
        periodic ? L"ZhuaTu" : L"ZhaoTu",
        extra.captureBackend == FS_CAIJI_DX ? L"dx" : extra.captureBackend == FS_CAIJI_OBS ? L"obs" :
            extra.captureBackend == FS_CAIJI_WGC ? L"wgc" : L"未选择",
        state.bound, state.hwnd, state.result, static_cast<unsigned>(state.detail),
        state.hasFrame ? L"GPU已缓存" : L"等待首帧", state.capturedFrames,
        state.frame.width, state.frame.height, state.frame.clientWidth, state.frame.clientHeight,
        state.frame.captureMs, readback, extra.gpuSlots, extra.activeLeases, extra.skippedFrames, remaining,
        find.backend?L"gpu":L"cpu",find.result,static_cast<unsigned>(find.detail),
        find.deviceSerial,find.devicesCreated,find.shaderBuilds,find.resourceBuilds,find.templateUploads,
        find.resultReadbackBytes);
    FsConsoleLine(output, text);
    if(!periodic) {
        FsOcrZhuangtai ocr{};ocr.size=sizeof(ocr);GetOcrStatus(&ocr);
        swprintf_s(text,L"[ZhaoTu] OCR 引擎=%ls 后端=%ls 模型=%d 字库=%d 状态=%d "
            L"已加载会话=%u 累计创建会话=%llu DirectML设备=%llu",
            ocr.engine?L"dict":L"model",ocr.backend?L"directml":L"cpu",ocr.modelIndex,ocr.dictIndex,
            ocr.result,ocr.loadedSessions,ocr.sessionsCreated,ocr.devicesCreated);
        FsConsoleLine(output,text);
    }
}
}

bool FsSetFrameOutput(int enabled, const FsZhuangtai& current) {
    if constexpr (!kGongzuotaiKaiqi) return true;
    std::lock_guard guard(frameOutputMutex);
    if (!enabled || !current.bound) {
        frameOutputEnabled = false;
        if (frameOutputWake) SetEvent(frameOutputWake);
        return true;
    }
    if (frameOutputEnabled && frameOutputBinding == current.bindingVersion) return true;
    if (!frameOutputWake) {
        // 模块已在 DllMain 中固定至进程退出。监视线程和两个句柄按同一生命周期复用。
        frameOutputConsole = FsOpenConsole(true);
        if (frameOutputConsole == INVALID_HANDLE_VALUE) return false;
        frameOutputWake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        HANDLE worker = frameOutputWake
            ? CreateThread(nullptr, 0, FrameOutputWorker, nullptr, 0, nullptr) : nullptr;
        if (!worker) {
            const DWORD error = GetLastError();
            if (frameOutputWake) CloseHandle(frameOutputWake);
            frameOutputWake = nullptr;
            CloseHandle(frameOutputConsole);
            frameOutputConsole = nullptr;
            SetLastError(error);
            return false;
        }
        CloseHandle(worker);
    }
    frameOutputBinding = current.bindingVersion;
    frameOutputNumber = current.hasFrame ? current.frame.frameNumber : 0;
    frameOutputEnabled = true;
    if (frameOutputNumber) {
        wchar_t number[32];
        swprintf_s(number, L"%llu", static_cast<unsigned long long>(frameOutputNumber));
        FsConsoleLine(frameOutputConsole, number);
    }
    SetEvent(frameOutputWake);
    return true;
}

DWORD WINAPI FsWorkbench(void*) {
    if constexpr (!kGongzuotaiKaiqi) return 0;
    HANDLE output = FsOpenConsole(true);
    HANDLE input = CreateFileW(L"CONIN$", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              nullptr, OPEN_EXISTING, 0, nullptr);
    FsConsoleLine(output, L"[ZhaoTu] bind HWND dx|obs|wgc normal normal 0 | unbind | status | log 次数 | help");
    FsConsoleLine(output, L"[ZhaoTu] HWND 支持十进制或 0x 十六进制；键鼠参数仅保存，操作尚未实现。");
    FsConsoleLine(output, L"[ZhaoTu] 默认不连续输出；log 5 每秒显示一次，共 5 次；log 0 取消。");
    FsConsoleLine(output, L"[ZhaoTu] GetFrameNumber(1) 持续输出变化的快照编号，GetFrameNumber(0) 关闭；解绑自动停止。");
    FsConsoleLine(output, L"[ZhaoTu] 可输入函数调用，例如 CapturePng(0, 0, 100, 100, \"C:\\test.png\", 2)。");
    FsConsoleLine(output, L"[ZhaoTu] BindWindow 第二个参数选择 dx/obs/wgc；找图找色默认 gpu，绑定后才采集。");
    FsConsoleLine(output, L"[ZhaoTu] SetFindBackend(\"cpu\") / SetFindBackend(\"gpu\") 切换查找，不重启采集。");
    FsConsoleLine(output, L"[ZhaoTu] SetCaptureFrameInterval(-1) 固定当前帧；传 0 恢复逐帧采集；传 N 跳过 N 个输入帧。");
    FsConsoleLine(output, L"[ZhaoTu] 查找直接显示 status/count 和完整 items 数组，例如 FindColor(0, 0, 100, 100, \"ffffff-000000\", 1.0, 0)。");
    FsConsoleLine(output, L"[ZhaoTu] 工作台显示后只释放本次结果；外部调用用 FreeResult(结果指针) 或 FreeAllResults()。");
    if (input == INVALID_HANDLE_VALUE) {
        if (output != INVALID_HANDLE_VALUE) CloseHandle(output);
        return 0;
    }
    wchar_t buffer[512];
    DWORD length{};
    // 剩余次数只由当前命令线程维护。log 新次数会替换旧计划，不创建额外输出线程。
    DWORD remaining{};
    ULONGLONG nextLog{};
    uint64_t logBinding{};
    while (true) {
        const DWORD wait = WaitForSingleObject(input, 100);
        if (wait == WAIT_TIMEOUT) {
            if (!remaining || GetTickCount64() < nextLog) continue;
            FsZhuangtai state{}; state.size = sizeof(state);
            if (GetStatus(&state) != FS_CHENGGONG || !state.bound ||
                state.bindingVersion != logBinding) {
                remaining = 0; // 自动解绑或外部重新绑定后，不沿用上一绑定的输出计划。
                FsConsoleLine(output, L"[ZhaoTu] 绑定已结束或改变，已取消剩余输出。");
                continue;
            }
            ShowStatus(output, true, --remaining);
            nextLog = GetTickCount64() + 1000;
            continue;
        }
        if (wait != WAIT_OBJECT_0) break;
        // 控制台收到按键松开、鼠标或焦点事件也会唤醒等待。
        // 先取走这些非输入事件，否则刚按完回车的松键事件会让下一次
        // ReadConsoleW 一直等新命令，导致已经设置的定时输出无法继续。
        INPUT_RECORD event{};
        DWORD events{};
        if (!PeekConsoleInputW(input, &event, 1, &events)) break;
        if (!events) continue;
        if (event.EventType != KEY_EVENT || !event.Event.KeyEvent.bKeyDown ||
            (!event.Event.KeyEvent.uChar.UnicodeChar &&
             event.Event.KeyEvent.wVirtualKeyCode != VK_UP &&
             event.Event.KeyEvent.wVirtualKeyCode != VK_DOWN)) {
            if (!ReadConsoleInputW(input, &event, 1, &events)) break;
            continue;
        }
        // 有输入时交给原来的整行读取；用户编辑命令期间暂停定时输出，
        // 回车处理命令后继续剩余次数，避免日志插进正在输入的文字。
        if (!ReadConsoleW(input, buffer, 511, &length, nullptr) || !length) break;
        buffer[length] = 0;
        if (std::wstring(buffer).find(L'(') != std::wstring::npos) {
            std::wstring name;
            std::vector<Argument> args;
            try {
                if (!ParseCall(buffer, name, args)) FsConsoleLine(output, L"函数格式错误，请使用 函数名(参数, ...)。");
                else if (!CallExport(name, args, output)) FsConsoleLine(output, L"找不到该导出函数，请检查名称及大小写。");
            } catch (...) { FsConsoleLine(output, L"调用失败。"); }
            continue;
        }
        std::wistringstream line(buffer);
        std::wstring command;
        line >> command;
        if (command == L"bind") {
            std::wstring handle, display, mouse, keypad, extra;
            int mode{};
            if (!(line >> handle >> display >> mouse >> keypad >> mode) || (line >> extra)) {
                FsConsoleLine(output, L"用法: bind 0x窗口句柄 dx|obs|wgc normal normal 0");
                continue;
            }
            try {
                size_t used{};
                const uint64_t value = std::stoull(handle, &used, 0);
                if (used != handle.size()) throw std::invalid_argument("HWND");
                auto narrow = [](const std::wstring& text) {
                    std::string result;
                    for (const wchar_t ch : text) {
                        if (ch > 127) throw std::invalid_argument("ASCII parameter");
                        result.push_back(static_cast<char>(ch));
                    }
                    return result;
                };
                const int result = BindWindow(reinterpret_cast<HWND>(value), narrow(display).c_str(),
                    narrow(mouse).c_str(), narrow(keypad).c_str(), mode);
                FsConsoleLine(output, result ? L"绑定成功；无呈现时等待首帧。" : L"绑定失败；使用 status 查看错误。");
            } catch (...) { FsConsoleLine(output, L"窗口句柄格式错误。"); }
        } else if (command == L"unbind") {
            remaining = 0;
            FsConsoleLine(output, UnBindWindow() ? L"已解绑，帧缓存已失效。" : L"解绑失败。");
        } else if (command == L"log") {
            std::wstring count, extra;
            if (!(line >> count) || (line >> extra) ||
                count.find_first_not_of(L"0123456789") != std::wstring::npos) {
                FsConsoleLine(output, L"用法: log 次数（非负十进制整数）；log 0 取消。");
                continue;
            }
            try {
                const auto value = std::stoull(count);
                if (value > std::numeric_limits<DWORD>::max()) throw std::out_of_range("count");
                if (!value) {
                    remaining = 0;
                    FsConsoleLine(output, L"[ZhaoTu] 已取消定时输出，像素采集继续。");
                    continue;
                }
                FsZhuangtai state{}; state.size = sizeof(state);
                if (GetStatus(&state) != FS_CHENGGONG || !state.bound) {
                    FsConsoleLine(output, L"[ZhaoTu] 请先绑定窗口，再设置输出次数。");
                    continue;
                }
                remaining = static_cast<DWORD>(value);
                logBinding = state.bindingVersion;
                nextLog = GetTickCount64() + 1000;
                FsConsoleLine(output, L"[ZhaoTu] 已设置输出次数；每秒一次，输入命令期间暂停。");
            } catch (...) {
                FsConsoleLine(output, L"次数超出范围，允许 0 到 4294967295。");
            }
        } else if (command == L"status") {
            ShowStatus(output, false);
        } else if (command == L"help") {
            FsConsoleLine(output, L"bind HWND dx|obs|wgc normal normal 0：绑定并选择采集方式；切换前先 unbind；gdi 尚未实现。");
            FsConsoleLine(output, L"bind HWND {dx|obs|wgc} {normal|windows|dx} {normal|windows|dx} {0|1}");
            FsConsoleLine(output, L"log 次数：每秒输出一次，共指定次数；新次数替换旧次数，log 0 取消。");
            FsConsoleLine(output, L"unbind 解绑并取消输出；status 查看一次状态；help 帮助。");
            FsConsoleLine(output, L"改参数前先解绑；仅支持本进程窗口；输入命令期间暂停定时输出。");
            FsConsoleLine(output, L"函数调用示例: CapturePng(0, 0, 100, 100, \"C:\\截图.png\", 2)；字符串使用双引号。");
            FsConsoleLine(output, L"SetFindBackend(\"gpu\") / SetFindBackend(\"cpu\") 切换查找；默认 gpu，选择会保留，不重启采集。");
            FsConsoleLine(output, L"SetCaptureFrameInterval(-1) 固定当前帧；0 逐帧采集；N>0 每次采集后跳过 N 个输入帧；解绑恢复 0。");
            FsConsoleLine(output, L"GetColor(10, 10)；FindPic(0, 0, 100, 100, \"test.bmp\", \"000000\", 0.9, 0)。");
            FsConsoleLine(output, L"GetFrameNumber(1) 开启当前绑定编号监视，GetFrameNumber(0) 关闭；约每 10 毫秒检查，编号可能跳号。");
            FsConsoleLine(output, L"查找函数统一返回结构体数组：status=0表示执行正常，count=0表示没找到；坐标在items中，保留分阶段耗时。");
        } else if (!command.empty()) FsConsoleLine(output, L"未知命令，输入 help 查看用法。");
    }
    CloseHandle(input);
    if (output != INVALID_HANDLE_VALUE) CloseHandle(output);
    return 0;
}
