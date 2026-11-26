// 本文件把 TestReport.md 中的 Lua 性能案例移植为独立 C++ 测试程序。
// 程序只调用已经交付的 ZhaoTu.dll，不包含或修改 DLL 实现代码。
// 找色、找图需要传入目标窗口句柄；OCR 文件识别直接读取指定 BMP，不需要绑定窗口。
#include "FsDmJiekou.h"

#include <Windows.h>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;

namespace {

struct DmApi {
    HMODULE module = nullptr;
    FsDm_BindWindowFn bindWindow = nullptr;
    FsDm_UnBindWindowFn unbindWindow = nullptr;
    FsDm_SetFindBackendFn setFindBackend = nullptr;
    FsDm_SetPathFn setPath = nullptr;
    FsDm_FindColorExFn findColorEx = nullptr;
    FsDm_FindPicFn findPic = nullptr;
    FsDm_SetOcrModelFn setOcrModel = nullptr;
    FsDm_UseOcrModelFn useOcrModel = nullptr;
    FsDm_SetOcrBackendFn setOcrBackend = nullptr;
    FsDm_SetOcrEngineFn setOcrEngine = nullptr;
    FsDm_OcrInFileFn ocrInFile = nullptr;
    FsDm_FreeResultFn freeResult = nullptr;
};

template <typename T>
T LoadFunction(HMODULE module, const char* name) {
    return reinterpret_cast<T>(GetProcAddress(module, name));
}

std::wstring Utf8ToWide(const std::string& value) {
    if (value.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, nullptr, 0);
    if (size <= 0) return {};
    std::wstring result(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, result.data(), size);
    result.resize(static_cast<size_t>(size - 1));
    return result;
}

std::string WideToUtf8(const std::wstring& value) {
    if (value.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (size <= 0) return {};
    std::string result(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, result.data(), size, nullptr, nullptr);
    result.resize(static_cast<size_t>(size - 1));
    return result;
}

fs::path ExecutableDirectory() {
    std::wstring buffer(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    buffer.resize(length);
    return fs::path(buffer).parent_path();
}

std::string ArgumentValue(int argc, wchar_t** argv, std::wstring_view option,
                          const std::string& fallback = {}) {
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::wstring_view(argv[i]) == option) return WideToUtf8(argv[i + 1]);
    }
    return fallback;
}

bool HasArgument(int argc, wchar_t** argv, std::wstring_view option) {
    for (int i = 1; i < argc; ++i) {
        if (std::wstring_view(argv[i]) == option) return true;
    }
    return false;
}

bool LoadApi(DmApi& api, const fs::path& dllPath) {
    api.module = LoadLibraryW(dllPath.c_str());
    if (!api.module) {
        std::wcerr << L"无法加载 DLL: " << dllPath << L"（错误码 " << GetLastError() << L"）\n";
        return false;
    }

#define LOAD_API(member, type, exportName) api.member = LoadFunction<type>(api.module, exportName)
    LOAD_API(bindWindow, FsDm_BindWindowFn, "BindWindow");
    LOAD_API(unbindWindow, FsDm_UnBindWindowFn, "UnBindWindow");
    LOAD_API(setFindBackend, FsDm_SetFindBackendFn, "SetFindBackend");
    LOAD_API(setPath, FsDm_SetPathFn, "SetPath");
    LOAD_API(findColorEx, FsDm_FindColorExFn, "FindColorEx");
    LOAD_API(findPic, FsDm_FindPicFn, "FindPic");
    LOAD_API(setOcrModel, FsDm_SetOcrModelFn, "SetOcrModel");
    LOAD_API(useOcrModel, FsDm_UseOcrModelFn, "UseOcrModel");
    LOAD_API(setOcrBackend, FsDm_SetOcrBackendFn, "SetOcrBackend");
    LOAD_API(setOcrEngine, FsDm_SetOcrEngineFn, "SetOcrEngine");
    LOAD_API(ocrInFile, FsDm_OcrInFileFn, "OcrInFile");
    LOAD_API(freeResult, FsDm_FreeResultFn, "FreeResult");
#undef LOAD_API

    const bool ready = api.bindWindow && api.unbindWindow && api.setFindBackend && api.setPath &&
        api.findColorEx && api.findPic && api.setOcrModel && api.useOcrModel &&
        api.setOcrBackend && api.setOcrEngine && api.ocrInFile && api.freeResult;
    if (!ready) {
        std::cerr << "当前 ZhaoTu.dll 缺少本测试需要的导出函数。\n";
        return false;
    }
    return true;
}

template <typename Call>
void RunTimedCase(const char* title, int iterations, Call&& call) {
    using Clock = std::chrono::steady_clock;
    const auto start = Clock::now();
    for (int i = 0; i < iterations; ++i) call();
    const auto elapsed = std::chrono::duration<double>(Clock::now() - start).count();
    std::cout << title << ": " << iterations << " 次，"
              << elapsed << " 秒，平均 "
              << (elapsed * 1000.0 / iterations) << " 毫秒/次\n";
}

void ReleaseResult(DmApi& api, const FsTongyongJieguo* result) {
    if (result) api.freeResult(result);
}

void PrintUsage() {
    std::cout
        << "用法：BaogaoFukeCeshi.exe --hwnd <窗口句柄> [选项]\n"
        << "选项：\n"
        << "  --dll <ZhaoTu.dll路径>       默认使用本程序目录中的 ZhaoTu.dll\n"
        << "  --template-dir <目录>        默认使用桌面 report 目录\n"
        << "  --image <BMP路径>            默认使用桌面 report\\test.bmp\n"
        << "  --model-dir <模型目录>       默认使用 FsDm\\third_party\\ocr_models\n"
        << "  --backend cpu|gpu            找色/找图后端，默认 cpu\n"
        << "  --ocr-backend cpu|directml   OCR 后端，默认跟随 --backend\n"
        << "  --display dx|obs|wgc         采集方式，默认 wgc\n"
        << "  --skip-ocr                   跳过 OCR 文件识别案例\n"
        << "  --skip-window                只运行 OCR 文件识别，不绑定目标窗口\n\n"
        << "注意：找图案例需要模板目录中存在 主页.bmp 和 应用.bmp；程序不会自行伪造模板。\n";
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    if (HasArgument(argc, argv, L"--help") || HasArgument(argc, argv, L"-h")) {
        PrintUsage();
        return 0;
    }

    const fs::path exeDir = ExecutableDirectory();
    const fs::path reportDir = fs::path(L"C:\\Users\\Administrator\\Desktop\\report");
    const fs::path defaultModels = exeDir / L"..\\..\\..\\..\\..\\third_party\\ocr_models";
    const fs::path dllPath = Utf8ToWide(ArgumentValue(argc, argv, L"--dll",
        WideToUtf8((exeDir / L"ZhaoTu.dll").wstring())));
    const fs::path templateDir = Utf8ToWide(ArgumentValue(argc, argv, L"--template-dir",
        WideToUtf8(reportDir.wstring())));
    const fs::path imagePath = Utf8ToWide(ArgumentValue(argc, argv, L"--image",
        WideToUtf8((reportDir / L"test.bmp").wstring())));
    const fs::path modelDir = Utf8ToWide(ArgumentValue(argc, argv, L"--model-dir",
        WideToUtf8(fs::weakly_canonical(defaultModels).wstring())));
    const std::string backend = ArgumentValue(argc, argv, L"--backend", "cpu");
    const std::string ocrBackend = ArgumentValue(argc, argv, L"--ocr-backend",
        backend == "gpu" ? "directml" : "cpu");
    const std::string display = ArgumentValue(argc, argv, L"--display", "wgc");
    const std::string hwndText = ArgumentValue(argc, argv, L"--hwnd");
    const bool skipWindow = HasArgument(argc, argv, L"--skip-window");
    const bool skipOcr = HasArgument(argc, argv, L"--skip-ocr");

    DmApi api;
    if (!LoadApi(api, dllPath)) {
        if (api.module) FreeLibrary(api.module);
        return 2;
    }

    bool bound = false;
    int exitCode = 0;
    if (!skipWindow) {
        if (hwndText.empty()) {
            std::cerr << "找色和找图需要目标窗口。请提供 --hwnd，或用 --skip-window 只测 OCR。\n";
            exitCode = 2;
        } else {
            HWND hwnd = reinterpret_cast<HWND>(static_cast<uintptr_t>(
                std::wcstoull(Utf8ToWide(hwndText).c_str(), nullptr, 0)));
            if (!IsWindow(hwnd)) {
                std::cerr << "--hwnd 不是有效窗口句柄。\n";
                exitCode = 2;
            } else if (!api.bindWindow(hwnd, display.c_str(), "normal", "normal", 0)) {
                std::cerr << "绑定窗口失败；请确认窗口、采集方式及当前运行环境。\n";
                exitCode = 3;
            } else {
                bound = true;
                if (!api.setFindBackend(backend.c_str())) {
                    std::cerr << "设置找色/找图后端失败: " << backend << "\n";
                    exitCode = 3;
                } else if (!api.setPath(WideToUtf8(templateDir.wstring()).c_str())) {
                    std::cerr << "设置模板目录失败: " << templateDir << "\n";
                    exitCode = 3;
                } else {
                    // 与报告一致：每轮对同一区域执行两种颜色串的全量找色。
                    RunTimedCase("找色案例（FindColorEx；每轮两次调用）", 100, [&] {
                        auto* result = api.findColorEx(61, 34, 80, 53, "F5F5F5-0A0A0A", 1.0, 0);
                        ReleaseResult(api, result);
                        result = api.findColorEx(61, 34, 80, 53,
                            "37843D-27242F|63A336-1A1D1A", 1.0, 0);
                        ReleaseResult(api, result);
                    });

                    const bool hasHome = fs::exists(templateDir / L"主页.bmp");
                    const bool hasApps = fs::exists(templateDir / L"应用.bmp");
                    if (hasHome && hasApps) {
                        // 报告中循环 5000 次、每轮查找两张模板，故总计 10000 次查图。
                        RunTimedCase("找图案例（FindPic；每轮两次调用）", 5000, [&] {
                            auto* result = api.findPic(97, 108, 130, 129,
                                "主页.bmp", "000000", 1.0, 0);
                            ReleaseResult(api, result);
                            result = api.findPic(162, 162, 195, 183,
                                "应用.bmp", "000000", 1.0, 0);
                            ReleaseResult(api, result);
                        });
                    } else {
                        std::cout << "跳过找图：模板目录缺少 "
                                  << (!hasHome ? "主页.bmp " : "")
                                  << (!hasApps ? "应用.bmp" : "") << "。\n";
                    }
                }
            }
        }
    }

    if (!skipOcr && exitCode == 0) {
        if (!fs::exists(imagePath)) {
            std::cerr << "OCR 输入图片不存在: " << imagePath << "\n";
            exitCode = 4;
        } else if (!fs::exists(modelDir / L"PP-OCRv5_mobile_det" / L"model.onnx") ||
                   !fs::exists(modelDir / L"PP-OCRv5_mobile_rec" / L"model.onnx")) {
            std::cerr << "OCR 模型目录无效，需包含 PP-OCRv5_mobile_det/model.onnx 与 "
                         "PP-OCRv5_mobile_rec/model.onnx: " << modelDir << "\n";
            exitCode = 4;
        } else if (!api.setOcrEngine("model") || !api.setOcrBackend(ocrBackend.c_str()) ||
                   !api.setOcrModel(0, WideToUtf8(modelDir.wstring()).c_str()) ||
                   !api.useOcrModel(0)) {
            std::cerr << "OCR 模型或后端初始化失败。\n";
            exitCode = 4;
        } else {
            const std::string imageUtf8 = WideToUtf8(imagePath.wstring());
            // 使用当前 DLL 的文件 OCR 入口对应报告中的“截取区域后识别”流程；
            // 输入报告提供的同一张画面，并对两块原坐标区域各识别 100 次。
            RunTimedCase("OCR 案例区域一（OcrInFile；100 次）", 100, [&] {
                auto* result = api.ocrInFile(96, 108, 131, 129,
                    imageUtf8.c_str(), "", 0.3);
                ReleaseResult(api, result);
            });
            RunTimedCase("OCR 案例区域二（OcrInFile；100 次）", 100, [&] {
                auto* result = api.ocrInFile(96, 343, 198, 366,
                    imageUtf8.c_str(), "", 0.3);
                ReleaseResult(api, result);
            });
        }
    }

    if (bound) api.unbindWindow();
    FreeLibrary(api.module);
    return exitCode;
}
