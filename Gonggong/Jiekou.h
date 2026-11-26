#pragma once
// 公共 C 接口：只传固定布局结构和调用方缓冲区，不跨 DLL 传递 C++ 对象。
#include <windows.h>
#include <stdint.h>

#define FSDM_BANBEN 1u                 // 当前采集接口结构版本；调用方通过 size 和 version 核对布局。
#define FSDM_ZUIDA_ZIJIE (67108864ull) // 单帧 BGRA 容量上限：64 MiB，即 16,777,216 个像素。
// 执行状态码：用于采集接口返回值，以及识别结果的 status、诊断结构的 result。
// 这里的 0 表示执行成功，与 BindWindow、SetPath 等接口的“1 成功、0 失败”不同。
// 找图找色执行成功也可能没有命中：必须同时检查 status == FS_CHENGGONG 和 count。
// 非零状态表示执行异常，不应把它当作“正常执行但没有找到”；detail 提供底层错误。
enum FsJieguo {
    FS_CHENGGONG = 0,             // 操作正常完成；是否找到结果还要看统一结果的 count。
    FS_WEIBANGDING = 1,           // 尚未绑定，或读取期间已经解绑、绑定失效。
    FS_DENGDAISHOUZHEN = 2,       // 已绑定但还没有可用画面，目标呈现后可再次读取。
    FS_HUANCHONG_BUZU = 3,        // 调用方像素缓冲区不足；按 frame.requiredBytes 准备后重试。
    FS_CANSHU_CUOWU = 4,          // 参数无效，例如空指针、结构大小错误、非法坐标或凭据。
    FS_BUZHICHI = 5,             // 当前不支持所选模式、图像格式或采集能力。
    FS_YIBANGDING = 6,            // 已有不同配置的绑定；切换窗口或参数前需要先解绑。
    FS_CHUSHIHUA_SHIBAI = 7,      // 拦截、设备或后端初始化失败；结合 detail 查看具体原因。
    FS_CAIJI_SHIBAI = 8,          // 画面采集、纹理复制或像素读回失败。
    FS_JIAZAI_SHIBAI = 9,         // 模板、模型、字库或其他依赖加载失败，并非正常查找无命中。
    FS_CHUSHIHUA_CHAOSHI = 10,    // 初始化等待超时，未能在规定时间内完成准备。
    FS_WEIXUAN_MOSHI = 11,        // 保留状态：尚未选择采集方式；当前由 BindWindow 的 display 选择。
    FS_ZIYUAN_MANG = 12,          // 所需资源或容量暂不可用，例如使用凭据耗尽、缓存或候选过多。
    FS_GPU_CHAOSHI = 13,          // 等待 GPU 工作完成超时，不能继续使用本次未完成的结果。
    FS_OBS_CHONGTU = 14,          // 检测到已有其他 OBS 采集会话，本模块不抢占该会话。
    FS_CHUANGKOU_BUPIPEI = 15     // OBS 发布画面的窗口身份与绑定窗口不一致，拒绝使用该画面。
};
// 采集后端编号：描述画面从哪里取得，不是找图后端，也不是 DirectX 的版本。
// 与 FsZhen.backend 不同，后者表示底层图形接口版本 10、11、12。
enum FsCaijiFangshi {
    FS_CAIJI_WEIXUAN = 0, // 未选择或未知；不代表可直接使用的采集方式。
    FS_CAIJI_DX = 1,      // 本模块的 DirectX 呈现拦截采集，对应 display="dx"。
    FS_CAIJI_OBS = 2,     // OBS graphics-hook 共享纹理采集，对应 display="obs"。
    FS_CAIJI_WGC = 3      // Windows Graphics Capture 窗口采集，对应 display="wgc"。
};
// 对外结构统一采用最多 8 字节对齐，保证 Windows x64 调用方与 DLL 的布局一致。
#pragma pack(push, 8)
// 通用识别结果的字段有效标志；没有置位的字段不能作为识别结果使用。
// 例如黑色 color=0 和坐标 x=0 都是正常值，必须通过 fields 判断是否存在。
// 标志可以组合：fields & FS_JG_XY 非零表示 x、y 有效，不要用 fields == FS_JG_XY 判断。
enum FsJieguoZiduan {
    FS_JG_ID = 1u,    // 二进制第 0 位：id 有效，通常是输入颜色、模板或待查文字的列表索引。
    FS_JG_XY = 2u,    // 二进制第 1 位：x、y 有效，单位是窗口客户区的物理像素。
    FS_JG_COLOR = 4u, // 二进制第 2 位：color 有效，为 0xRRGGBB 的 RGB 整数。
    FS_JG_TEXT = 8u,  // 二进制第 3 位：text 有效，内容是 UTF-8 文字。
    FS_JG_FILE = 16u  // 二进制第 4 位：file 有效，内容是命中的模板名称。
};
// 一个识别条目；同一次查找可以返回多个条目，通过父结果的 items[i] 读取。
// 本结构及 text、file 均由 DLL 持有，不能单独释放；随所属统一结果一起释放。
typedef struct FsJieguoXiang {
    uint32_t fields;          // FsJieguoZiduan 标志组合，决定本条目哪些字段有实际含义。
    int32_t id;              // 从 0 开始的列表索引；仅当 FS_JG_ID 置位时使用。
    int32_t x, y;             // 客户区物理像素；找图为模板左上角，多点找色为整组匹配的首点。
    uint32_t color;           // RGB 数值，例如 0x12f3f3，无须解析颜色字符串。
    const char* text;         // UTF-8 文字，和整个结果共同存活。
    const char* file;         // 命中的模板名称，保留传入名称。
} FsJieguoXiang;
typedef struct FsTongyongJieguo {
    uint32_t size, version;   // DLL 填写：size 为结构字节数，version 为布局版本，当前为 1。
    int32_t status, detail;   // status 取 FsJieguo；detail 为底层错误信息，0 表示未附加错误信息。
    uint32_t count;           // items 中有效条目数；status=0/count=0 表示正常执行但没有命中。
    const FsJieguoXiang* items; // 只读条目数组，只能访问 [0,count)；count=0 时可以是空指针。
    const char* fullText;     // OCR 全文；其他操作为空字符串。
} FsTongyongJieguo;
// 统一结果是一次调用的独立快照，不因后续调用、解绑或原调用线程退出而失效。
// 用 FreeResult 按结构体指针释放，或用 FreeAllResults 全量释放；释放后内部指针全部失效。
// 读取与释放同一结果不能并发；nullptr 返回值表示结果内存分配失败。

