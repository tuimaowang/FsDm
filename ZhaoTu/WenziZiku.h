#pragma once
#include "WenziShibie.h"
#include <memory>
namespace fsdm::wenzi {
struct Dictionary;
bool SetDictionary(int index, const std::filesystem::path& path);
bool SetDictionaryMemory(int index, uintptr_t address, int size);
bool HasDictionary(int index);
int DictionaryCount(int index);
bool ClearDictionary(int index);
// targets 非空表示快速模式，只匹配完整出现在某个目标串内的字形；空指针保留普通识别。
bool RecognizeDictionary(const Image& image, int index, double sim, Lines& lines,
                         const std::vector<std::string>* targets = nullptr);
}
