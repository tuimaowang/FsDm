#pragma once
#include "../Gonggong/Jiekou.h"
#ifdef ZHUATU_SHENGCHENG
#define ZT_API __declspec(dllexport)
#else
#define ZT_API __declspec(dllimport)
#endif
#ifdef __cplusplus
extern "C" {
#endif
// 与 BindWindow 同时接收采集方式；底层返回 FsJieguo，0 为成功。
ZT_API int WINAPI ZhuaTuBegin(HWND hwnd, const char* display, const char* mouse, const char* keypad, int mode);
ZT_API int WINAPI ZhuaTuEnd(void);
// interval=-1 固定现有快照；interval>=0 表示每次采集后跳过的输入帧数，0 为逐帧采集。
ZT_API int WINAPI ZhuaTuSetCaptureFrameInterval(int interval);
ZT_API int WINAPI ZhuaTuCopyFrame(void* pixels, uint64_t capacity, FsZhen* frame);
ZT_API int WINAPI ZhuaTuGetStatus(FsZhuangtai* status);
ZT_API int WINAPI ZhuaTuAcquireGpuFrame(FsGpuZhen* frame);
ZT_API int WINAPI ZhuaTuReleaseGpuFrame(uint64_t token);
ZT_API int WINAPI ZhuaTuGetCaptureStatus(FsCaijiZhuangtai* status);
// 内部设备租约：GPU 查找和 CPU 读回复用同一处理设备与互斥锁。
ZT_API HRESULT WINAPI ZhuaTuAcquireProcessor(uint32_t low, int32_t high, FsGpuChuli* processor);
ZT_API HRESULT WINAPI ZhuaTuReleaseProcessor(uint64_t token);
// direction=0 客户区到原图，direction=1 原图到客户区；允许小数，越界返回参数错误。
ZT_API int WINAPI ZhuaTuMapPoint(double x, double y, double* outX, double* outY, int direction);
#ifdef __cplusplus
}
#endif
