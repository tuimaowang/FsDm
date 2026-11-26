#pragma once
#include "WenziShibie.h"
namespace fsdm::wenzi {
bool SetModel(int index, const std::filesystem::path& path);
bool SetModelFiles(int index, const std::filesystem::path& classifier,
    const std::filesystem::path& detector, const std::filesystem::path& recognizer,
    const std::filesystem::path& dictionary);
bool UseModel(int index);
bool FreeModel(int index);
bool RecognizeModel(const Image& image, int index, int backend, double sim, Lines& lines);
// 返回累计创建次数，便于验证切换采集/重复识别没有重新创建设备或会话。
void ModelStatistics(uint64_t& sessions, uint64_t& devices, uint32_t& loaded);
}