// 一帧画面的说明；本结构不直接保存像素指针，CPU 像素由 CopyFrame 写入调用方缓冲区。
typedef struct FsZhen {
    uint32_t size;             // 调用前设置 sizeof(FsZhen)，返回本版本完整结构。
    uint32_t version;          // DLL 返回的结构布局版本，当前为 FSDM_BANBEN。
    // width/height：原始采集画面的宽高，单位为像素，不保证等于客户区尺寸。
    // stride：CPU BGRA 每行字节数，为 width*4；不是 GPU 纹理映射时的实际行跨度。
    // backend：底层图形接口版本，DX/OBS 为 10、11、12；WGC 的 D3D11 纹理为 11。
    uint32_t width, height, stride, backend;
    uint32_t clientWidth, clientHeight;     // 窗口客户区物理像素宽高，用于与原始画面坐标换算。
    // requiredBytes：完整 CPU BGRA 帧所需字节数，为 width*height*4。
    // frameNumber：本次绑定内成功发布的快照编号，不是 OBS 源呈现帧编号。
    // bindingVersion：绑定版本；跨解绑、重绑比较帧时必须同时核对这个值。
    uint64_t requiredBytes, frameNumber, bindingVersion;
    // captureQpc：快照提交时 QueryPerformanceCounter 计数；qpcFrequency：每秒计数。
    // 同一时钟下的计数差除以 qpcFrequency 得到秒数，不是日期时间或 GPU 耗时。
    uint64_t captureQpc, qpcFrequency;       // 快照命令提交时的单调时钟，非 GPU 完成时刻。
    double readbackMs;                     // CopyFrame 返回本次读回耗时；GetStatus 中为 0。
    double captureMs;                      // CPU 提交 GPU 快照的耗时，不是 GPU 执行时间。
} FsZhen;
// 绑定和最新画面状态；GetStatus 返回值表示“查询是否成功”，result 表示采集当前状态。
// 调用前置零并设置 size=sizeof(FsZhuangtai)，不要只用查询函数返回值判断是否已经有画面。
typedef struct FsZhuangtai {
    uint32_t size, version;                // 结构字节数与布局版本；size 由调用方预先填写。
    int32_t result, detail;                 // result 取 FsJieguo；detail 为原始 HRESULT / Win32 错误。
    // bound：是否有有效绑定；hasFrame：是否缓存画面，是否可读取仍需检查 result。
    // initializing：底层拦截是否仍在初始化；inputImplemented：键鼠操作是否已实现，当前为 0。
    uint32_t bound, hasFrame, initializing, inputImplemented;
    // hwnd：绑定窗口句柄的完整 x64 数值；bindingVersion：当前绑定版本。
    // capturedFrames/failedFrames：本次绑定成功发布的快照数、采集失败数，解绑时清零。
    uint64_t hwnd, bindingVersion, capturedFrames, failedFrames;
    // ownerThreadId：发起绑定的线程，退出后自动解绑；mode：绑定时保存的 0 或 1。
    uint32_t ownerThreadId, mode;
    // display：dx/obs/wgc；mouse/keypad：绑定时的配置，目前只保存，不执行键鼠操作。
    // 三个数组均为以零结尾的模式名称，容量包含结尾零字符。
    char display[16], mouse[16], keypad[16];
    FsZhen frame;                          // 最新成功发布帧的信息，hasFrame=0 时不可视为有效画面。
} FsZhuangtai;
// GPU 租约：sharedHandle 是旧式 DXGI 共享句柄，禁止 CloseHandle。
// 用同一适配器的 D3D11 设备 OpenSharedResource；只读使用，GPU 完成后释放 token。
// textureFormat 是实际共享纹理的 DXGI_FORMAT；flip 指示源画面是否上下颠倒。
typedef struct FsGpuZhen {
    uint32_t size, version;                // 结构字节数与布局版本；调用前填写 size。
    // token：本次借用纹理的凭据，用 ReleaseGpuFrame(token) 归还，不是系统句柄。
    // sharedHandle：DXGI 共享纹理句柄的 x64 数值，只能在凭据有效期间使用。
    uint64_t token, sharedHandle;
    uint32_t textureFormat, captureBackend; // 纹理的 DXGI_FORMAT 数值，以及 FsCaijiFangshi 采集编号。
    uint32_t adapterLuidLow;               // 显卡适配器 LUID 的低 32 位，须与 high 一起比较。
    int32_t adapterLuidHigh;               // 显卡适配器 LUID 的高 32 位，用于选择同一适配器。
    uint32_t flip, reserved;               // flip=1 表示需要上下翻转，0 表示不翻转；reserved 保留为 0。
    FsZhen frame;                         // 这份纹理对应的帧尺寸、快照号和绑定版本。
} FsGpuZhen;
// AcquireGpuFrame 成功返回前已确认生产方 GPU 复制完成；持有期间采集端不覆盖该槽。
// 调用方须等自己提交的 GPU 读取完成再释放 token；解绑后已借用纹理仍保留到归还。
// 扩展统计不改变旧结构布局。readbackFrame 为最近一次 CPU 读取尝试的快照编号。
typedef struct FsCaijiZhuangtai {
    // size/version：结构字节数与布局版本；captureBackend：FsCaijiFangshi。
    // gpuSlots：已分配快照槽位数，最多 3 个，包含解绑后仍被借用的旧槽位。
    uint32_t size, version, captureBackend, gpuSlots;
    // activeLeases：尚未归还的使用凭据总数，包含内部 CPU 读取；不是占用槽位数。
    // textureFormat：最新快照的 DXGI_FORMAT；当前没有快照时为 0。
    uint32_t activeLeases, textureFormat;
    // skippedFrames：无空闲槽位时跳过的更新次数，不代表所有未采集的源帧。
    // cpuReadbacks：本次绑定中已完成并登记的 CPU 读回尝试数，包含失败，不包含容量查询。
    // readbackFrame/readbackBindingVersion：最近一次登记读回的快照编号及绑定版本。
    uint64_t skippedFrames, cpuReadbacks, readbackFrame, readbackBindingVersion;
    double lastReadbackMs;                 // 最近一次登记 CPU 读回耗时，单位毫秒，含 GPU 等待。
    int32_t lastReadbackResult;            // 最近一次登记 CPU 读回的 FsJieguo，未读取时不能仅据此判断成功。
    uint32_t reserved;                     // 保留字段，调用方初始化为 0。
} FsCaijiZhuangtai;
// cpuReadbacks=0 表示尚未登记 CPU 读回，不能把 lastReadbackMs=0 解释为读回没有耗时。
// 这些统计在解绑时清零；gpuSlots/activeLeases 则按仍存活的资源即时统计。
// 扩展结构独立版本化，不改变旧采集结构。backend: 0=cpu，1=gpu。
// result/detail 为当前线程最近一次查找的诊断，未命中并不等于执行失败。
typedef struct FsChazhaoZhuangtai {
    // size/version：结构字节数与布局版本；backend：0=CPU、1=GPU；reserved 保留为 0。
    uint32_t size, version, backend, reserved;
    int32_t result, detail;                // 当前线程最近一次查找的 FsJieguo 与底层错误信息。
    // deviceSerial：当前使用的处理设备编号，0 表示尚未取得。
    // devicesCreated：共享处理设备累计创建数；shaderBuilds：查找着色器累计构建数。
    // resourceBuilds：查找 GPU 资源累计构建数；templateUploads：模板累计上传次数。
    // 累计值用于观察复用情况，不表示单次查找的耗时，也不是当前存活资源数量。
    uint64_t deviceSerial, devicesCreated, shaderBuilds, resourceBuilds, templateUploads;
    // frameNumber/bindingVersion：本线程最近查找所用快照及绑定版本。
    // resultReadbackBytes：最近一次查找读取的 GPU 结果字节数，不是整帧像素读回量。
    uint64_t frameNumber, bindingVersion, resultReadbackBytes;
} FsChazhaoZhuangtai;
// OCR 独立状态，不修改现有结构。索引为 0～99，backend=0/1 为 CPU/DirectML，
// engine=0/1 为神经网络模型/点阵字库。message 是当前线程的 UTF-8 诊断。
typedef struct FsOcrZhuangtai {
    // size/version：结构字节数与布局版本；backend：0=CPU、1=DirectML。
    // engine：0=神经网络模型、1=点阵字库；模型和字库的选择与找图后端相互独立。
    uint32_t size, version, backend, engine;
    // modelIndex/dictIndex：当前选择的模型、字库序号；result：本线程 OCR 诊断 FsJieguo。
    int32_t modelIndex, dictIndex, result;
    uint32_t loadedSessions;               // 当前已加载的模型推理会话数，不等于登记模型的数量。
    // sessionsCreated：累计创建推理会话数；devicesCreated：累计创建 DirectML 设备数。
    // frameNumber/bindingVersion：窗口识别所用快照及绑定版本，文件识别时不代表窗口画面。
    uint64_t sessionsCreated, devicesCreated, frameNumber, bindingVersion;
    // 以下耗时单位均为毫秒：initializationMs=初始化，acquireMs=采集取帧，
    // preprocessMs=图像预处理，detectionMs=文字检测，recognitionMs=文字识别，
    // resultMs=结果整理；未执行阶段可以为 0，不表示 GPU 精确执行时长。
    double initializationMs, acquireMs, preprocessMs, detectionMs, recognitionMs, resultMs;
    char message[512];                    // 当前线程的 UTF-8 诊断信息，结构内固定数组，无须单独释放。
} FsOcrZhuangtai;
// 仅供两个 DLL 内部协作。指针属于采集 DLL，不允许调用者 Release。
// token 持有统一处理设备的上下文锁；必须由获取线程释放，不能嵌套获取。
typedef struct FsGpuChuli {
    uint32_t size, version;                // 结构字节数与布局版本；调用前填写 size。
    // token：处理上下文使用凭据，同线程调用 ZhuaTuReleaseProcessor 归还。
    // device/context：D3D11 设备和立即上下文指针的 x64 数值，只有凭据有效时才允许使用。
    // deviceSerial：处理设备编号；devicesCreated：共享处理设备累计创建数。
    uint64_t token, device, context, deviceSerial, devicesCreated;
} FsGpuChuli;
#pragma pack(pop)

