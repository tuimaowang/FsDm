#pragma once
// 文字模块内部公共数据；这些 C++ 对象只在 ZhaoTu.dll 内部使用，不跨 DLL 边界。
#include "ZhaoTu.h"
#include <filesystem>
#include <string>
#include <vector>

std::filesystem::path FsResolvePath(const char* path);
namespace fsdm::wenzi {
struct Image {
    int width{}, height{};
    std::vector<unsigned char> bgra;
};
struct Character {
    std::string text;
    int x{}, y{}, right{}, bottom{};
    float confidence{};
};
struct Line { std::vector<Character> chars; };
using Lines = std::vector<Line>;
struct Selection {
    int model{}, dict{}, backend{}, engine{}; // 后端 0=CPU 1=DirectML；引擎 0=模型 1=字库。
};
std::filesystem::path ModuleDirectory();
std::wstring Wide(const std::string& text);
std::string Utf8(const std::wstring& text);
// 诊断按调用线程保存，设置成功与识别成功都会清除上一条错误。
void Error(int result, const std::string& message);
void Success();
Selection Selected();
// 返回当前调用线程最近一次屏幕 OCR 是否复用了共享帧缓存；文件 OCR 返回 false。
bool OcrFrameReused() noexcept;
// true 表示最近一次屏幕 OCR/找字直接返回了完成的结果，未进行预处理和推理。
bool OcrResultReused() noexcept;
void SortLines(Lines& lines);
}
