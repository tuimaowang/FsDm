#pragma once
#include "../Gonggong/Jiekou.h"
#ifdef ZHAOTU_SHENGCHENG
#define ZH_API __declspec(dllexport)
#else
#define ZH_API __declspec(dllimport)
#endif
#ifdef __cplusplus
extern "C" {
#endif
// display 直接选择 dx/obs/wgc；相同配置重复绑定成功，更换方式或参数必须先解绑。
// 当前未实现 gdi/gdi2/normal/dx2 取图，传入这些名称返回 0，GetStatus 报不支持。
ZH_API int WINAPI BindWindow(HWND hwnd, const char* display, const char* mouse, const char* keypad, int mode);
ZH_API int WINAPI UnBindWindow(void);
// interval=-1 固定当前已发布快照并暂停发布，直到设置非负值或解绑。
// interval>=0 表示每次采集后跳过的输入帧数：0 逐帧采集，1 跳过 1 帧，2 跳过 2 帧，以此类推。
// -1 需要已有快照；小于 -1、未绑定或尚无快照时返回 0。每次新绑定默认 0。
ZH_API int WINAPI SetCaptureFrameInterval(int interval);
// bypass=1 时强制每次重新执行找图/OCR/YOLO 分析；bypass=0 时允许复用同帧同条件的分析结果。默认允许复用。
// 这是 ZhaoTu.dll 内跨线程共享的设置，涵盖找图找色、CvFindPic、窗口 OCR/找字及 YOLO。
// 帧号相同还需绑定版本、区域、参数及模板/模型配置一致；缓存淘汰后重新计算，文件 OCR 每次读取文件。
ZH_API int WINAPI SetFrameResultCache(int bypass);
ZH_API int WINAPI CopyFrame(void* pixels, uint64_t capacity, FsZhen* frame);
ZH_API int WINAPI GetStatus(FsZhuangtai* status);
// output 为 1 时开启当前绑定的编号监视，为 0 时关闭；同时返回最新编号，无快照时返回 0。
// 开启时输出已有编号，后续约每 10 毫秒检查一次，只在编号变化时输出十进制编号。
// 重复开启复用同一任务，解绑或绑定版本变化后停止；工作台关闭配置下仅查询编号。
// 编号代表成功发布的快照，像素相同也可能增加；高帧率下监视输出可能跳号。
// output 只接受 0 和 1，其他值返回 0，并将线程错误设置为 ERROR_INVALID_PARAMETER。
ZH_API uint64_t WINAPI GetFrameNumber(int output);
// 调试占用率：无需绑定窗口，返回整机 CPU 占用百分比 0～100；每次同步采样约 200ms。
// 包含全部逻辑处理器，按忙碌时间占比统计；失败返回 -1，GetLastError 获取系统/PDH 错误。
ZH_API double WINAPI GetCpuUsage(void);
// 返回所有显卡中最繁忙引擎的占用百分比 0～100；同一引擎先汇总所有进程。
// 每次同步采样约 200ms，含初始化开销；驱动/计数器不可用或采样无效返回 -1。
// 查询独立于绑定、OCR/找图后端和帧缓存；GetLastError 获取系统/PDH 错误。
ZH_API double WINAPI GetGpuUsage(void);
ZH_API int WINAPI AcquireGpuFrame(FsGpuZhen* frame);
ZH_API int WINAPI ReleaseGpuFrame(uint64_t token);
ZH_API int WINAPI GetCaptureStatus(FsCaijiZhuangtai* status);
ZH_API int WINAPI MapPoint(double x, double y, double* outX, double* outY, int direction);
// 路径采用 UTF-8；区域为客户区物理像素，右下角不包含在内。
ZH_API int WINAPI SetPath(const char* path);
// 设置 CPU 与每个 GPU 设备模板缓存各自的字节预算；默认 256 MiB，必须大于 0。
// 若预算小于当前显式预加载模板占用则返回 0，配置保持不变；正在查找的资源在查找结束后回收。
ZH_API int WINAPI SetTemplateCacheLimit(uint64_t bytes);
// 模板预加载无需绑定窗口，返回 1 成功、0 失败。名称采用 UTF-8，支持 | 列表与文件名中的 *、?。
// 显式加载的 CPU 模板保留到 FreePic；模板数量按需增长，并受 SetTemplateCacheLimit 字节预算约束。
// 多项加载中若某项失败，返回 0，已经成功加载的项仍保留。重复加载已显式缓存的名称直接成功。
ZH_API int WINAPI LoadPic(const char* pic_name);
// 按名称释放 CPU 与对应 GPU 模板，支持列表和通配符；有效名称没有缓存也返回 1。
// 已经开始的查找继续使用旧模板，结束后释放；普通文件下次找图时仍会自动重新加载。
ZH_API int WINAPI FreePic(const char* pic_name);
// addr 是本进程内完整 BMP24 文件数据的首地址；size 为文件字节数，不能超过当前模板缓存预算。
// 成功后已复制并解码，原始内存可立即释放；pic_name 是单个虚拟文件名，不要求磁盘文件存在。
// 同名调用替换旧模板；地址使用 uintptr_t，避免 x64 地址被截断，大小无效或内存不可读返回 0。
ZH_API int WINAPI LoadPicByte(uintptr_t addr, int size, const char* pic_name);
// mode=1 仅登记为模板，2 仅保存文件，3 同时执行；name 同时用作模板别名和文件路径。
// C++ 调用省略 mode 时默认为 2；C 和动态调用必须明确传入六个参数。
#ifdef __cplusplus
#define ZH_CAPTURE_MOREN = 2
#else
#define ZH_CAPTURE_MOREN
#endif
// 模式 1 不要求别名有扩展名或对应文件存在；模板保留到 FreePic 或进程结束。
ZH_API int WINAPI Capture(int x1, int y1, int x2, int y2, const char* name, int mode ZH_CAPTURE_MOREN);
ZH_API int WINAPI CaptureJpg(int x1, int y1, int x2, int y2, const char* name, int mode ZH_CAPTURE_MOREN);
ZH_API int WINAPI CapturePng(int x1, int y1, int x2, int y2, const char* name, int mode ZH_CAPTURE_MOREN);
#undef ZH_CAPTURE_MOREN
ZH_API int WINAPI CaptureGif(int x1, int y1, int x2, int y2, const char* file, int delay, int time);
// GetColor、CmpColor、FindColor/Ex、FindMultiColor/Ex、FindPic/S/Ex/ExS 及四个 CvFindPic 入口
// 自动比较绑定版本和帧编号。同帧且全部参数、模板版本一致时复用成功结果，含正常无命中。
// 同帧换条件会重新查找，CPU 路径复用跨模块共享的已读回像素；新帧使用新快照重新查找。
// 普通文件模板沿用文件大小和修改时间检查；显式加载模板以 LoadPicByte、Capture、FreePic 等更新为准。
// 结果缓存跨线程共享，最多 64 项、16 MiB，超限淘汰较少使用项；每次返回独立结果，按原方式释放。
// 找图找色默认 gpu；与 cpu 使用相同 TSPlug 规则，选择只更新设置，不重启设备或采集。
ZH_API int WINAPI SetFindBackend(const char* backend);
// 查询本线程的最近查找诊断和共享资源累计计数。返回 FsJieguo。
ZH_API int WINAPI GetFindStatus(FsChazhaoZhuangtai* status);
// 找图、找色、OCR、找字统一返回独立结构体，直接读取 count/items，无须拆分坐标字符串。
// status=0/count=0为正常无命中；错误用 FsJieguo 状态码；nullptr仅表示分配失败。
// 结果及内部指针不随后续调用或线程退出失效；禁止自行delete/free。释放与读取不能并发。
// 精确释放本次结果；nullptr成功，未知地址返回0，不解引用地址。
ZH_API int WINAPI FreeResult(const FsTongyongJieguo* result);
// 释放全部通用结果和文字数组；不释放模板、模型、设备、GPU租约或原始像素。
ZH_API int WINAPI FreeAllResults(void);
// 保留的旧名称为 FreeAllResults 的兼容别名，同样处理通用结果和文字数组。
// 释放所有函数、所有线程尚未释放的通用结果及文字数组；重复调用成功。1 成功，0 失败。
ZH_API int WINAPI FreeAllStringResults(void);
// GetColor返回一个XY/COLOR条目，RGB为整数；负坐标归零，正向越界为参数错误。
ZH_API const FsTongyongJieguo* WINAPI GetColor(int x, int y);
// 比色最多接受 10 组 RRGGBB[-DRDGDB]，以 | 分隔；sim 沿用找色容差。
// 匹配时count=1，包含颜色列表索引、坐标和实际RGB；不匹配时status=0/count=0。
ZH_API const FsTongyongJieguo* WINAPI CmpColor(int x, int y, const char* color, double sim);
// 获取客户区区域的 CPU 数据，右下角不包含；要求整个区域在客户区内。
// 无文件头、从上到下紧密排列，每个客户区像素为 B,G,R,0（整数 00RRGGBB）。
// 地址属于本线程，至同线程下次 GetScreenData（包括失败调用）或线程退出有效。
// 失败返回 0；x64 必须使用 uintptr_t 接收，禁止由调用方释放，不能直接传给 LoadPicByte。
ZH_API uintptr_t WINAPI GetScreenData(int x1, int y1, int x2, int y2);
// 持续 t 秒比较区域实际像素：变化立即返回 0，不变返回 1。
// t=0 只验证并读取一次有效区域；非法参数、解绑/重绑、窗口销毁或取帧失败返回 0。
// 每约 50ms 检查一次，不能保证捕获两次检查之间短暂出现又消失的变化。
// 使用 CPU 按需读回，不修改找图后端；右下角不包含，错误查询 GetFindStatus。
ZH_API int WINAPI IsDisplayDead(int x1, int y1, int x2, int y2, int t);
// 按 SetPath 枚举存在的 BMP 文件，支持文件名中的 *、? 和 | 列表，无需绑定。
// 结果用 | 分隔，保留输入的相对目录/绝对路径；每组排序并去重；失败/无匹配返回 ""。
ZH_API const char* WINAPI MatchPicName(const char* pic_name);
// 找色返回0/1个ID/XY条目，id为颜色列表索引。
// color 支持 RRGGBB-DRDGDB|...；dir 为 0～8，sim 为 [0.1,1.0]。
// 与 TSPlug 一致：sim 为 .5/.6/.7/.8/.9 时额外通道容差为 49/38/29/18/9，其余为 0。
// 多颜色按列表优先；右下角包含，超出客户区时裁剪；dir=4 为上、右、下、左螺旋。
ZH_API const FsTongyongJieguo* WINAPI FindColor(int x1, int y1, int x2, int y2, const char* color, double sim, int dir);
// 返回颜色条目数组，按颜色列表优先。
// 支持方向0～3、5～8；不设固定条数上限，返回搜索区域内各颜色的全部命中。
// 结果量取决于命中点数量；不同颜色均匹配的同一坐标会按颜色索引分别返回。
ZH_API const FsTongyongJieguo* WINAPI FindColorEx(int x1, int y1, int x2, int y2, const char* color, double sim, int dir);
// 多点找色：首色只接受一个 RRGGBB[-DRDGDB]；所有偏移点必须同时满足。
// offset_color 为 dx|dy|[-]RRGGBB[-DRDGDB],...；颜色前的负号表示不在该容差范围内。
// 偏移按客户区物理像素计算，所有点须在裁剪后的搜索区域内；dir 为 0～3。
// 单次返回0/1个首点条目；相似度和右下角规则同FindColor。
ZH_API const FsTongyongJieguo* WINAPI FindMultiColor(int x1, int y1, int x2, int y2, const char* first_color, const char* offset_color, double sim, int dir);
// 返回首点数组，最多1500项；每项表示整组偏移色匹配的首点。
ZH_API const FsTongyongJieguo* WINAPI FindMultiColorEx(int x1, int y1, int x2, int y2, const char* first_color, const char* offset_color, double sim, int dir);
// 找图只接受 BMP24；四角同色为透明色，全图同色则按不透明图比较。
// 模板保持原图像素尺寸，区域和结果为客户区坐标；dir 为 0～3。
// sim 采用同上离散表，叠加 delta_color；所有有效像素均须满足通道容差。
// 按模板列表优先，无法加载的项跳过且保留原索引；区域右下角不包含。
ZH_API const FsTongyongJieguo* WINAPI FindPic(int x1, int y1, int x2, int y2, const char* pic_name, const char* delta_color, double sim, int dir);
// S与普通版均返回0/1个ID/XY/FILE条目，保留函数名方便迁移。
ZH_API const FsTongyongJieguo* WINAPI FindPicS(int x1, int y1, int x2, int y2, const char* pic_name, const char* delta_color, double sim, int dir);
// Ex/ExS均返回ID/XY/FILE数组，最多1500项。
ZH_API const FsTongyongJieguo* WINAPI FindPicEx(int x1, int y1, int x2, int y2, const char* pic_name, const char* delta_color, double sim, int dir);
ZH_API const FsTongyongJieguo* WINAPI FindPicExS(int x1, int y1, int x2, int y2, const char* pic_name, const char* delta_color, double sim, int dir);
// OpenCV 模板匹配使用 CPU 读取截图，sim 为 0.1～1.0，按归一化像素平方误差计算。
// 文件名支持 | 列表和已登记模板别名，dir 为 0～3，区域右下角不包含。
// Mem 版本使用 LoadPicByte 登记后的别名，结果均由 FreeResult 释放。
ZH_API const FsTongyongJieguo* WINAPI CvFindPic(int x1, int y1, int x2, int y2, const char* pic_name, double sim, int dir);
ZH_API const FsTongyongJieguo* WINAPI CvFindPicEx(int x1, int y1, int x2, int y2, const char* pic_name, double sim, int dir);
ZH_API const FsTongyongJieguo* WINAPI CvFindPicMem(int x1, int y1, int x2, int y2, const char* pic_name, double sim, int dir);
ZH_API const FsTongyongJieguo* WINAPI CvFindPicMemEx(int x1, int y1, int x2, int y2, const char* pic_name, double sim, int dir);
// AiFindPic 是大漠接口名兼容层，不加载缺失的 ai.module；实际使用本项目 OpenCV 模板匹配。
// 返回值遵循 FsTongyongJieguo，而非原插件的单个序号/坐标输出参数。
ZH_API const FsTongyongJieguo* WINAPI AiFindPic(int x1, int y1, int x2, int y2, const char* pic_name, double sim, int dir);
ZH_API const FsTongyongJieguo* WINAPI AiFindPicEx(int x1, int y1, int x2, int y2, const char* pic_name, double sim, int dir);
// pic_info 格式为十进制地址,字节长度|地址,长度；地址须指向完整 BMP24 数据。
ZH_API const FsTongyongJieguo* WINAPI AiFindPicMem(int x1, int y1, int x2, int y2, const char* pic_info, double sim, int dir);
ZH_API const FsTongyongJieguo* WINAPI AiFindPicMemEx(int x1, int y1, int x2, int y2, const char* pic_info, double sim, int dir);
// YOLO 首版支持 v5-7.0 ONNX；模型同目录需有同名 .class，每行一个 UTF-8 类别名。
ZH_API int WINAPI AiYoloSetVersion(const char* version);
ZH_API int WINAPI AiYoloSetModel(int index, const char* file, const char* password);
// 仅接受未加密 ONNX 字节和空密码；.dmx 需要专有解密模块，当前不支持。
ZH_API int WINAPI AiYoloSetModelMemory(int index, uintptr_t data, int size, const char* password);
ZH_API int WINAPI AiYoloUseModel(int index);
ZH_API int WINAPI AiYoloFreeModel(int index);
ZH_API const char* WINAPI AiYoloDetectObjects(int x1, int y1, int x2, int y2, double prob, double iou);
ZH_API int WINAPI AiYoloDetectObjectsToFile(int x1, int y1, int x2, int y2, double prob, double iou, const char* file, int mode);
// data 指向调用方 BMP 缓冲区；*size 调用前为容量，调用后为实际大小。缓冲区不足返回 0 并写入所需大小。
ZH_API int WINAPI AiYoloDetectObjectsToDataBmp(int x1, int y1, int x2, int y2, double prob, double iou,
    void* data, uint32_t* size, int mode);
ZH_API const char* WINAPI AiYoloSortsObjects(const char* objects, int height);
ZH_API const char* WINAPI AiYoloObjectsToString(const char* objects);
// OCR 默认使用 model 引擎、CPU 后端和 0 号 PP-OCRv5 mobile 模型。
// SetOcrModel 只登记并核对资源，实际会话在首次识别时初始化；相同目录不重复加载。
// 除默认 PP-OCRv5 目录外，也支持 PP-OCRv3 中文 models/models10 文件布局。
ZH_API int WINAPI SetOcrModel(int index, const char* model_dir);
// 按文件路径登记 PP-OCRv3 兼容模型；classifier_path 可为 NULL 或空字符串表示不使用方向分类模型。
// 输入尺寸、输出类别及字典数量在首次 OCR 创建会话时校验。
ZH_API int WINAPI SetOcrModelFiles(int index, const char* classifier_path, const char* detector_path,
    const char* recognizer_path, const char* dictionary_path);
ZH_API int WINAPI UseOcrModel(int index);
ZH_API int WINAPI FreeOcrModel(int index);
ZH_API int WINAPI SetOcrBackend(const char* backend);
ZH_API int WINAPI SetOcrEngine(const char* engine);
// 字库采用 TSPlug 未加密文本点阵格式，索引 0～99；SetDict 成功后同时选中该字库。
// 支持 UTF-8 与 GB18030 文本；路径相对于 SetPath，ClearDict 不删除磁盘文件。
ZH_API int WINAPI SetDict(int index, const char* file);
// 从当前进程内存加载完整 TSPlug 未加密文本点阵字库；数据会复制，成功后同时选中该字库。
// address 为字节首地址，size 为字节数，范围 1 字节～16 MiB；文本编码支持 UTF-8 与 GB18030。
ZH_API int WINAPI SetDictMemory(int index, uintptr_t address, int size);
ZH_API int WINAPI UseDict(int index);
ZH_API int WINAPI GetNowDict(void);
ZH_API int WINAPI GetDictCount(int index);
ZH_API int WINAPI ClearDict(int index);
ZH_API int WINAPI GetOcrStatus(FsOcrZhuangtai* status);
// 便于工作台查看本线程诊断；返回独立 UTF-8 数组，释放规则同其他字符串接口。
ZH_API const char* WINAPI GetOcrError(void);
// 窗口区域采用客户区物理像素，右下角不包含；需先绑定窗口。文件 OCR 无需绑定。
// model 的 sim 是识别置信度阈值，dict 的 sim 是字形匹配阈值；均为 [0.1,1.0]。
// color_format 支持 RGB/HSV/灰度、多色及 b@ 背景；空字符串保留原图自动识别。
// 四个OCR入口均返回fullText全文及逐字符TEXT/XY条目；支持颜色串后追加,分行符。
// 模型字符位置由 CTC 时间步估计，不能作为像素精确的字形框。
ZH_API const FsTongyongJieguo* WINAPI Ocr(int x1, int y1, int x2, int y2, const char* color_format, double sim);
ZH_API const FsTongyongJieguo* WINAPI OcrEx(int x1, int y1, int x2, int y2, const char* color_format, double sim);
ZH_API const FsTongyongJieguo* WINAPI OcrExOne(int x1, int y1, int x2, int y2, const char* color_format, double sim);
ZH_API const FsTongyongJieguo* WINAPI OcrInFile(int x1, int y1, int x2, int y2, const char* pic_name, const char* color_format, double sim);
// FindStr/FindStrS返回0/1个ID/XY/TEXT条目。
// Ex/ExS返回所有匹配，最多1500项，不跨行拼接匹配。
ZH_API const FsTongyongJieguo* WINAPI FindStr(int x1, int y1, int x2, int y2, const char* text, const char* color_format, double sim);
ZH_API const FsTongyongJieguo* WINAPI FindStrS(int x1, int y1, int x2, int y2, const char* text, const char* color_format, double sim);
ZH_API const FsTongyongJieguo* WINAPI FindStrEx(int x1, int y1, int x2, int y2, const char* text, const char* color_format, double sim);
ZH_API const FsTongyongJieguo* WINAPI FindStrExS(int x1, int y1, int x2, int y2, const char* text, const char* color_format, double sim);
// 快速版只让目标字符串涉及的字形参与匹配；可能忽略中间的其他字符，不跨文字行。
// dict 引擎先筛选字形，精确匹配再按首列点阵定位候选；模糊匹配超过允许误差即停止。
// model 引擎复用原模型识别，再筛选目标字符；此方式不会减少模型本身的推理工作。
// Fast/FastS返回0/1项；FastEx/FastExS返回所有匹配，布局与普通版一致。
ZH_API const FsTongyongJieguo* WINAPI FindStrFast(int x1, int y1, int x2, int y2, const char* text, const char* color_format, double sim);
ZH_API const FsTongyongJieguo* WINAPI FindStrFastS(int x1, int y1, int x2, int y2, const char* text, const char* color_format, double sim);
ZH_API const FsTongyongJieguo* WINAPI FindStrFastEx(int x1, int y1, int x2, int y2, const char* text, const char* color_format, double sim);
ZH_API const FsTongyongJieguo* WINAPI FindStrFastExS(int x1, int y1, int x2, int y2, const char* text, const char* color_format, double sim);
#ifdef __cplusplus
}
#endif