#ifdef __cplusplus
static_assert(sizeof(FsJieguoXiang) == 40);
static_assert(sizeof(FsTongyongJieguo) == 40);
static_assert(sizeof(FsZhen) == 88);
static_assert(sizeof(FsZhuangtai) == 208);
static_assert(sizeof(FsGpuZhen) == 136);
static_assert(sizeof(FsCaijiZhuangtai) == 72);
static_assert(sizeof(FsOcrZhuangtai) == 624);
extern "C" {
#endif
// 采集查询结构和像素缓冲区由调用者持有；通用识别结果由 DLL 持有，通过 FreeResult 等接口释放。
// 除绑定、解绑和找图 DLL 的模式选择返回 1/0 外，采集 result 使用 FsJieguo。
// 采集 DLL 的 ZhuaTuBegin 返回 FsJieguo；display 同时决定绑定及采集方式。
typedef int (WINAPI* FsBindFn)(HWND, const char*, const char*, const char*, int);
typedef int (WINAPI* FsUnbindFn)(void);
typedef int (WINAPI* FsCopyFn)(void*, uint64_t, FsZhen*);
typedef int (WINAPI* FsStatusFn)(FsZhuangtai*);
typedef int (WINAPI* FsMapFn)(double, double, double*, double*, int);
typedef int (WINAPI* FsBackendFn)(const char*);
typedef int (WINAPI* FsGpuAcquireFn)(FsGpuZhen*);
typedef int (WINAPI* FsGpuReleaseFn)(uint64_t);
typedef int (WINAPI* FsCaptureStatusFn)(FsCaijiZhuangtai*);
typedef HRESULT (WINAPI* FsProcessorAcquireFn)(uint32_t, int32_t, FsGpuChuli*);
typedef HRESULT (WINAPI* FsProcessorReleaseFn)(uint64_t);
#ifdef __cplusplus
}
#endif
