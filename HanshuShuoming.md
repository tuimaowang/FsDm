# FsDm 函数使用说明

**主接口：`ZhaoTu.dll`。** 文末另列 `ZhuaTu.dll` 的底层协作入口。

## 阅读及调用约定

- 实际接口是 C 接口：`int` 为 32 位，`uintptr_t` 在 x64 为 64 位，`double` 为双精度；输入路径和文字使用 UTF-8。
- 26 个找图、找色、OCR、找字函数统一返回 `const FsTongyongJieguo*`。直接读取 `status`、`count`、`items[i].x/y/id/color/text/file`，不解析坐标字符串，不使用 JSON。
- `status=0` 表示正常执行；`count=0` 表示正常无命中，`count>0` 表示存在结果。错误状态非 0；只有结果分配失败才返回 `nullptr`。`id=0` 是列表第一项，不能当作失败。
- 工作台和 C/C++ 调用使用同一组输入参数；坐标从返回结构体中读取。工作台完整显示全文和每一项，再自动精确释放本次结果。
- 每次返回的结构体、条目数组和文字均由 DLL 独立分配，后续调用、解绑、原线程退出不覆盖它。调用方用 `FreeResult` 精确释放指定结构体，或用 `FreeAllResults` 释放全部结果，禁止自行 `delete`、`delete[]`、`free`。释放后所有内部指针一并失效。
- `MatchPicName`、`GetOcrError` 返回独立 UTF-8 文字数组；通过 `FreeAllResults()` 或兼容别名 `FreeAllStringResults()` 回收。`GetScreenData` 是单独的原始像素指针，寿命和释放规则见其说明。
- 标为“C++ 调用”的示例不在工作台输入。业务程序包含交付目录中的 `FsDmJiekou.h`，通过 `LoadLibraryW` 加载 DLL、`GetProcAddress` 获取函数地址，并使用头文件中对应的函数指针类型调用。工作台路径直接写反斜杠，C++ 字符串用 `\\` 或原始字符串。
- 函数名和模式区分大小写。窗口句柄必须替换成实际值；窗口接口需要绑定本进程窗口并等待首帧。
- 采集由 `BindWindow` 第二参数选择 `dx/obs/wgc`；未绑定初始显示 `dx`。找图找色默认 `gpu`；OCR 默认 `model` 引擎、`cpu` 后端、0 号模型，三者相互独立。
- 绑定、设置、加载、保存、释放通常返回 `1` 成功、`0` 失败；帧读取、状态查询和坐标换算使用 `FsJieguo` 状态码，`0` 成功。
- 找色区域右下边界包含；截图、找图、OCR 区域右下边界不包含。以本页实际行为为准。

## 函数目录

正文依次按 **初始设置 → 找图 → 找色 → OCR 识别** 排列。每个函数提供原型、参数、返回值和示例，公共结构及底层接口见文末附录。

| 分类 | 内容 | 函数 |
|---|---|---|
| 初始设置 | 绑定与采集设置 | [BindWindow](#BindWindow)、[UnBindWindow](#UnBindWindow) |
| 初始设置 | 路径设置 | [SetPath](#SetPath) |
| 初始设置 | 找图找色后端与状态 | [SetFindBackend](#SetFindBackend)、[GetFindStatus](#GetFindStatus) |
| 初始设置 | 截图保存 | [Capture](#Capture)、[CaptureJpg](#CaptureJpg)、[CapturePng](#CapturePng)、[CaptureGif](#CaptureGif) |
| 初始设置 | 帧读取与采集诊断 | [GetStatus](#GetStatus)、[GetCaptureStatus](#GetCaptureStatus)、[CopyFrame](#CopyFrame)、[AcquireGpuFrame](#AcquireGpuFrame)、[ReleaseGpuFrame](#ReleaseGpuFrame)、[MapPoint](#MapPoint) |
| 初始设置 | 区域数据与卡屏检测 | [GetScreenData](#GetScreenData)、[IsDisplayDead](#IsDisplayDead) |
| 初始设置 | 结果内存管理 | [FreeResult](#FreeResult)、[FreeAllResults](#FreeAllResults)、[FreeAllStringResults](#FreeAllStringResults) |
| 找图 | 模板加载与管理 | [SetTemplateCacheLimit](#SetTemplateCacheLimit)、[LoadPic](#LoadPic)、[LoadPicByte](#LoadPicByte)、[FreePic](#FreePic)、[MatchPicName](#MatchPicName) |
| 找图 | 图片匹配 | [FindPic](#FindPic)、[FindPicS](#FindPicS)、[FindPicEx](#FindPicEx)、[FindPicExS](#FindPicExS)、[OpenCV 找图](#CvFindPic)、[AiFindPic 兼容接口](#AiFindPic) |
| AI 识别 | YOLO 模型与目标检测 | [AiYoloSetVersion](#AiYoloSetVersion)、[AiYoloSetModel](#AiYoloSetModel)、[AiYoloSetModelMemory](#AiYoloSetModelMemory)、[AiYoloUseModel](#AiYoloUseModel)、[AiYoloFreeModel](#AiYoloFreeModel)、[AiYoloDetectObjects](#AiYoloDetectObjects)、[AiYoloDetectObjectsToFile](#AiYoloDetectObjectsToFile)、[AiYoloDetectObjectsToDataBmp](#AiYoloDetectObjectsToDataBmp)、[AiYoloSortsObjects](#AiYoloSortsObjects)、[AiYoloObjectsToString](#AiYoloObjectsToString) |
| 找色 | 取色与比色 | [GetColor](#GetColor)、[CmpColor](#CmpColor) |
| 找色 | 单点与多点找色 | [FindColor](#FindColor)、[FindColorEx](#FindColorEx)、[FindMultiColor](#FindMultiColor)、[FindMultiColorEx](#FindMultiColorEx) |
| OCR 识别 | 模型与引擎设置 | [SetOcrModel](#SetOcrModel)、[SetOcrModelFiles](#SetOcrModelFiles)、[UseOcrModel](#UseOcrModel)、[FreeOcrModel](#FreeOcrModel)、[SetOcrBackend](#SetOcrBackend)、[SetOcrEngine](#SetOcrEngine) |
| OCR 识别 | 点阵字库管理 | [SetDict](#SetDict)、[UseDict](#UseDict)、[GetNowDict](#GetNowDict)、[GetDictCount](#GetDictCount)、[ClearDict](#ClearDict) |
| OCR 识别 | 文字识别 | [Ocr](#Ocr)、[OcrEx](#OcrEx)、[OcrExOne](#OcrExOne)、[OcrInFile](#OcrInFile) |
| OCR 识别 | 查找文字 | [FindStr](#FindStr)、[FindStrS](#FindStrS)、[FindStrEx](#FindStrEx)、[FindStrExS](#FindStrExS) |
| OCR 识别 | 快速查找文字 | [FindStrFast](#FindStrFast)、[FindStrFastS](#FindStrFastS)、[FindStrFastEx](#FindStrFastEx)、[FindStrFastExS](#FindStrFastExS) |
| OCR 识别 | OCR 诊断 | [GetOcrError](#GetOcrError)、[GetOcrStatus](#GetOcrStatus) |

## 一、初始设置

### 常用调用流程

```text
BindWindow(0x6b12e8, "wgc", "normal", "normal", 0)
status
SetPath("C:\Users\Administrator\Documents\DaMo\FsDm")
CapturePng(0, 0, 1172, 590, "screen.png")
LoadPic("test.bmp")
SetFindBackend("gpu")
FindPic(0, 0, 1172, 590, "test.bmp", "000000", 0.9, 0)
FindColor(0, 0, 1171, 589, "ffffff-000000", 1.0, 0)
SetOcrEngine("model")
SetOcrBackend("cpu")
Ocr(0, 0, 1172, 590, "", 0.5)
FindStr(0, 0, 1172, 590, "开始游戏|确定", "", 0.5)
UnBindWindow()
```

先替换实际句柄，按窗口客户区尺寸调整区域；`status` 确认已有有效画面后再截图或查找。`status` 是工作台命令，不是导出函数。

### 绑定与采集设置

<a id="BindWindow"></a>

#### BindWindow

**函数简介：**

绑定本进程的指定窗口，同时选择采集方式；绑定成功后才开始采集。

**函数原型：**

```cpp
int WINAPI BindWindow(HWND hwnd, const char* display, const char* mouse, const char* keypad, int mode);
```

**参数定义：**

- hwnd：本进程的有效窗口句柄；x64 不得截断成 32 位。
- display："dx"、"obs" 或 "wgc"，直接选择对应采集方式，区分大小写。当前不支持 "gdi"、"gdi2"、"normal"、"dx2"。
- mouse："normal"、"windows" 或 "dx"，当前仅保存配置。
- keypad："normal"、"windows" 或 "dx"，当前仅保存配置。
- mode：0 或 1。

**返回值：**

- 1：绑定成功，可能仍在等待首帧。
- 0：绑定失败。

**示例：**

```text
BindWindow(0x实际窗口句柄, "dx", "normal", "normal", 0)
UnBindWindow()
BindWindow(0x实际窗口句柄, "wgc", "normal", "normal", 0)
UnBindWindow()
BindWindow(0x实际窗口句柄, "obs", "normal", "normal", 0)
```

**注意：** 同进程只保留一个绑定。相同参数重复绑定成功；改变参数（包括采集方式）须先解绑。失败绑定不改变原绑定及其采集方式。最多等待初始化 7 秒，不等待首帧。绑定线程退出或窗口销毁会自动解绑。鼠标和键盘参数仅保存配置，不执行键鼠操作。

<a id="UnBindWindow"></a>

#### UnBindWindow

**函数简介：**

解除绑定，停止采集并使当前帧缓存失效。

**函数原型：**

```cpp
int WINAPI UnBindWindow(void);
```

**参数定义：**

- 无。

**返回值：**

- 1：解绑成功，重复解绑也成功。
- 0：解绑失败。

**示例：**

```text
UnBindWindow()
```

**注意：** 已经取得的 GPU 帧租约仍需 ReleaseGpuFrame；底层拦截保持加载，可再次绑定。

#### SetCaptureFrameInterval

`int WINAPI SetCaptureFrameInterval(int interval)`

`interval=-1` 固定当前已发布快照并暂停发布新快照；之后的找图、找色、OCR、CV 找图及其他共享采集帧的处理都继续读取该快照。固定期间 `GetFrameNumber` 不再递增。需要已有快照，否则返回 0 且原设置不变。

`interval>=0` 表示每次采集后跳过的输入帧数：`0` 表示逐帧采集，`1` 表示每次采集后跳过 1 帧，`2` 表示跳过 2 帧，以此类推。设置成功后从下一输入帧重新计数；传入非负值会解除固定并恢复采集。小于 `-1`、未绑定或参数无效时返回 0，原设置不变。每次新绑定和解绑后的默认值均为 `0`。

```text
SetCaptureFrameInterval(-1)  // 固定当前快照
SetCaptureFrameInterval(0)   // 恢复逐帧采集
SetCaptureFrameInterval(1)   // 每次采集后跳过 1 个输入帧
```

### 路径设置

<a id="SetPath"></a>

#### SetPath

**函数简介：**

设置截图、模板及 OCR 资源等相对路径使用的基础目录。

**函数原型：**

```cpp
int WINAPI SetPath(const char* path);
```

**参数定义：**

- path：UTF-8 目录路径；目录必须已经存在。

**返回值：**

- 1：设置成功。
- 0：路径无效、目录不存在或设置失败。

**示例：**

```text
SetPath("C:\Users\Administrator\Documents\DaMo\FsDm")
```

**注意：** 绝对路径不受此设置影响。尚未设置时，相对路径按宿主进程当前目录解析。此函数不会创建目录。

### 找图找色后端与状态

<a id="SetFindBackend"></a>

#### SetFindBackend

**函数简介：**

选择找图、找色和取色使用 CPU 或 GPU 执行。

**函数原型：**

```cpp
int WINAPI SetFindBackend(const char* backend);
```

**参数定义：**

- backend："cpu" 或 "gpu"，区分大小写。

**返回值：**

- 1：设置成功。
- 0：参数错误，保留原设置。

**示例：**

```text
SetFindBackend("gpu")
```

**注意：** 默认 gpu。允许绑定期间切换，不重启采集或设备；与采集方式和 OCR 后端相互独立。GPU 不可用时不会自动退回 CPU。

<a id="GetFindStatus"></a>

#### GetFindStatus

**函数简介：**

查询本线程最近一次找图找色诊断及共享资源累计计数。

**函数原型：**

```cpp
int WINAPI GetFindStatus(FsChazhaoZhuangtai* status);
```

**参数定义：**

- status：FsChazhaoZhuangtai 输出结构指针；调用前设置 size=sizeof(FsChazhaoZhuangtai)。

**返回值：**

- 返回 FsJieguo 状态码：0（FS_CHENGGONG）表示查询成功；4（FS_CANSHU_CUOWU）表示指针或结构 size 不正确。查询成功不代表已经绑定或已有有效画面。

**示例：**

```cpp
// C++ 调用；启用工作台的调试版可用 status 查看摘要；发行版无工作台。
FsChazhaoZhuangtai s{};
s.size = sizeof(s);
int ret = GetFindStatus(&s);
```

**注意：** s.result/detail 才是最近查找诊断。未命中不等于执行出错；状态查询本身不触发找图或像素读回。

### 截图保存

<a id="Capture"></a>

#### Capture

**函数简介：**

抓取指定区域，可将截图登记为找图模板、保存为 24 位 BMP 位图，或同时执行。

**函数原型：**

```cpp
int WINAPI Capture(int x1, int y1, int x2, int y2, const char* name, int mode = 2);
```

**参数定义：**

- x1、y1：区域左上角坐标。
- x2、y2：区域右下角坐标，不包含右下边界；窗口接口使用客户区物理像素。
- name：模式 1 时为模板别名；模式 2 时为输出文件路径；模式 3 时同时作为模板别名和输出路径。
- mode：1 仅登记模板，2 仅保存文件，3 同时登记模板和保存文件。C++ 和工作台可省略，默认 2；C 或动态调用必须传入 6 个参数。

**返回值：**

- 1：保存成功。
- 0：未绑定、无有效帧、区域或路径无效、编码或写入失败等。

**示例：**

```text
Capture(0, 0, 500, 500, "screen.bmp")
```

**注意：** 需先绑定并取得首帧。模式 1 无需写文件，别名无需带扩展名；模板使用原始截图像素并占用模板缓存预算，可用 FreePic 释放。模式 2、3 成功保存时覆盖同名文件。模式 3 若文件已写入但模板缓存空间不足，会返回 0，文件不会回滚。

<a id="CaptureJpg"></a>

#### CaptureJpg

**函数简介：**

抓取指定区域，可将截图登记为找图模板、保存为 JPEG 图片，或同时执行。

**函数原型：**

```cpp
int WINAPI CaptureJpg(int x1, int y1, int x2, int y2, const char* name, int mode = 2);
```

**参数定义：**

- x1、y1：区域左上角坐标。
- x2、y2：区域右下角坐标，不包含右下边界；窗口接口使用客户区物理像素。
- name：模式 1 时为模板别名；模式 2 时为输出文件路径；模式 3 时同时作为模板别名和输出路径。
- mode：1 仅登记模板，2 仅保存文件，3 同时登记模板和保存文件。C++ 和工作台可省略，默认 2；C 或动态调用必须传入 6 个参数。

**返回值：**

- 1：保存成功。
- 0：保存失败。

**示例：**

```text
CaptureJpg(0, 0, 500, 500, "screen.jpg")
```

**注意：** 绑定和模式行为同 Capture。本接口没有 JPEG 质量参数。模式 1/3 登记的是压缩前的原始截图像素；模式 2/3 的文件路径规则同 Capture。

<a id="CapturePng"></a>

#### CapturePng

**函数简介：**

抓取指定区域，可将截图登记为找图模板、保存为 PNG 图片，或同时执行。

**函数原型：**

```cpp
int WINAPI CapturePng(int x1, int y1, int x2, int y2, const char* name, int mode = 2);
```

**参数定义：**

- x1、y1：区域左上角坐标。
- x2、y2：区域右下角坐标，不包含右下边界；窗口接口使用客户区物理像素。
- name：模式 1 时为模板别名；模式 2 时为输出文件路径；模式 3 时同时作为模板别名和输出路径。
- mode：1 仅登记模板，2 仅保存文件，3 同时登记模板和保存文件。C++ 和工作台可省略，默认 2；C 或动态调用必须传入 6 个参数。

**返回值：**

- 1：保存成功。
- 0：保存失败。

**示例：**

```text
CapturePng(0, 0, 500, 500, "screen.png")
```

**注意：** 绑定和模式行为同 Capture。模式 1/3 登记原始截图像素；模式 2/3 的文件路径规则同 Capture。

<a id="CaptureGif"></a>

#### CaptureGif

**函数简介：**

持续抓取指定区域并保存为 GIF，或保存单帧 GIF。

**函数原型：**

```cpp
int WINAPI CaptureGif(int x1, int y1, int x2, int y2, const char* file, int delay, int time);
```

**参数定义：**

- x1、y1：区域左上角坐标。
- x2、y2：区域右下角坐标，不包含右下边界；窗口接口使用客户区物理像素。
- file：输出 GIF 文件路径。
- delay：帧间隔，单位毫秒；0 表示单帧静态 GIF。
- time：录制总时长，单位毫秒；动画使用正值，静态可填 0。

**返回值：**

- 1：保存成功。
- 0：参数、采集、编码或写入失败。

**示例：**

```text
CaptureGif(0, 0, 500, 500, "screen.gif", 100, 3000)
CaptureGif(0, 0, 500, 500, "static.gif", 0, 0)
```

**注意：** 调用期间同步等待，工作台会被录制占用。GIF 时间精度为 10 毫秒，实际间隔还受编码开销影响；期间解绑或尺寸改变会失败。

### 帧读取与采集诊断

<a id="GetStatus"></a>

#### GetStatus

**函数简介：**

查询绑定状态、最近采集诊断及最新帧元数据。

**函数原型：**

```cpp
int WINAPI GetStatus(FsZhuangtai* status);
```

**参数定义：**

- status：FsZhuangtai 输出结构指针；调用前设置 size=sizeof(FsZhuangtai)。

**返回值：**

- 返回 FsJieguo 状态码：0（FS_CHENGGONG）表示查询成功；4（FS_CANSHU_CUOWU）表示指针或结构 size 不正确。查询成功不代表已经绑定或已有有效画面。

**示例：**

```cpp
// C++ 调用；工作台可直接输入 status。
FsZhuangtai s{};
s.size = sizeof(s);
int ret = GetStatus(&s);
```

**注意：** ret=0 仅表示查询成功；请看 s.bound、s.hasFrame 和 s.result。此函数不读取像素，不承担找图或 OCR 专属诊断。

<a id="GetCaptureStatus"></a>

#### GetCaptureStatus

**函数简介：**

查询采集后端、GPU 槽位/租约数量和 CPU 读回统计。

**函数原型：**

```cpp
int WINAPI GetCaptureStatus(FsCaijiZhuangtai* status);
```

**参数定义：**

- status：FsCaijiZhuangtai 输出结构指针；调用前设置 size=sizeof(FsCaijiZhuangtai)。

**返回值：**

- 返回 FsJieguo 状态码：0（FS_CHENGGONG）表示查询成功；4（FS_CANSHU_CUOWU）表示指针或结构 size 不正确。查询成功不代表已经绑定或已有有效画面。

**示例：**

```cpp
// C++ 调用。
FsCaijiZhuangtai s{};
s.size = sizeof(s);
int ret = GetCaptureStatus(&s);
```

**注意：** 不触发 CPU 像素读回。lastReadbackResult 表示最近读回尝试结果，cpuReadbacks 为读回尝试累计次数。

<a id="CopyFrame"></a>

#### CopyFrame

**函数简介：**

按需把最新 GPU 快照读取为 CPU BGRA，并复制到调用方缓冲区。

**函数原型：**

```cpp
int WINAPI CopyFrame(void* pixels, uint64_t capacity, FsZhen* frame);
```

**参数定义：**

- pixels：调用方 CPU 缓冲区；查询容量时可为 nullptr。
- capacity：缓冲区容量，单位字节；查询时填 0。
- frame：FsZhen 输入输出结构，调用前设置 size=sizeof(FsZhen)。

**返回值：**

- 0：成功复制一帧。
- 3（FS_HUANCHONG_BUZU）：容量不足，通过 frame.requiredBytes 获取所需容量。
- 其他值：未绑定、等待首帧、参数错误或采集失败等，见状态码表。

**示例：**

```cpp
// C++ 调用；工作台不支持此缓冲区接口。
FsZhen f{};
f.size = sizeof(f);
int ret = CopyFrame(nullptr, 0, &f);
if (ret == FS_HUANCHONG_BUZU) {
    std::vector<unsigned char> pixels(f.requiredBytes);
    ret = CopyFrame(pixels.data(), pixels.size(), &f);
    // 仅 ret == FS_CHENGGONG 才能使用本次像素。
    // 尺寸若改变导致容量仍不足，应重新分配并重试。
}
```

**注意：** 示例需包含 <vector>。输出从上到下、每行 width×4 字节、紧密排列 BGRA；最大 16,777,216 像素。容量查询和容量不足不会读回。暂停呈现保留最新帧，可比较 frameNumber。

<a id="AcquireGpuFrame"></a>

#### AcquireGpuFrame

**函数简介：**

获取最新 GPU 共享纹理的只读租约，持有期间纹理不会被采集端覆盖。

**函数原型：**

```cpp
int WINAPI AcquireGpuFrame(FsGpuZhen* frame);
```

**参数定义：**

- frame：FsGpuZhen 输出结构指针；调用前设置 size=sizeof(FsGpuZhen)。

**返回值：**

- 0：取得可读资源，生产方 GPU 复制已完成。
- 其他状态码：未绑定、等待首帧、资源忙、GPU 等待超时等。

**示例：**

```cpp
// C++ 调用；工作台不支持此结构体接口。
FsGpuZhen g{};
g.size = sizeof(g);
if (AcquireGpuFrame(&g) == FS_CHENGGONG) {
    // 同进程、同适配器 D3D11 设备打开 g.sharedHandle。
    // 只读使用，并确认调用方 GPU 操作完成。
    ReleaseGpuFrame(g.token);
}
```

**注意：** sharedHandle 是旧式 DXGI 共享句柄，禁止 CloseHandle。纹理格式不保证 BGRA，按 textureFormat 处理，flip=1 表示上下翻转。等待上限 2 秒；资源池最多三个槽位，租约请求最多 64 个。

<a id="ReleaseGpuFrame"></a>

#### ReleaseGpuFrame

**函数简介：**

释放此前 AcquireGpuFrame 获得的 GPU 帧租约。

**函数原型：**

```cpp
int WINAPI ReleaseGpuFrame(uint64_t token);
```

**参数定义：**

- token：AcquireGpuFrame 返回的有效租约标识，64 位无符号整数。

**返回值：**

- 0：释放成功。
- 4：token 无效或重复释放等。

**示例：**

```cpp
// C++ 调用；token 必须来自本次进程实际取得的租约。
int ret = ReleaseGpuFrame(g.token);
```

**注意：** 须等调用方 GPU 操作完成后再释放。解绑后仍可释放旧 token；不能仅提交 GPU 命令就立即释放。

<a id="MapPoint"></a>

#### MapPoint

**函数简介：**

在客户区坐标与原始采集画面坐标之间换算。

**函数原型：**

```cpp
int WINAPI MapPoint(double x, double y, double* outX, double* outY, int direction);
```

**参数定义：**

- x、y：待换算坐标，允许小数，必须在输入坐标系有效范围内。
- outX、outY：double 输出指针，接收换算结果。
- direction：0 表示客户区到原始画面；1 表示原始画面到客户区。

**返回值：**

- 0：换算成功。
- 其他状态码：参数错误、未绑定、无有效帧等。

**示例：**

```cpp
// C++ 调用；工作台不支持 double* 输出参数。
double imageX{}, imageY{};
int ret = MapPoint(100.0, 50.0, &imageX, &imageY, 0);
```

**注意：** 按完整画面对应完整客户区比例换算，不推断程序内部视口。不取整，负坐标和越界不接受。

### 区域数据与卡屏检测

<a id="GetScreenData"></a>

#### GetScreenData

**函数简介：** 返回指定区域的 CPU 二进制颜色数据地址，供同进程代码使用。

**函数原型：**

```cpp
uintptr_t WINAPI GetScreenData(int x1, int y1, int x2, int y2);
```

**参数定义：**

- `x1/y1`：区域左上角，客户区物理像素坐标，不能为负数。
- `x2/y2`：区域右下角，不包含此边界，要求整个区域有效；不自动裁剪越界区域。

**返回值：** 非零为区域数据首地址，0 为失败，错误查询同线程 `GetFindStatus`。
数据没有文件头，从上到下、每行从左到右紧密排列，宽 `x2-x1`、高 `y2-y1`，
容量为 `宽×高×4`。每个整数像素是 `00RRGGBB`，Windows 小端内存顺序为 `B,G,R,0`。
最多 16,777,216 个区域像素；客户区尺寸与原始渲染尺寸不同时按比例最近邻取样。

**示例（C++ 调用）：**

```cpp
const uintptr_t address = GetScreenData(0, 0, 100, 100);
if (address) {
    const auto* pixels = reinterpret_cast<const uint32_t*>(address);
    const uint32_t firstColor = pixels[0]; // 00RRGGBB
}
```

**注意：** 必须使用 `uintptr_t` 等 64 位类型接收，不可用 Windows 的 32 位 `long`。
内存属于 DLL，禁止释放；至同线程下一次 `GetScreenData`（包括失败调用）或该线程退出前有效。
需要跨调用保留时立即复制，其他线程的调用不覆盖本线程地址。
这个地址只在加载 DLL 的目标进程中有效；数据不是完整 BMP 文件，不能直接交给 `LoadPicByte`。
该函数必需按需读取 CPU，不受找图后端设置影响，不重建设备。
工作台可直接调用并显示十六进制地址，以及采集取帧、区域整理和其他耗时。

<a id="IsDisplayDead"></a>

#### IsDisplayDead

**函数简介：** 判断指定客户区区域在指定时间内是否一直不变；检查到图像变化立即返回。

**函数原型：**

```cpp
int WINAPI IsDisplayDead(int x1, int y1, int x2, int y2, int t);
```

**参数定义：**

- `x1/y1`：区域左上角，客户区物理像素坐标。
- `x2/y2`：区域右下角，不包含此边界；要求非空且整个区域在客户区内。
- `t`：检测时长，单位为秒，必须大于等于 0。0 表示验证并读取一次有效区域。

**返回值：**

- `0`：检测到变化；也用于非法参数、解绑/重绑、窗口销毁、尺寸变化或采集失败。
- `1`：整个检测时段内没有检测到区域像素变化。
- 区分正常变化与错误时，使用同线程 `GetFindStatus`：正常变化为 `FS_CHENGGONG`。

**示例：**

```text
IsDisplayDead(0, 0, 100, 100, 5)
```

**注意：** 需先绑定并等待首帧。函数同步等待，期间该工作台不能接收下一条命令；
其他业务线程可以调用接口。每约 50ms 检查一次，不能保证发现两个检查之间短暂出现又消失的变化。
新快照也会比较实际 BGR 数据，不能把“快照编号在增加”当作画面变化；没有新快照时复用基准，不重复 CPU 读回。
取样区域保持原位，不进行找图、位移容错或临时文件保存，不改变 CPU/GPU 找图设置。
工作台分别显示采集取帧、区域整理及比较、检测等待和其他耗时。

### 通用结果与内存管理

22 个识别函数共享下面的结构，按 8 字节对齐；本项目 x64 下两个结构均为 40 字节。只传 C 的整数、指针和数组，不跨 DLL 传 C++ 容器。

```cpp
enum FsJieguoZiduan {
    FS_JG_ID = 1u, FS_JG_XY = 2u, FS_JG_COLOR = 4u,
    FS_JG_TEXT = 8u, FS_JG_FILE = 16u
};
struct FsJieguoXiang {
    uint32_t fields;
    int32_t id;
    int32_t x, y;
    uint32_t color;
    const char* text;
    const char* file;
};
struct FsTongyongJieguo {
    uint32_t size, version;
    int32_t status, detail;
    uint32_t count;
    const FsJieguoXiang* items;
    const char* fullText;
};
```

- `size/version`：结构大小和版本，目前为 40 和 1。
- `status/detail`：执行状态与底层错误。`status=0/count=0` 是正常没有结果；错误不会携带半途生成的坐标和全文。
- `count/items`：结果数量和连续数组。`count=0` 时 `items=nullptr`，只能在 `i<count` 时读取数组。
- `fields`：位标志，说明本项哪些字段有效。黑色 `color=0` 和坐标 `x=0` 都是正常值，不用数值是否为零判断字段存在。
- `id`：颜色、模板、目标文字的列表索引，或 OCR 字符顺序号；多点找色的 id 为 0。
- `x/y`：类型为 `int32_t`，各占 4 字节。窗口接口为客户区物理像素；OcrInFile 为文件像素。找图是模板左上角，找字是首字符位置，多点找色是整组匹配的首点。
- `color`：`0xRRGGBB` 的整数值，不转换颜色文本即可比较。
- `text/file/fullText`：UTF-8 文字指针。数字和坐标可直接从字段读取，文字按 UTF-8 字符串读取。没有相应内容时指向空字符串。

| 函数 | 结果条目 |
| --- | --- |
| GetColor | 1 个 XY、COLOR 条目 |
| CmpColor | 匹配时 1 个 ID、XY、COLOR 条目，不匹配 0 个 |
| FindColor / FindColorEx | ID、XY；单个 / 多个 |
| FindMultiColor / FindMultiColorEx | ID、XY；单组 / 多组匹配的首点 |
| FindPic / FindPicS | 0 或 1 个 ID、XY、FILE 条目 |
| FindPicEx / FindPicExS | 0～1500 个 ID、XY、FILE 条目 |
| Ocr / OcrEx / OcrExOne / OcrInFile | fullText 全文，加 ID、XY、TEXT 字符数组 |
| FindStr / FindStrS / FindStrFast / FindStrFastS | 0 或 1 个 ID、XY、TEXT 条目 |
| FindStrEx / FindStrExS / FindStrFastEx / FindStrFastExS | 0～1500 个 ID、XY、TEXT 条目 |

C++ 调用示例：module 为已加载的 ZhaoTu.dll 模块句柄。调用前先绑定窗口、等待首帧，并通过 SetPath 设置模板目录。

```cpp
#include "FsDmJiekou.h"
#include <cstdio>

void TestFindPic(HMODULE module) {
    if (!module) return;
    auto find = reinterpret_cast<FsDm_FindPicExFn>(
        GetProcAddress(module, "FindPicEx"));
    auto release = reinterpret_cast<FsDm_FreeResultFn>(
        GetProcAddress(module, "FreeResult"));
    if (!find || !release) {
        std::printf("无法取得找图或释放接口\n");
        return;
    }
    const FsTongyongJieguo* r = find(0, 0, 834, 419,
        "test.bmp|test1.bmp", "000000", 0.9, 0);
    if (!r) {
        std::printf("结果内存分配失败\n");
        return;
    }
    if (r->status != FS_CHENGGONG) {
        std::printf("失败 status=%d detail=0x%08x\n", r->status,
            static_cast<unsigned int>(r->detail));
    } else {
        std::printf("命中数量=%u\n", r->count);
        for (uint32_t i = 0; i < r->count; ++i) {
            const FsJieguoXiang& item = r->items[i];
            std::printf("id=%d x=%d y=%d file=%s\n",
                item.id, item.x, item.y, item.file);
        }
    }
    release(r);
}
```

工作台显示示例（假设匹配到两组多点图案；坐标仅演示格式）：

```text
返回值: status=0，detail=0x00000000，count=2，总耗时: 2.800 毫秒
items[0]: id=0 x=678 y=72
items[1]: id=0 x=700 y=90
```

工作台显示本次采集、查找、GPU 等待等阶段耗时，并精确释放本次结果。外部调用者保留的其他结果不受影响。

结果不会因后续调用、解绑或原线程退出而失效；它是调用完成时的快照，不会随画面自动改变。DLL 卸载前须释放结果。内部登记表的锁只保护分配及删除，调用者必须协调读取与释放，不能一边使用指针一边释放该结果或全量释放。

<a id="FreeResult"></a>

#### FreeResult

**函数简介：** 精确释放一个通用结果，连同条目数组及所有文字一起释放。

**函数原型：**

```cpp
int WINAPI FreeResult(const FsTongyongJieguo* result);
```

**参数定义：** result 为某个通用识别函数本次返回的指针，不是 items 或 text 的地址。

**返回值：** 1 成功，nullptr 也成功；0 为未知地址或重复释放。

**示例（C++）：**

```cpp
const FsTongyongJieguo* a = GetColor(30, 30);
const FsTongyongJieguo* b = GetColor(40, 40);
if (a && a->status == 0 && a->count == 1) {
    const uint32_t rgb = a->items[0].color;
    const bool white = rgb == 0xffffffu;
    (void)white;
}
FreeResult(a);
FreeResult(b);
```

工作台自动释放自身的结果，无须手动 FreeResult。`FreeResult(0)` 可验证空指针成功；不要填写已自动释放的工作台结果地址。

<a id="FreeAllResults"></a>

#### FreeAllResults

**函数简介：** 释放 22 个通用识别函数及 2 个文字数组函数在所有线程中尚未释放的结果。与 `FreeAllStringResults()` 功能、释放范围及返回规则完全相同，建议统一使用本函数；一次清理只调用其中一个即可。

**函数原型：**

```cpp
int WINAPI FreeAllResults(void);
```

**参数定义：** 无。

**返回值：** 1 成功；无结果、重复调用也成功；0 失败。

**示例：**

```text
FreeAllResults()
```

<a id="FreeAllStringResults"></a>

#### FreeAllStringResults

**函数简介：** 与 `FreeAllResults()` 等效，释放所有线程中尚未释放的通用识别结果结构体、内部条目数组及文字，以及 `MatchPicName`、`GetOcrError` 返回的独立文字数组。虽然名称含 String，本函数并非只释放字符串；两者任选其一，建议使用 `FreeAllResults()`。

**函数原型：**

```cpp
int WINAPI FreeAllStringResults(void);
```

**参数定义：** 无。

**返回值：** 1 成功；无结果、重复调用也成功；0 失败。

**示例：**

```text
FreeAllStringResults()
```

**释放时机：** 全量释放前，须确保所有线程都已停止读取这些结果。释放后，相关结构体、items、text、file、fullText 及独立文字数组指针均失效，不可继续访问；只释放一个通用结果时使用 `FreeResult(result)`。

以上释放接口不清理模板、OCR 模型、字库、设备、GPU 租约及 GetScreenData 原始像素。MatchPicName、GetOcrError 返回独立文字数组，寿命持续到全量释放；不能将它们传给 FreeResult。

## 二、找图

### 找图找色方向与比较规则

| dir | 扫描方向 |
|---|---|
| 0 | 横向扫描：从左到右，从上到下 |
| 1 | 横向扫描：从左到右，从下到上 |
| 2 | 横向扫描：从右到左，从上到下 |
| 3 | 横向扫描：从右到左，从下到上 |
| 4 | 从中心向外，按上、右、下、左螺旋；仅 FindColor 支持 |
| 5 | 纵向扫描：从上到下，从左到右 |
| 6 | 纵向扫描：从上到下，从右到左 |
| 7 | 纵向扫描：从下到上，从左到右 |
| 8 | 纵向扫描：从下到上，从右到左 |

#### 找图找色相似度规则

`sim` 接受 0.1～1.0，按下表离散值增加 RGB 通道容差；其他值增加 0。此参数不表示匹配像素占比。

| sim | 每个 RGB 通道增加的容差（十进制） |
|---|---|
| 0.5 | 49 |
| 0.6 | 38 |
| 0.7 | 29 |
| 0.8 | 18 |
| 0.9 | 9 |
| 其他合法值（含 1.0） | 0 |

额外容差与颜色偏色相加。找图对所有有效、非透明模板像素逐通道比较；没有模板缩放功能。

### 模板加载与管理

<a id="SetTemplateCacheLimit"></a>

#### SetTemplateCacheLimit

**函数简介：**

设置模板缓存的字节预算。CPU 缓存与每个 GPU 设备缓存分别使用此预算，互不合并计算。

**函数原型：**

```cpp
int WINAPI SetTemplateCacheLimit(uint64_t bytes);
```

**参数定义：**

- bytes：大于 0 的字节数；默认值为 268435456（256 MiB）。

**返回值：**

- 1：设置成功。减小预算会淘汰普通 CPU 缓存及 GPU 最近较少使用的模板；正在执行的查找结束后释放其仍在使用的资源。
- 0：预算为 0、数值超出平台范围，或新预算小于当前显式预加载模板的 CPU 占用；失败时保留原预算。

**示例：**

```text
SetTemplateCacheLimit(536870912)
```

**注意：** 调高预算只允许缓存继续增长，不会自动加载模板。模板数量没有固定上限，仍受字节预算和可用内存/显存约束。CPU 与每个 GPU 设备各自最多使用所配置的预算，不代表整个进程的合计用量。

<a id="LoadPic"></a>

#### LoadPic

**函数简介：**

预先加载 BMP24、PNG、JPG/JPEG 文件模板到内存，供后续找图直接复用。

**函数原型：**

```cpp
int WINAPI LoadPic(const char* pic_name);
```

**参数定义：**

- pic_name：模板名称，支持 | 列表及文件名中的 *、? 通配符；目录部分不支持通配符。

**返回值：**

- 1：全部指定模板加载成功。
- 0：至少有一项失败；此前已成功加载的项仍保留。

**示例：**

```text
LoadPic("test.bmp|test1.bmp")
```

**注意：** 无需绑定。支持 BMP24、PNG、JPG/JPEG 文件；显式缓存不自动淘汰，模板数量按需增长，受 `SetTemplateCacheLimit` 的 CPU 字节预算约束。加载后不再检查源文件；要更新请 FreePic 后重新加载。GPU 模板首次查找时按需上传，并受各 GPU 设备的同值预算约束。

<a id="LoadPicByte"></a>

#### LoadPicByte

**函数简介：**

从本进程内存加载完整 BMP24 文件数据，并登记为模板别名。

**函数原型：**

```cpp
int WINAPI LoadPicByte(uintptr_t addr, int size, const char* pic_name);
```

**参数定义：**

- addr：本进程内完整 BMP24 文件数据首地址，uintptr_t 为指针宽度整数。
- size：文件总字节数，包含 BMP 文件头，不能超过当前模板缓存预算（默认 256 MiB）。
- pic_name：单个虚拟模板名称，不接受列表和通配符。

**返回值：**

- 1：复制并解码成功。
- 0：地址不可读、内容或大小无效、容量不足等。

**示例：**

```text
LoadPicByte(0x实际BMP内存地址, 实际文件字节数, "memory.bmp")
```

**注意：** 不能传裸 BGRA 像素或其他进程的地址。成功后原内存可立即释放；同名成功加载会替换旧模板，失败保留旧内容。上例占位参数必须替换为宿主进程提供的实际值。

<a id="FreePic"></a>

#### FreePic

**函数简介：**

释放对应 CPU 模板及 GPU 模板缓存。

**函数原型：**

```cpp
int WINAPI FreePic(const char* pic_name);
```

**参数定义：**

- pic_name：模板名称，支持 | 列表及文件名通配符，可匹配内存别名和已删除文件的缓存。

**返回值：**

- 1：释放完成；合法名称没有对应缓存时也返回 1。
- 0：参数无效或释放失败。

**示例：**

```text
FreePic("test.bmp|memory.bmp")
```

**注意：** 不删除磁盘文件。正在查找的旧模板在该次使用结束后释放；普通文件下次 FindPic 会自动加载，内存别名必须重新 LoadPicByte。

<a id="MatchPicName"></a>

#### MatchPicName

**函数简介：** 根据文件名和通配符获取 BMP、PNG、JPG/JPEG 文件集合，结果可直接传给 `FindPic/FindPicEx` 等。

**函数原型：**

```cpp
const char* WINAPI MatchPicName(const char* pic_name);
```

**参数定义：** `pic_name` 为 UTF-8 文件名或路径，支持 `|` 列表、文件名中的 `*`、`?`。
相对路径按 `SetPath` 解析，未设定时按进程当前目录。通配符不能用于目录部分，不递归子目录。

**返回值：** 匹配到的 BMP、PNG、JPG/JPEG 文件名以 `|` 分隔，无匹配或错误返回空字符串。
不包含目录、其他扩展名、磁盘上不存在的文件或仅由 `LoadPicByte` 加载的虚拟模板。
每组匹配按文件名排序，整个列表按输入组顺序拼接，重复文件只保留第一次。
返回名称保留输入的相对目录或绝对路径形式；字符串属于 DLL，后续调用不会覆盖；使用完后调用 FreeAllResults 或 FreeAllStringResults 全量释放。

**示例：**

```text
SetPath("C:\Users\Administrator\Documents\DaMo\FsDm")
MatchPicName("abc???.bmp|1.bmp|aa??.bmp")
```

假设目录内有 `abc001.bmp`、`abc002.bmp`，`MatchPicName("abc*.bmp")` 返回
`abc001.bmp|abc002.bmp`。

**注意：** 只枚举 BMP、PNG、JPG/JPEG 扩展名（不区分大小写），不解码验证文件内容；
BMP 找图模板仍要求有效 BMP24，PNG/JPG/JPEG 必须能由系统图像解码器解码。无需绑定窗口，不预加载模板，不创建 GPU 设备。
工作台显示文件枚举和其他耗时。

### 图片匹配

<a id="CvFindPic"></a>
### OpenCV 图片匹配

```c
const FsTongyongJieguo* WINAPI CvFindPic(int x1, int y1, int x2, int y2, const char* pic_name, double sim, int dir);
const FsTongyongJieguo* WINAPI CvFindPicEx(int x1, int y1, int x2, int y2, const char* pic_name, double sim, int dir);
const FsTongyongJieguo* WINAPI CvFindPicMem(int x1, int y1, int x2, int y2, const char* pic_name, double sim, int dir);
const FsTongyongJieguo* WINAPI CvFindPicMemEx(int x1, int y1, int x2, int y2, const char* pic_name, double sim, int dir);
```

- `x1, y1, x2, y2` 指定客户区物理像素区域，右下边界不包含。超出客户区的部分会截断。
- `pic_name` 为 UTF-8 模板名称，多个名称以 `|` 分隔，按列表顺序查找。路径遵循 `SetPath`，支持项目现有的模板格式和缓存。
- `sim` 范围为 0.1～1.0。按有效模板像素计算分数 `1 - RGB 各通道平方误差之和 / (有效像素数 × 3 × 255²)`，分数达到阈值即命中。浮点计算使用少量误差容限。
- `dir` 取 0～3，使用现有找图方向规则。普通版本返回首个结果，`Ex` 返回最多 1500 个结果，包含可能重叠的匹配。
- `Mem` 版本先用 `LoadPicByte` 将完整 BMP 文件字节登记为模板别名，再传入该别名。该项目接口的参数和大漠原版的内存地址参数不同。调用 `FreePic` 可释放登记的模板。
- 透明像素沿用项目现有规则，PNG 的 alpha 为 0 的像素不参与比较，其他模板遵循现有四角透明色处理。纯色模板按不透明处理。
- 查找使用 OpenCV CPU 模板匹配，需要读取采集画面到 CPU，独立于 `SetFindBackend` 的选择。
- 检查返回指针、`status` 和 `count`。每条结果的 `id` 是模板列表下标，`x/y` 是命中模板左上角，`file` 是输入的模板名称。结果使用后调用 `FreeResult`。

```c
const FsTongyongJieguo* result = CvFindPic(0, 0, 800, 600, "target.bmp", 0.95, 0);
if (result && result->status == FS_CHENGGONG && result->count > 0) {
    int x = result->items[0].x;
    int y = result->items[0].y;
    /* 使用 x 和 y */
}
FreeResult(result);
```

<a id="AiFindPic"></a>
### AiFindPic 兼容入口

```c
const FsTongyongJieguo* WINAPI AiFindPic(int x1, int y1, int x2, int y2, const char* pic_name, double sim, int dir);
const FsTongyongJieguo* WINAPI AiFindPicEx(int x1, int y1, int x2, int y2, const char* pic_name, double sim, int dir);
const FsTongyongJieguo* WINAPI AiFindPicMem(int x1, int y1, int x2, int y2, const char* pic_info, double sim, int dir);
const FsTongyongJieguo* WINAPI AiFindPicMemEx(int x1, int y1, int x2, int y2, const char* pic_info, double sim, int dir);
```

- 该项目没有原厂 `ai.module` 算法；这四个名字是便于迁移的兼容入口，实际调用现有 OpenCV CPU 模板匹配，不代表算法效果与原插件相同。
- 文件版参数和坐标规则与 `CvFindPic*` 相同。普通版返回首个匹配，`Ex` 版本最多 1500 项。
- Mem 版 `pic_info` 使用 `十进制地址,字节长度|地址,长度` 格式；地址须属于当前进程，数据必须是完整、未加密的 BMP24。使用 `LoadPicByte` 对应的模板缓存规则登记和释放。
- 返回 `FsTongyongJieguo`，使用后调用 `FreeResult`。

### YOLO 目标检测

当前使用 CPU 执行预处理、ONNX 推理及重叠框过滤。模型实际输入为 float32 `[1,3,640,640]`，单输出为 `[1,N,5+类别数]`，候选框格式为中心 x、中心 y、宽、高、目标存在分数及各类别分数。类别维度须能在加载时确定。

完整工作台调用可查阅 CHM 的“完整调用实例 → YOLO 模型与检测”。

<a id="AiYoloSetVersion"></a>

#### AiYoloSetVersion

设置后续 YOLO 检测使用的模型解析格式。当前可传入的版本字符串为 "v5-7.0"。

```cpp
int WINAPI AiYoloSetVersion(const char* version);
```

**参数定义**

- `version`　传入 "v5-7.0"，区分大小写。

**返回值与结果处理**

- 1 表示设置成功。
- 0 表示空指针或版本字符串不受支持。此函数失败时没有更新 GetOcrError 的诊断信息。

**作用范围与调用说明**

- 模型表、版本设置和当前选中编号在当前进程的这一个 ZhaoTu.dll 实例内跨线程共享。多个线程共用当前模型选择，已经取得模型引用的检测可继续使用该次取得的模型。
- 版本设置用于选择输入输出解析规则。模型文件需另行通过 AiYoloSetModel 或 AiYoloSetModelMemory 加载。当前支持的格式见 YOLO 总览中的模型要求。
- 调用顺序为设置版本、加载模型、选择模型、绑定并等待首帧、检测。

完整示例见 CHM 的 `AiYoloSetVersion` 独立页面。

<a id="AiYoloSetModel"></a>

#### AiYoloSetModel

从 ONNX 文件加载并校验模型，建立 CPU 推理会话，将模型登记到指定编号。

```cpp
int WINAPI AiYoloSetModel(int index, const char* file, const char* password);
```

**参数定义**

- `index`　模型编号，整数 0～19。
- `file`　UTF-8 ONNX 文件路径，扩展名为 .onnx。绝对路径直接使用，相对路径按 SetPath 设置的目录解析，未设置时按进程当前目录解析。文件大小大于 0 且不超过 512 MiB。同目录须放置同名 .class 文件，例如 yolov5n.onnx 和 yolov5n.class。
- `password`　传入空字符串 ""。

**返回值与结果处理**

- 1 表示文件、类别表及推理会话校验成功，并登记到指定编号。
- 0 表示失败，立即在同线程调用 GetOcrError() 可读取原因。

**作用范围与调用说明**

- 模型表、版本设置和当前选中编号在当前进程的这一个 ZhaoTu.dll 实例内跨线程共享。多个线程共用当前模型选择，已经取得模型引用的检测可继续使用该次取得的模型。
- 类别文件使用 UTF-8，每个非空行对应一个类别名，首行编号为 0。行首尾空白会去除，空行忽略，类别数量须与模型输出一致。
- 同编号加载成功会替换原模型，加载失败保留原模型。设置模型保持当前选择编号，使用前通过 AiYoloUseModel 明确选择。
- 此函数会创建模型会话，首次加载耗时可能较高。推理依赖 ZhaoTu.dll 同目录中配套的 onnxruntime.dll。

完整示例见 CHM 的 `AiYoloSetModel` 独立页面。

<a id="AiYoloSetModelMemory"></a>

#### AiYoloSetModelMemory

复制调用方内存中的完整 ONNX 文件字节，建立 CPU 推理会话并登记模型。

```cpp
int WINAPI AiYoloSetModelMemory(int index, uintptr_t data, int size, const char* password);
```

**参数定义**

- `index`　模型编号，整数 0～19。
- `data`　当前进程中完整、未加密 ONNX 字节的起始地址。必须非零，并在本次调用期间可读取 size 个字节。
- `size`　字节数量，大于 0 且不超过 512 MiB。
- `password`　传入空字符串 ""。

**返回值与结果处理**

- 1 表示复制、校验及登记成功，函数返回后可释放调用方原始模型字节。
- 0 表示失败，立即在同线程调用 GetOcrError() 读取原因。

**作用范围与调用说明**

- 模型表、版本设置和当前选中编号在当前进程的这一个 ZhaoTu.dll 实例内跨线程共享。多个线程共用当前模型选择，已经取得模型引用的检测可继续使用该次取得的模型。
- 内存版自动使用类别编号的十进制字符串作为类别名，例如 "0"、"1"。该入口仅接收模型字节，类别名由模型输出类别数生成。
- 同编号加载成功会替换原模型，加载失败保留原模型。检测使用前调用 AiYoloUseModel。
- 工作台可以接收数值地址，该地址必须指向工作台所在进程内的有效内存。下方 C++ 示例负责读取文件并传入真实地址。

完整示例见 CHM 的 `AiYoloSetModelMemory` 独立页面。

<a id="AiYoloUseModel"></a>

#### AiYoloUseModel

选择后续 YOLO 检测和类别名称转换使用的模型编号。

```cpp
int WINAPI AiYoloUseModel(int index);
```

**参数定义**

- `index`　已通过文件版或内存版成功登记的模型编号，0～19。

**返回值与结果处理**

- 1 表示选择成功。
- 0 表示编号越界或该编号尚未加载模型。此函数失败时没有更新 GetOcrError 的诊断信息。

**作用范围与调用说明**

- 模型表、版本设置和当前选中编号在当前进程的这一个 ZhaoTu.dll 实例内跨线程共享。多个线程共用当前模型选择，已经取得模型引用的检测可继续使用该次取得的模型。
- 当前选择编号初始为 0，使用其他编号时应显式选择。模型登记和模型选择分别通过 SetModel 与 UseModel 完成。
- AiYoloObjectsToString 使用调用时选中的模型类别表，处理已有检测字符串时应保持对应模型被选中。

完整示例见 CHM 的 `AiYoloUseModel` 独立页面。

<a id="AiYoloFreeModel"></a>

#### AiYoloFreeModel

释放指定编号持有的 YOLO 模型，回收该编号的模型引用。

```cpp
int WINAPI AiYoloFreeModel(int index);
```

**参数定义**

- `index`　模型编号，整数 0～19。

**返回值与结果处理**

- 1 表示编号有效并已清空。该编号原本为空时也返回 1。
- 0 表示编号越界。此函数失败时没有更新 GetOcrError 的诊断信息。

**作用范围与调用说明**

- 模型表、版本设置和当前选中编号在当前进程的这一个 ZhaoTu.dll 实例内跨线程共享。多个线程共用当前模型选择，已经取得模型引用的检测可继续使用该次取得的模型。
- 若释放的是当前选中编号，当前编号会被设置为 0。后续检测须保证编号 0 有模型，或重新加载并选择其他编号。
- 已经取得该模型引用的并发检测可继续完成。模型资源在最后一个使用者释放引用后回收。
- 检测缓存可能暂时保留先前结果和区域像素。释放模型后重新加载会获得新的模型标识，后续检测按新的模型标识匹配缓存。

完整示例见 CHM 的 `AiYoloFreeModel` 独立页面。

<a id="AiYoloDetectObjects"></a>

#### AiYoloDetectObjects

检测已绑定窗口指定区域中的目标，返回类别、置信度和检测框坐标组成的字符串。

```cpp
const char* WINAPI AiYoloDetectObjects(int x1, int y1, int x2, int y2, double prob, double iou);
```

**参数定义**

- `x1、y1`　检测区域左上角，单位为目标窗口客户区物理像素。超出左上边界的部分会裁剪。
- `x2、y2`　检测区域右下角，右边界和下边界不包含。须满足 x2>x1、y2>y1，超出客户区的部分会裁剪，裁剪后区域须非空。
- `prob`　置信度阈值，有限数值 0～1，例如 0.5。置信度由目标存在分数乘以最高类别分数得到，低于阈值的候选框被过滤。提高阈值通常减少返回数量。
- `iou`　同类别重叠框过滤阈值，有限数值 0～1，例如 0.45。IoU 是两框交集面积除以并集面积。按置信度从高到低保留框，与已保留同类框的 IoU 大于阈值时删除该候选框。降低阈值会更积极地过滤重叠框。

**返回值与结果处理**

- 有目标时返回 "类别编号,置信度,x,y,w,h|..."。例如 "0,0.920000,100,80,60,120" 表示类别 0、置信度 0.92、左上角 (100,80)、宽 60、高 120。坐标相对于整个客户区。
- 成功且没有目标时返回空字符串。检测出错也可能返回空字符串，应立即调用 GetOcrError()，成功检测时其内容为空，检测失败时包含原因。
- 结果按置信度从高到低排列，最多返回 1000 个框。
- 返回值为 DLL 分配的独立 UTF-8 字符串，分配失败可能返回 nullptr。使用完后调用 FreeAllStringResults() 或 FreeAllResults()。全量释放会同时回收其他线程持有的字符串和通用结果，调用前须完成所有结果读取。工作台会自动显示并释放本次字符串结果。

**作用范围与调用说明**

- 调用前成功绑定目标窗口、等待首帧，并调用 AiYoloSetVersion、AiYoloSetModel（或内存版）、AiYoloUseModel。当前 ONNX 推理、预处理和重叠框过滤均在 CPU 执行。
- 区域先从共享的完整采集帧中裁剪，再按比例缩放并补边为 640×640，转换为 RGB 浮点输入。结果坐标映射回客户区，包含检测区域的 x1、y1 偏移。
- 三个检测入口共用检测结果缓存，缓存保存已经算出的检测框和对应区域图像。同一绑定、同一帧、同一模型及相同区域、prob、iou 且缓存仍保留时，复用结果并跳过取图、预处理和模型推理。
- SetFrameResultCache(0) 允许复用结果，为默认行为。SetFrameResultCache(1) 强制每次执行分析。此开关在 DLL 内全局共享，同帧像素仍可由帧缓存复用。缓存有容量限制，被淘汰的结果会重新计算。
- SetCaptureFrameInterval(-1) 固定已发布的采集快照，SetCaptureFrameInterval(0) 恢复无间隔采集。固定帧和复用结果分别由采集设置和结果缓存开关控制。

完整示例见 CHM 的 `AiYoloDetectObjects` 独立页面。

<a id="AiYoloDetectObjectsToFile"></a>

#### AiYoloDetectObjectsToFile

检测窗口区域，并将该区域及红色检测框标注保存为 BMP 文件。

```cpp
int WINAPI AiYoloDetectObjectsToFile(int x1, int y1, int x2, int y2, double prob, double iou, const char* file, int mode);
```

**参数定义**

- `x1、y1`　检测区域左上角，单位为目标窗口客户区物理像素。超出左上边界的部分会裁剪。
- `x2、y2`　检测区域右下角，右边界和下边界不包含。须满足 x2>x1、y2>y1，超出客户区的部分会裁剪，裁剪后区域须非空。
- `prob`　置信度阈值，有限数值 0～1，例如 0.5。置信度由目标存在分数乘以最高类别分数得到，低于阈值的候选框被过滤。提高阈值通常减少返回数量。
- `iou`　同类别重叠框过滤阈值，有限数值 0～1，例如 0.45。IoU 是两框交集面积除以并集面积。按置信度从高到低保留框，与已保留同类框的 IoU 大于阈值时删除该候选框。降低阈值会更积极地过滤重叠框。
- `file`　UTF-8 输出路径，绝对路径直接使用，相对路径按 SetPath 解析，未设置时按进程当前目录解析。父目录须已存在，同名文件会被覆盖。文件内容为 24 位 BMP。
- `mode`　0 表示标注类别编号和置信度，1 表示仅标注类别编号。

**返回值与结果处理**

- 1 表示检测及文件写入成功。没有目标时也保存区域图片并返回 1。
- 0 表示失败，立即在同线程调用 GetOcrError() 读取原因。

**作用范围与调用说明**

- 调用前成功绑定目标窗口、等待首帧，并调用 AiYoloSetVersion、AiYoloSetModel（或内存版）、AiYoloUseModel。当前 ONNX 推理、预处理和重叠框过滤均在 CPU 执行。
- 区域先从共享的完整采集帧中裁剪，再按比例缩放并补边为 640×640，转换为 RGB 浮点输入。结果坐标映射回客户区，包含检测区域的 x1、y1 偏移。
- 三个检测入口共用检测结果缓存，缓存保存已经算出的检测框和对应区域图像。同一绑定、同一帧、同一模型及相同区域、prob、iou 且缓存仍保留时，复用结果并跳过取图、预处理和模型推理。
- SetFrameResultCache(0) 允许复用结果，为默认行为。SetFrameResultCache(1) 强制每次执行分析。此开关在 DLL 内全局共享，同帧像素仍可由帧缓存复用。缓存有容量限制，被淘汰的结果会重新计算。
- SetCaptureFrameInterval(-1) 固定已发布的采集快照，SetCaptureFrameInterval(0) 恢复无间隔采集。固定帧和复用结果分别由采集设置和结果缓存开关控制。
- 保存图片的内容为裁剪后的检测区域，尺寸为该区域映射到采集帧后的像素尺寸。复用检测结果时仍执行绘制、编码和文件写入。

完整示例见 CHM 的 `AiYoloDetectObjectsToFile` 独立页面。

<a id="AiYoloDetectObjectsToDataBmp"></a>

#### AiYoloDetectObjectsToDataBmp

检测窗口区域，将带检测框标注的完整 BMP 文件字节写入调用方缓冲区。

```cpp
int WINAPI AiYoloDetectObjectsToDataBmp(int x1, int y1, int x2, int y2, double prob, double iou, void* data, uint32_t* size, int mode);
```

**参数定义**

- `x1、y1`　检测区域左上角，单位为目标窗口客户区物理像素。超出左上边界的部分会裁剪。
- `x2、y2`　检测区域右下角，右边界和下边界不包含。须满足 x2>x1、y2>y1，超出客户区的部分会裁剪，裁剪后区域须非空。
- `prob`　置信度阈值，有限数值 0～1，例如 0.5。置信度由目标存在分数乘以最高类别分数得到，低于阈值的候选框被过滤。提高阈值通常减少返回数量。
- `iou`　同类别重叠框过滤阈值，有限数值 0～1，例如 0.45。IoU 是两框交集面积除以并集面积。按置信度从高到低保留框，与已保留同类框的 IoU 大于阈值时删除该候选框。降低阈值会更积极地过滤重叠框。
- `data`　调用方可写缓冲区地址。可传 nullptr 查询所需容量。
- `size`　有效的 uint32_t 指针。调用前写入 data 可用容量，单位为字节。完成 BMP 编码后写回所需字节数。
- `mode`　0 表示标注类别编号和置信度，1 表示仅标注类别编号。

**返回值与结果处理**

- 1 表示完整 BMP 字节已写入 data，*size 为实际大小。没有目标时也可成功返回区域图片。
- 0 表示容量不足、data 为空或发生错误。查询容量成功时也返回 0，同时 *size 大于 0。其他失败应立即读取 GetOcrError()，并结合 *size 判断。

**作用范围与调用说明**

- 调用前成功绑定目标窗口、等待首帧，并调用 AiYoloSetVersion、AiYoloSetModel（或内存版）、AiYoloUseModel。当前 ONNX 推理、预处理和重叠框过滤均在 CPU 执行。
- 区域先从共享的完整采集帧中裁剪，再按比例缩放并补边为 640×640，转换为 RGB 浮点输入。结果坐标映射回客户区，包含检测区域的 x1、y1 偏移。
- 三个检测入口共用检测结果缓存，缓存保存已经算出的检测框和对应区域图像。同一绑定、同一帧、同一模型及相同区域、prob、iou 且缓存仍保留时，复用结果并跳过取图、预处理和模型推理。
- SetFrameResultCache(0) 允许复用结果，为默认行为。SetFrameResultCache(1) 强制每次执行分析。此开关在 DLL 内全局共享，同帧像素仍可由帧缓存复用。缓存有容量限制，被淘汰的结果会重新计算。
- SetCaptureFrameInterval(-1) 固定已发布的采集快照，SetCaptureFrameInterval(0) 恢复无间隔采集。固定帧和复用结果分别由采集设置和结果缓存开关控制。
- 输出为 24 位 BMP 文件字节，包含文件头。缓冲区由调用方分配和释放。该接口带原始指针输出参数，工作台无法直接调用，请通过 C/C++ 测试。
- 查询容量也会执行检测及 BMP 编码。两次调用之间若帧或尺寸变化，第二次所需容量可能变化，应根据写回值扩容重试。复用检测结果时仍执行绘制和 BMP 编码。

完整示例见 CHM 的 `AiYoloDetectObjectsToDataBmp` 独立页面。

<a id="AiYoloSortsObjects"></a>

#### AiYoloSortsObjects

对检测字符串中的目标按分行规则和横坐标重新排序，保留每个目标原有字段。

```cpp
const char* WINAPI AiYoloSortsObjects(const char* objects, int height);
```

**参数定义**

- `objects`　检测字符串，格式为 "类别编号,置信度,x,y,w,h|..."，可以传入 AiYoloDetectObjects 的结果。空字符串表示空列表。
- `height`　非负整数，单位为客户区像素。大于 0 时，先按 y、x 排序，以当前行首个目标的 y 为基准，后续目标与基准 y 的差达到 height 时另起一行，各行内部按 x 升序排列。传 0 时将全部目标作为一行，最终按 x 升序排列。

**返回值与结果处理**

- 成功时返回重新排序后的独立字符串，各项字段保留原始文本。
- 输入为空列表或解析失败时返回空字符串，分配失败可能返回 nullptr。此函数没有更新 GetOcrError 的诊断信息。
- 返回值为 DLL 分配的独立 UTF-8 字符串，分配失败可能返回 nullptr。使用完后调用 FreeAllStringResults() 或 FreeAllResults()。全量释放会同时回收其他线程持有的字符串和通用结果，调用前须完成所有结果读取。工作台会自动显示并释放本次字符串结果。

**作用范围与调用说明**

- 这是检测结果的排序接口，可直接处理已取得的字符串。
- height=0 的实际结果为全部目标按 x 排序。同一 x 的条目保持前面 y、x 预排序产生的先后顺序。
- 输入末尾须紧跟最后一个目标字段，尾部多余的 | 会使解析失败。

完整示例见 CHM 的 `AiYoloSortsObjects` 独立页面。

<a id="AiYoloObjectsToString"></a>

#### AiYoloObjectsToString

把检测字符串中的类别编号映射为当前选中模型的类别名，返回以 | 分隔的名称列表。

```cpp
const char* WINAPI AiYoloObjectsToString(const char* objects);
```

**参数定义**

- `objects`　检测结果字符串，例如 "0,0.920000,100,80,60,120|2,0.850000,220,90,80,50"。应与当前选中模型的类别表对应。

**返回值与结果处理**

- 成功示例为 "person|car"，名称顺序与输入目标顺序一致，重复类别会重复输出。返回内容为类别名列表。
- 文件模型的名称来自同名 .class，内存模型的名称为数字字符串，例如 "0|2"。
- 空列表或转换失败返回空字符串，分配失败可能返回 nullptr。此函数捕获异常后没有更新 GetOcrError 的诊断信息。
- 返回值为 DLL 分配的独立 UTF-8 字符串，分配失败可能返回 nullptr。使用完后调用 FreeAllStringResults() 或 FreeAllResults()。全量释放会同时回收其他线程持有的字符串和通用结果，调用前须完成所有结果读取。工作台会自动显示并释放本次字符串结果。

**作用范围与调用说明**

- 模型表、版本设置和当前选中编号在当前进程的这一个 ZhaoTu.dll 实例内跨线程共享。多个线程共用当前模型选择，已经取得模型引用的检测可继续使用该次取得的模型。
- 转换时通过当前模型类别表查找类别编号。加载其他模型、切换编号或释放当前模型可能影响转换结果，应在对应模型仍被选中时完成转换。
- 坐标和置信度可继续从原检测字符串读取，转换结果用于取得名称。

完整示例见 CHM 的 `AiYoloObjectsToString` 独立页面。

<a id="FindPic"></a>

#### FindPic

**函数简介：**

查找 BMP24、PNG 或 JPG/JPEG 文件模板的第一个位置。

**函数原型：**

```cpp
const FsTongyongJieguo* WINAPI FindPic(int x1, int y1, int x2, int y2, const char* pic_name, const char* delta_color, double sim, int dir);
```

**参数定义：**

- x1、y1：区域左上角坐标。
- x2、y2：区域右下角坐标，不包含右下边界；窗口接口使用客户区物理像素。
- pic_name：一个或多个 BMP24、PNG 或 JPG/JPEG 模板名称，用 | 分隔；支持文件路径或已预加载的内存模板别名。
- delta_color：六位十六进制 RGB 通道容差，例如 "203040"；"000000" 表示无额外偏色。
- sim：范围 0.1～1.0，使用本页“找图找色相似度规则”，不是命中像素比例。
- dir：扫描方向，支持 0～3，见方向表。

**返回值：**

- 返回独立的 `FsTongyongJieguo` 指针。`status=0` 正常，`count=0` 无命中；错误为非 0 状态，分配失败为 `nullptr`。
- 条目提供模板索引 id、左上角 x/y、模板名称 file。最多 1 项。
- 条目有效字段标志：`FS_JG_ID | FS_JG_XY | FS_JG_FILE`；用完后通过 DLL 释放。fullText 为空字符串。

**示例：**

```text
FindPic(0, 0, 834, 419, "test.bmp|test1.bmp", "000000", 0.9, 0)
```

**注意：** 按模板列表优先，保留输入列表索引和文件名。单项无法加载时跳过；所有模板均无法加载报 status=9。有效模板没有命中仍为 status=0/count=0。BMP24 四角同色作透明色，全图同色例外；PNG 完全透明像素跳过，半透明像素按 RGB 比较；JPG/JPEG 按不透明图处理。模板不缩放。

<a id="FindPicS"></a>

#### FindPicS

**函数简介：**

查找 BMP24、PNG 或 JPG/JPEG 文件模板的第一个位置。

**函数原型：**

```cpp
const FsTongyongJieguo* WINAPI FindPicS(int x1, int y1, int x2, int y2, const char* pic_name, const char* delta_color, double sim, int dir);
```

**参数定义：**

- x1、y1：区域左上角坐标。
- x2、y2：区域右下角坐标，不包含右下边界；窗口接口使用客户区物理像素。
- pic_name：一个或多个 BMP24、PNG 或 JPG/JPEG 模板名称，用 | 分隔；支持文件路径或已预加载的内存模板别名。
- delta_color：六位十六进制 RGB 通道容差，例如 "203040"；"000000" 表示无额外偏色。
- sim：范围 0.1～1.0，使用本页“找图找色相似度规则”，不是命中像素比例。
- dir：扫描方向，支持 0～3，见方向表。

**返回值：**

- 返回独立的 `FsTongyongJieguo` 指针。`status=0` 正常，`count=0` 无命中；错误为非 0 状态，分配失败为 `nullptr`。
- 条目提供模板索引 id、左上角 x/y、模板名称 file。最多 1 项。
- 条目有效字段标志：`FS_JG_ID | FS_JG_XY | FS_JG_FILE`；用完后通过 DLL 释放。fullText 为空字符串。

**示例：**

```text
FindPicS(0, 0, 834, 419, "test.bmp|test1.bmp", "000000", 0.9, 0)
```

**注意：** 与 FindPic 使用相同的结果结构；通过 items 读取 id、x、y、file。file 为传入的相对名称或完整路径。

<a id="FindPicEx"></a>

#### FindPicEx

**函数简介：**

查找 BMP24、PNG 或 JPG/JPEG 文件模板的全部位置。

**函数原型：**

```cpp
const FsTongyongJieguo* WINAPI FindPicEx(int x1, int y1, int x2, int y2, const char* pic_name, const char* delta_color, double sim, int dir);
```

**参数定义：**

- x1、y1：区域左上角坐标。
- x2、y2：区域右下角坐标，不包含右下边界；窗口接口使用客户区物理像素。
- pic_name：一个或多个 BMP24、PNG 或 JPG/JPEG 模板名称，用 | 分隔；支持文件路径或已预加载的内存模板别名。
- delta_color：六位十六进制 RGB 通道容差，例如 "203040"；"000000" 表示无额外偏色。
- sim：范围 0.1～1.0，使用本页“找图找色相似度规则”，不是命中像素比例。
- dir：扫描方向，支持 0～3，见方向表。

**返回值：**

- 返回独立的 `FsTongyongJieguo` 指针。`status=0` 正常，`count=0` 无命中；错误为非 0 状态，分配失败为 `nullptr`。
- 条目提供模板索引 id、左上角 x/y、模板名称 file。最多 1500 项。
- 条目有效字段标志：`FS_JG_ID | FS_JG_XY | FS_JG_FILE`；用完后通过 DLL 释放。fullText 为空字符串。

**示例：**

```text
FindPicEx(0, 0, 834, 419, "test.bmp|test1.bmp", "000000", 0.9, 0)
```

**注意：** 最多 1500 项，先列第一张模板的所有命中，再列下一张；保留重叠位置和重复模板的命中。

<a id="FindPicExS"></a>

#### FindPicExS

**函数简介：**

查找 BMP24、PNG 或 JPG/JPEG 文件模板的全部位置。

**函数原型：**

```cpp
const FsTongyongJieguo* WINAPI FindPicExS(int x1, int y1, int x2, int y2, const char* pic_name, const char* delta_color, double sim, int dir);
```

**参数定义：**

- x1、y1：区域左上角坐标。
- x2、y2：区域右下角坐标，不包含右下边界；窗口接口使用客户区物理像素。
- pic_name：一个或多个 BMP24、PNG 或 JPG/JPEG 模板名称，用 | 分隔；支持文件路径或已预加载的内存模板别名。
- delta_color：六位十六进制 RGB 通道容差，例如 "203040"；"000000" 表示无额外偏色。
- sim：范围 0.1～1.0，使用本页“找图找色相似度规则”，不是命中像素比例。
- dir：扫描方向，支持 0～3，见方向表。

**返回值：**

- 返回独立的 `FsTongyongJieguo` 指针。`status=0` 正常，`count=0` 无命中；错误为非 0 状态，分配失败为 `nullptr`。
- 条目提供模板索引 id、左上角 x/y、模板名称 file。最多 1500 项。
- 条目有效字段标志：`FS_JG_ID | FS_JG_XY | FS_JG_FILE`；用完后通过 DLL 释放。fullText 为空字符串。

**示例：**

```text
FindPicExS(0, 0, 834, 419, "test.bmp|test1.bmp", "000000", 0.9, 0)
```

**注意：** 与 FindPicEx 使用相同的结果结构；每个结果条目包含 id、x、y、file。

## 三、找色

找色方向及相似度沿用上一节的公共规则，各函数支持的方向以其参数说明为准。

### 取色与比色

<a id="GetColor"></a>

#### GetColor

**函数简介：**

获取指定坐标的 RGB 整数值。

**函数原型：**

```cpp
const FsTongyongJieguo* WINAPI GetColor(int x, int y);
```

**参数定义：**

- x：客户区 X 坐标。
- y：客户区 Y 坐标。

**返回值：**

- 返回独立的 `FsTongyongJieguo` 指针。`status=0` 正常，`count=0` 无命中；错误为非 0 状态，分配失败为 `nullptr`。
- 成功时 count=1；条目提供 x、y、color。
- 条目有效字段标志：`FS_JG_XY | FS_JG_COLOR`；用完后通过 DLL 释放。fullText 为空字符串。

**示例：**

```text
GetColor(30, 30)
```

**注意：** 负坐标归零，正向越界为参数错误。color 是整数 RGB，例如 0x12f3f3，黑色 0x000000 也是有效值。

<a id="CmpColor"></a>

#### CmpColor

**函数简介：**

比较指定坐标与颜色列表。

**函数原型：**

```cpp
const FsTongyongJieguo* WINAPI CmpColor(int x, int y, const char* color, double sim);
```

**参数定义：**

- `x/y`：客户区坐标。负坐标归零，正向越界失败，和当前 `GetColor` 一致。
- `color`：最多 10 组 `RRGGBB[-DRDGDB]`，用 `|` 分隔，十六进制大小写均可；不支持 HSV 或反色。
- `sim`：范围 0.1～1.0，使用本文的离散通道容差规则。

**返回值：**

- 返回独立的 `FsTongyongJieguo` 指针。`status=0` 正常，`count=0` 无命中；错误为非 0 状态，分配失败为 `nullptr`。
- 匹配时 count=1，不匹配时 count=0。
- 条目有效字段标志：`FS_JG_ID | FS_JG_XY | FS_JG_COLOR`；用完后通过 DLL 释放。fullText 为空字符串。

**示例：**

```text
CmpColor(200, 300, "000000-000000|ff00ff-101010", 0.9)
```

**注意：** 最多 10 组颜色；命中条目的 id 为匹配颜色在列表中的索引，color 是实际像素。未匹配时 status=0/count=0。

### 单点与多点找色

<a id="FindColor"></a>

#### FindColor

**函数简介：**

查找指定区域的颜色。

**函数原型：**

```cpp
const FsTongyongJieguo* WINAPI FindColor(int x1, int y1, int x2, int y2, const char* color, double sim, int dir);
```

**参数定义：**

- x1、y1：搜索区域左上角客户区坐标。
- x2、y2：搜索区域右下角客户区坐标，包含右下边界，超出客户区的部分会裁剪。
- color：RGB 颜色串，格式为 RRGGBB[-DRDGDB]，多个颜色用 | 分隔，例如 "123456-000000|ffffff-020202"。
- sim：范围 0.1～1.0，使用本页“找图找色相似度规则”，不是命中像素比例。
- dir：支持 0～8，含 4 中心向外。

**返回值：**

- 返回独立的 `FsTongyongJieguo` 指针。`status=0` 正常，`count=0` 无命中；错误为非 0 状态，分配失败为 `nullptr`。
- 条目提供颜色索引 id 和客户区坐标 x/y。最多 1 项。
- 条目有效字段标志：`FS_JG_ID | FS_JG_XY`；用完后通过 DLL 释放。fullText 为空字符串。

**示例：**

```text
FindColor(0, 0, 833, 418, "ffffff-000000|123456-020202", 1.0, 0)
```

**注意：** 先完整搜索第一种颜色，再搜索下一种；id 为颜色列表索引。支持方向 0～8。

<a id="FindColorEx"></a>

#### FindColorEx

**函数简介：**

查找指定区域的颜色。

**函数原型：**

```cpp
const FsTongyongJieguo* WINAPI FindColorEx(int x1, int y1, int x2, int y2, const char* color, double sim, int dir);
```

**参数定义：**

- x1、y1：搜索区域左上角客户区坐标。
- x2、y2：搜索区域右下角客户区坐标，包含右下边界，超出客户区的部分会裁剪。
- color：RGB 颜色串，格式为 RRGGBB[-DRDGDB]，多个颜色用 | 分隔，例如 "123456-000000|ffffff-020202"。
- sim：范围 0.1～1.0，使用本页“找图找色相似度规则”，不是命中像素比例。
- dir：支持 0～3、5～8，不支持 4。

**返回值：**

- 返回独立的 `FsTongyongJieguo` 指针。`status=0` 正常，`count=0` 无命中；错误为非 0 状态，分配失败为 `nullptr`。
- 条目提供颜色索引 id 和客户区坐标 x/y。
- 条目有效字段标志：`FS_JG_ID | FS_JG_XY`；用完后通过 DLL 释放。fullText 为空字符串。

**示例：**

```text
FindColorEx(0, 0, 833, 418, "ffffff-000000|123456-020202", 1.0, 0)
```

**注意：** 按颜色列表优先，支持方向 0～3、5～8；对每种颜色返回搜索区域内的全部命中，不设固定条数或 1024 字符上限。多个颜色都匹配同一坐标时，会按各自的颜色索引分别返回该坐标。结果数量受命中点数量和可用内存限制；通过 count 和 items 读取。

<a id="FindMultiColor"></a>

#### FindMultiColor

**函数简介：**

查找颜色组合的首点。

**函数原型：**

```cpp
const FsTongyongJieguo* WINAPI FindMultiColor(int x1, int y1, int x2, int y2, const char* first_color, const char* offset_color, double sim, int dir);
```

**参数定义：**

- x1、y1：搜索区域左上角客户区坐标。
- x2、y2：搜索区域右下角客户区坐标，包含右下边界，超出客户区的部分会裁剪。
- first_color：单个首点 RGB 颜色，格式 RRGGBB[-DRDGDB]，不支持首色列表。
- offset_color：相对于首点的偏移颜色，格式 dx|dy|颜色,dx|dy|颜色；至少一个偏移点。颜色可带偏色，颜色前的 - 表示不在指定颜色容差范围内。
- sim：范围 0.1～1.0，使用本页“找图找色相似度规则”，不是命中像素比例。
- dir：扫描方向，支持 0～3。

**返回值：**

- 返回独立的 `FsTongyongJieguo` 指针。`status=0` 正常，`count=0` 无命中；错误为非 0 状态，分配失败为 `nullptr`。
- 条目提供颜色索引 id 和客户区坐标 x/y。多点每项代表一整组匹配的首点。最多 1 项。
- 条目有效字段标志：`FS_JG_ID | FS_JG_XY`；用完后通过 DLL 释放。fullText 为空字符串。

**示例：**

```text
FindMultiColor(0, 0, 833, 418, "cc805b-020202", "9|2|-00ff00,15|2|2dff1c-010101", 1.0, 0)
```

**注意：** 首色和所有偏移点必须同时符合，并位于裁剪后的搜索区域。每个条目代表整组匹配的首点，偏移点不会分别计为结果；id 为 0。负偏移与反色可组合，反色不是求 RGB 补色。

<a id="FindMultiColorEx"></a>

#### FindMultiColorEx

**函数简介：**

查找颜色组合的首点。

**函数原型：**

```cpp
const FsTongyongJieguo* WINAPI FindMultiColorEx(int x1, int y1, int x2, int y2, const char* first_color, const char* offset_color, double sim, int dir);
```

**参数定义：**

- x1、y1：搜索区域左上角客户区坐标。
- x2、y2：搜索区域右下角客户区坐标，包含右下边界，超出客户区的部分会裁剪。
- first_color：单个首点 RGB 颜色，格式 RRGGBB[-DRDGDB]，不支持首色列表。
- offset_color：相对于首点的偏移颜色，格式 dx|dy|颜色,dx|dy|颜色；至少一个偏移点。颜色可带偏色，颜色前的 - 表示不在指定颜色容差范围内。
- sim：范围 0.1～1.0，使用本页“找图找色相似度规则”，不是命中像素比例。
- dir：扫描方向，支持 0～3。

**返回值：**

- 返回独立的 `FsTongyongJieguo` 指针。`status=0` 正常，`count=0` 无命中；错误为非 0 状态，分配失败为 `nullptr`。
- 条目提供颜色索引 id 和客户区坐标 x/y。多点每项代表一整组匹配的首点。
- 条目有效字段标志：`FS_JG_ID | FS_JG_XY`；用完后通过 DLL 释放。fullText 为空字符串。

**示例：**

```text
FindMultiColorEx(0, 0, 833, 418, "cc805b-020202", "9|2|-00ff00,15|2|2dff1c-010101", 1.0, 0)
```

**注意：** 最多 1500 项，每项是整组匹配的首点；找到三组图案就是 count=3，而不是返回每组中的采样点。id 均为 0，坐标为客户区物理像素。

## 四、OCR 识别

### OCR 模型、字库与颜色规则

- 0 号默认资源在 `ZhaoTu.dll` 同目录的 `ocr_models`，使用 PP-OCRv5 mobile 检测/识别模型及其 `inference.yml`。
- `SetOcrModel` 的 PP-OCRv3 目录布局使用中文模型；同目录的英文专用模型不会自动选用。需要指定其他模型文件时使用 `SetOcrModelFiles`，但模型仍须符合本项目支持的输入输出及前后处理约定；不是任意 ONNX 模型都兼容。
- PP-OCRv5 面向水平正向文字；PP-OCRv3 使用分类模型纠正置信度不低于 0.9 的 180 度倒置文字。模型字符坐标依据 CTC 预测位置估计，不是精确字形边框。CPU 与 DirectML 均先读取 CPU BGRA、预处理；DirectML 加速模型推理。
- 字库为未加密 TSPlug 十六进制点阵文本：`十六进制$文字$...$实际高度`，每列固定 11 位，顶部为最高位；高度字段支持 1～255。识别时每个字形最多按 11 行点阵匹配，因此高度字段大于 11 的条目可加载，但会按 11 行处理，不保证与原 TSPlug 的识别结果一致。文件内容支持 UTF-8/GB18030；不支持大漠加密字库。
- OCR 颜色支持 `RRGGBB-DRDGDB`、`H.S.V-dH.dS.dV`、`#灰度-容差`，最多 10 种，以 `|` 分隔；HSV 为 H=0～360、S/V=0～100，灰度为十进制 0～255。
- `b@` 前缀表示背景颜色，匹配之外的像素视为文字。空颜色：模型保留原图；字库采用自动阈值及边缘背景推断。
- OCR 的 `sim` 是模型整行平均识别置信度或字形相似度阈值，不使用找图找色离散容差表。

### 模型与引擎设置

<a id="SetOcrModel"></a>

#### SetOcrModel

**函数简介：**

登记一个 OCR 模型目录并校验必要资源。

**函数原型：**

```cpp
int WINAPI SetOcrModel(int index, const char* model_dir);
```

**参数定义：**

- index：模型编号，0～99。
- model_dir：UTF-8 模型目录；须包含检测、识别模型及其配置，目录结构见本页模型说明。

**返回值：**

- 1：登记成功。
- 0：编号、路径、配置或模型资源无效。

**示例：**

```text
SetOcrModel(1, "C:\模型目录")
```

**注意：** 登记不自动选中，随后 UseOcrModel(1)。实际推理会话在首次识别时创建；相同规范目录重复登记复用会话。不是任意 ONNX 模型都兼容。

PP-OCRv3 中文模型示例：

```text
SetOcrModel(1, "C:\Users\Administrator\Documents\DaMo\ocr_model\models")
UseOcrModel(1)
SetOcrEngine("model")
SetOcrBackend("cpu")
Ocr(0, 0, 834, 419, "", 0.5)
```

`models10` 目录也可作为 `model_dir`。v3 会在首次 OCR 时创建检测、识别和方向分类三个 ONNX 会话；英文专用识别模型当前不参与推理。

<a id="SetOcrModelFiles"></a>

#### SetOcrModelFiles

**函数简介：**

按文件路径登记一组 PP-OCRv3 兼容的检测、识别、可选方向分类模型和字符字典。

**函数原型：**

```cpp
int WINAPI SetOcrModelFiles(int index, const char* classifier_path, const char* detector_path,
    const char* recognizer_path, const char* dictionary_path);
```

**参数定义：**

- index：模型编号，0～99。
- classifier_path：方向分类 ONNX 文件路径；可传空字符串，不使用方向分类。
- detector_path：文字检测 ONNX 文件路径。
- recognizer_path：文字识别 ONNX 文件路径。
- dictionary_path：识别字符字典文本路径，按 UTF-8 每行一个字符读取。

**返回值：**

- 1：文件存在且模型配置登记成功。
- 0：参数、文件路径或字符字典无效。

**示例：**

```text
SetOcrModelFiles(1, "C:\模型\cls.onnx", "C:\模型\detEN.onnx", "C:\模型\RecEN.onnx", "C:\模型\KeysEN.txt")
UseOcrModel(1)
SetOcrEngine("model")
```

**注意：** 此函数不按文件名推断语言。字符字典必须与识别模型配套。登记阶段检查文件并读取字典；输入形状、方向分类器结构及识别输出类别数在首次 OCR 创建会话时校验。仅支持符合本项目 PP-OCRv3 前后处理约定的模型，不代表任意 ONNX 文件均兼容。

<a id="UseOcrModel"></a>

#### UseOcrModel

**函数简介：**

选择后续模型 OCR 使用的模型编号。

**函数原型：**

```cpp
int WINAPI UseOcrModel(int index);
```

**参数定义：**

- index：已登记的模型编号，0～99。

**返回值：**

- 1：选择成功。
- 0：编号无效、未登记或默认模型登记失败。

**示例：**

```text
UseOcrModel(0)
```

**注意：** 默认选择 0，其资源来自 ZhaoTu.dll 同目录的 ocr_models。选择模型不会把 dict 引擎切换为 model。

<a id="FreeOcrModel"></a>

#### FreeOcrModel

**函数简介：**

移除指定模型登记并释放其会话资源。

**函数原型：**

```cpp
int WINAPI FreeOcrModel(int index);
```

**参数定义：**

- index：模型编号，0～99。

**返回值：**

- 1：清理成功；有效但未登记的编号也可清理。
- 0：编号无效等。

**示例：**

```text
FreeOcrModel(1)
```

**注意：** 正在执行的识别保留引用，可安全完成。清理后使用该编号需重新 SetOcrModel；清理 0 号也不会再次自动登记默认模型。共用 DirectML 设备保持复用。

<a id="SetOcrBackend"></a>

#### SetOcrBackend

**函数简介：**

选择模型推理在 CPU 或 DirectML 上运行。

**函数原型：**

```cpp
int WINAPI SetOcrBackend(const char* backend);
```

**参数定义：**

- backend："cpu" 或 "directml"，区分大小写。

**返回值：**

- 1：设置成功。
- 0：参数错误。

**示例：**

```text
SetOcrBackend("directml")
```

**注意：** 默认 cpu。设置成功不代表推理设备已初始化；不支持的 DirectML 设备在实际识别时明确失败，不自动退回 CPU。两种方式都先读取 CPU BGRA 并在 CPU 预处理；dict 引擎不因此变为 GPU 点阵识别。

<a id="SetOcrEngine"></a>

#### SetOcrEngine

**函数简介：**

选择神经网络模型 OCR 或点阵字库识别。

**函数原型：**

```cpp
int WINAPI SetOcrEngine(const char* engine);
```

**参数定义：**

- engine："model" 表示模型；"dict" 表示点阵字库。

**返回值：**

- 1：设置成功。
- 0：参数错误。

**示例：**

```text
SetOcrEngine("model")
```

**注意：** 默认 model。切换引擎不自动加载字库，dict 使用前须 SetDict/UseDict。模型自身字符表与点阵字库是两套资源。

### 点阵字库管理

<a id="SetDict"></a>

#### SetDict

**函数简介：**

加载指定点阵字库，并将其设为当前字库。

**函数原型：**

```cpp
int WINAPI SetDict(int index, const char* file);
```

**参数定义：**

- index：字库编号，0～99。
- file：UTF-8 路径；字库文件内容支持 UTF-8 或 GB18030，格式要求见本页字库说明。

**返回值：**

- 1：加载并选中成功。
- 0：编号、路径、编码、格式或容量不合法；替换失败保留旧字库。

**示例：**

```text
SetDict(0, "C:\字库目录\ziku.txt")
```

**注意：** 不会自动切换到 dict 引擎，需要 SetOcrEngine("dict")。不支持大漠加密字库的通用解密。

<a id="UseDict"></a>

#### UseDict

**函数简介：**

选择已加载的点阵字库。

**函数原型：**

```cpp
int WINAPI UseDict(int index);
```

**参数定义：**

- index：已加载字库编号，0～99。

**返回值：**

- 1：选择成功。
- 0：编号无效或字库未加载。

**示例：**

```text
UseDict(0)
```

**注意：** 选择字库不改变 model/dict 引擎。

<a id="GetNowDict"></a>

#### GetNowDict

**函数简介：**

获取当前选择的点阵字库编号。

**函数原型：**

```cpp
int WINAPI GetNowDict(void);
```

**参数定义：**

- 无。

**返回值：**

- 当前选择编号，范围 0～99；这是编号，不是成功或失败标志。

**示例：**

```text
GetNowDict()
```

**注意：** 默认编号为 0。返回编号不保证该字库已经加载，清空字库不会自动改选其他编号。

<a id="GetDictCount"></a>

#### GetDictCount

**函数简介：**

获取指定字库中已加载的字形条目数量。

**函数原型：**

```cpp
int WINAPI GetDictCount(int index);
```

**参数定义：**

- index：字库编号，0～99。

**返回值：**

- 大于或等于 0：字形条目数；未加载时为 0。
- 编号无效也返回 0，可通过 GetOcrError 区分。

**示例：**

```text
GetDictCount(0)
```

**注意：** 计数字形条目，不一定等于去重后的字符数量。当前单个字库最多 10000 个字形。

<a id="ClearDict"></a>

#### ClearDict

**函数简介：**

清理指定编号的内存字库。

**函数原型：**

```cpp
int WINAPI ClearDict(int index);
```

**参数定义：**

- index：字库编号，0～99。

**返回值：**

- 1：清理成功；有效但未加载的编号也成功。
- 0：编号无效等。

**示例：**

```text
ClearDict(0)
```

**注意：** 不删除磁盘文件，不改变当前字库编号。要继续使用需重新 SetDict。

### 文字识别

<a id="Ocr"></a>

#### Ocr

**函数简介：**

识别已绑定窗口中指定区域的文字，通过 fullText 返回全文，通过 items 返回逐字符坐标。当前 Ocr、OcrEx、OcrExOne 使用相同的识别流程，返回内容一致。

**函数原型：**

```cpp
const FsTongyongJieguo* WINAPI Ocr(int x1, int y1, int x2, int y2, const char* color_format, double sim);
```

**参数定义：**

- x1、y1：区域左上角坐标。
- x2、y2：区域右下角坐标，不包含右下边界；窗口接口使用客户区物理像素。
- color_format：文字颜色过滤规则，支持 RGB、HSV、灰度、多个颜色及 b@ 背景模式；"" 表示自动识别。
- sim：范围 0.1～1.0；model 引擎为整行平均识别置信度阈值，dict 引擎为字形匹配阈值。

**返回值：**

- 返回独立的 `FsTongyongJieguo` 指针。`status=0` 正常，`count=0` 无命中；错误为非 0 状态，分配失败为 `nullptr`。
- fullText 提供全文；items 按识别顺序提供字符序号 id、坐标 x/y 和字符 text。
- 条目有效字段标志：`FS_JG_ID | FS_JG_XY | FS_JG_TEXT`；用完后通过 DLL 释放。

**示例：**

```text
Ocr(0, 0, 834, 419, "", 0.5)
```

**注意：** fullText 是全文；color_format 可追加 ,分行符，例如 ",#"。每个字符同时提供条目，模型坐标为估计值，当前面向水平正向文字。

<a id="OcrEx"></a>

#### OcrEx

**函数简介：**

识别已绑定窗口中指定区域的文字。当前与 Ocr 使用相同的识别流程和参数，通过 fullText 返回全文，通过 items 返回逐字符坐标。

**函数原型：**

```cpp
const FsTongyongJieguo* WINAPI OcrEx(int x1, int y1, int x2, int y2, const char* color_format, double sim);
```

**参数定义：**

- x1、y1：区域左上角坐标。
- x2、y2：区域右下角坐标，不包含右下边界；窗口接口使用客户区物理像素。
- color_format：文字颜色过滤规则，支持 RGB、HSV、灰度、多个颜色及 b@ 背景模式；"" 表示自动识别。
- sim：范围 0.1～1.0；model 引擎为整行平均识别置信度阈值，dict 引擎为字形匹配阈值。

**返回值：**

- 返回独立的 `FsTongyongJieguo` 指针。`status=0` 正常，`count=0` 无命中；错误为非 0 状态，分配失败为 `nullptr`。
- fullText 提供全文；items 按识别顺序提供字符序号 id、坐标 x/y 和字符 text。
- 条目有效字段标志：`FS_JG_ID | FS_JG_XY | FS_JG_TEXT`；用完后通过 DLL 释放。

**示例：**

```text
OcrEx(0, 0, 834, 419, "", 0.5)
```

**注意：** 与 Ocr 返回布局一致；通过 fullText 读取全文，通过 items 读取字符及坐标。窗口字符坐标为客户区物理像素，模型坐标由 CTC 时间步估计。

<a id="OcrExOne"></a>

#### OcrExOne

**函数简介：**

识别已绑定窗口中指定区域的文字。当前与 Ocr、OcrEx 的识别流程和返回内容一致，fullText 保存全文，items 保存全部字符及坐标，可返回多行文字。

**函数原型：**

```cpp
const FsTongyongJieguo* WINAPI OcrExOne(int x1, int y1, int x2, int y2, const char* color_format, double sim);
```

**参数定义：**

- x1、y1：区域左上角坐标。
- x2、y2：区域右下角坐标，不包含右下边界；窗口接口使用客户区物理像素。
- color_format：文字颜色过滤规则，支持 RGB、HSV、灰度、多个颜色及 b@ 背景模式；"" 表示自动识别。
- sim：范围 0.1～1.0；model 引擎为整行平均识别置信度阈值，dict 引擎为字形匹配阈值。

**返回值：**

- 返回独立的 `FsTongyongJieguo` 指针。`status=0` 正常，`count=0` 无命中；错误为非 0 状态，分配失败为 `nullptr`。
- fullText 提供全文；items 按识别顺序提供字符序号 id、坐标 x/y 和字符 text。
- 条目有效字段标志：`FS_JG_ID | FS_JG_XY | FS_JG_TEXT`；用完后通过 DLL 释放。

**示例：**

```text
OcrExOne(0, 0, 834, 419, "", 0.5)
```

**注意：** 与 Ocr 返回布局一致，fullText 与字符数组分开保存。分行符只进入全文，不单独生成字符坐标。

<a id="OcrInFile"></a>

#### OcrInFile

**函数简介：**

读取 pic_name 指定的图片文件，识别图片中指定区域的文字，无需绑定窗口。通过 fullText 返回全文，通过 items 返回逐字符坐标，坐标相对于原图片左上角。

**函数原型：**

```cpp
const FsTongyongJieguo* WINAPI OcrInFile(int x1, int y1, int x2, int y2, const char* pic_name, const char* color_format, double sim);
```

**参数定义：**

- x1、y1：图片区域左上角像素坐标。
- x2、y2：图片区域右下角像素坐标，不包含右下边界。
- pic_name：图片文件路径，按 SetPath 解析相对路径。
- color_format：文字过滤规则，同 Ocr。
- sim：识别阈值，同 Ocr。

**返回值：**

- 返回独立的 `FsTongyongJieguo` 指针。`status=0` 正常，`count=0` 无命中；错误为非 0 状态，分配失败为 `nullptr`。
- fullText 提供全文；items 按识别顺序提供字符序号 id、坐标 x/y 和字符 text。
- 条目有效字段标志：`FS_JG_ID | FS_JG_XY | FS_JG_TEXT`；用完后通过 DLL 释放。

**示例：**

```text
OcrInFile(0, 0, 834, 419, "screen.bmp", "", 0.5)
```

**注意：** 无需绑定窗口；坐标相对于图片，区域须按文件尺寸填写。采用当前模型、字库和 OCR 后端，不能用内存模板别名替代图片文件。

### 查找文字

<a id="FindStr"></a>

#### FindStr

**函数简介：**

查找目标文字的第一个位置。

**函数原型：**

```cpp
const FsTongyongJieguo* WINAPI FindStr(int x1, int y1, int x2, int y2, const char* text, const char* color_format, double sim);
```

**参数定义：**

- x1、y1：区域左上角坐标。
- x2、y2：区域右下角坐标，不包含右下边界；窗口接口使用客户区物理像素。
- text：待查文字，多个目标用 | 分隔，例如 "开始游戏|确定"；按列表优先，不跨行匹配。
- color_format：文字颜色过滤规则，同 Ocr；模型通常使用 ""。
- sim：范围 0.1～1.0；含义同 Ocr，与找图找色的离散容差不同。

**返回值：**

- 返回独立的 `FsTongyongJieguo` 指针。`status=0` 正常，`count=0` 无命中；错误为非 0 状态，分配失败为 `nullptr`。
- 条目提供目标索引 id、首字符坐标 x/y 和命中文字 text。最多 1 项。
- 条目有效字段标志：`FS_JG_ID | FS_JG_XY | FS_JG_TEXT`；用完后通过 DLL 释放。fullText 为空字符串。

**示例：**

```text
FindStr(0, 0, 834, 419, "开始游戏|确定", "", 0.5)
```

**注意：** 目标列表优先，不跨行拼接；x/y 为命中文字首字符的位置，id 为目标索引，text 为命中的目标文字。模型坐标为估计值。S 与非 S 版本使用相同的结果布局。

<a id="FindStrS"></a>

#### FindStrS

**函数简介：**

查找目标文字的第一个位置。

**函数原型：**

```cpp
const FsTongyongJieguo* WINAPI FindStrS(int x1, int y1, int x2, int y2, const char* text, const char* color_format, double sim);
```

**参数定义：**

- x1、y1：区域左上角坐标。
- x2、y2：区域右下角坐标，不包含右下边界；窗口接口使用客户区物理像素。
- text：待查文字，多个目标用 | 分隔，例如 "开始游戏|确定"；按列表优先，不跨行匹配。
- color_format：文字颜色过滤规则，同 Ocr；模型通常使用 ""。
- sim：范围 0.1～1.0；含义同 Ocr，与找图找色的离散容差不同。

**返回值：**

- 返回独立的 `FsTongyongJieguo` 指针。`status=0` 正常，`count=0` 无命中；错误为非 0 状态，分配失败为 `nullptr`。
- 条目提供目标索引 id、首字符坐标 x/y 和命中文字 text。最多 1 项。
- 条目有效字段标志：`FS_JG_ID | FS_JG_XY | FS_JG_TEXT`；用完后通过 DLL 释放。fullText 为空字符串。

**示例：**

```text
FindStrS(0, 0, 834, 419, "开始游戏|确定", "", 0.5)
```

**注意：** 目标列表优先，不跨行拼接；x/y 为命中文字首字符的位置，id 为目标索引，text 为命中的目标文字。模型坐标为估计值。S 与非 S 版本使用相同的结果布局。

<a id="FindStrEx"></a>

#### FindStrEx

**函数简介：**

查找目标文字的全部位置。

**函数原型：**

```cpp
const FsTongyongJieguo* WINAPI FindStrEx(int x1, int y1, int x2, int y2, const char* text, const char* color_format, double sim);
```

**参数定义：**

- x1、y1：区域左上角坐标。
- x2、y2：区域右下角坐标，不包含右下边界；窗口接口使用客户区物理像素。
- text：待查文字，多个目标用 | 分隔，例如 "开始游戏|确定"；按列表优先，不跨行匹配。
- color_format：文字颜色过滤规则，同 Ocr；模型通常使用 ""。
- sim：范围 0.1～1.0；含义同 Ocr，与找图找色的离散容差不同。

**返回值：**

- 返回独立的 `FsTongyongJieguo` 指针。`status=0` 正常，`count=0` 无命中；错误为非 0 状态，分配失败为 `nullptr`。
- 条目提供目标索引 id、首字符坐标 x/y 和命中文字 text。最多 1500 项。
- 条目有效字段标志：`FS_JG_ID | FS_JG_XY | FS_JG_TEXT`；用完后通过 DLL 释放。fullText 为空字符串。

**示例：**

```text
FindStrEx(0, 0, 834, 419, "开始游戏|确定", "", 0.5)
```

**注意：** 目标列表优先，不跨行拼接；x/y 为命中文字首字符的位置，id 为目标索引，text 为命中的目标文字。模型坐标为估计值。S 与非 S 版本使用相同的结果布局。

<a id="FindStrExS"></a>

#### FindStrExS

**函数简介：**

查找目标文字的全部位置。

**函数原型：**

```cpp
const FsTongyongJieguo* WINAPI FindStrExS(int x1, int y1, int x2, int y2, const char* text, const char* color_format, double sim);
```

**参数定义：**

- x1、y1：区域左上角坐标。
- x2、y2：区域右下角坐标，不包含右下边界；窗口接口使用客户区物理像素。
- text：待查文字，多个目标用 | 分隔，例如 "开始游戏|确定"；按列表优先，不跨行匹配。
- color_format：文字颜色过滤规则，同 Ocr；模型通常使用 ""。
- sim：范围 0.1～1.0；含义同 Ocr，与找图找色的离散容差不同。

**返回值：**

- 返回独立的 `FsTongyongJieguo` 指针。`status=0` 正常，`count=0` 无命中；错误为非 0 状态，分配失败为 `nullptr`。
- 条目提供目标索引 id、首字符坐标 x/y 和命中文字 text。最多 1500 项。
- 条目有效字段标志：`FS_JG_ID | FS_JG_XY | FS_JG_TEXT`；用完后通过 DLL 释放。fullText 为空字符串。

**示例：**

```text
FindStrExS(0, 0, 834, 419, "开始游戏|确定", "", 0.5)
```

**注意：** 目标列表优先，不跨行拼接；x/y 为命中文字首字符的位置，id 为目标索引，text 为命中的目标文字。模型坐标为估计值。S 与非 S 版本使用相同的结果布局。

### 快速查找文字

这四个函数使用 SetOcrEngine 选择的引擎，并提供分阶段耗时：

- `dict`：只匹配完整出现在某个待查目标内的字形。`sim=1.0` 先按字形高度与首列点阵定位候选，再比较其他列；模糊匹配扫描筛选后的字形，误差超过阈值立即放弃当前位置。
- `model`：复用既有模型会话识别整个区域，再在每一行内过滤无关字符；不会减少模型推理工作，因此不保证比普通 FindStr 更快。
- 会忽略部分中间文字。例如画面为“张和三”，目标为“张三”，当“和”不在本次待查字形中时，快速版可能命中；普通版保留“和”，不会把这两个字连成命中。多个备选目标共同决定保留哪些字符。
- 每行独立查找，不把上下两行拼成一次命中。字库复合字形按完整标签筛选，模型字符坐标为估计值。
- 字典、模型会话和已有采集设备继续复用；临时点阵内存按调用线程复用，每次依据本帧重建候选索引。关闭或切换绑定不会重建 OCR 会话。

<a id="FindStrFast"></a>

#### FindStrFast

**函数简介：**

查找目标文字的第一个位置。

**函数原型：**

```cpp
const FsTongyongJieguo* WINAPI FindStrFast(int x1, int y1, int x2, int y2, const char* text, const char* color_format, double sim);
```

**参数定义：**

- x1、y1：客户区区域左上角坐标。
- x2、y2：区域右下角坐标，不包含右下边界。
- text：UTF-8 待查文字，多个目标用 `|` 分隔，例如 `"长安|洛阳|大雁塔"`；各项不能为空，按列表顺序优先查找。
- color_format：同 Ocr，支持 RGB、HSV、灰度、多偏色及背景模式；模型通常用 `""`。
- sim：0.1～1.0，模型为整行置信度阈值，字库为点阵匹配阈值；字库精确匹配建议 1.0。

**返回值：**

- 返回独立的 `FsTongyongJieguo` 指针。`status=0` 正常，`count=0` 无命中；错误为非 0 状态，分配失败为 `nullptr`。
- 条目提供目标索引 id、首字符坐标 x/y 和命中文字 text。最多 1 项。
- 条目有效字段标志：`FS_JG_ID | FS_JG_XY | FS_JG_TEXT`；用完后通过 DLL 释放。fullText 为空字符串。

**示例：**

```text
FindStrFast(0, 0, 834, 419, "长安|洛阳|大雁塔", "9f2e3f-000000", 1.0)
```

**注意：** 目标列表优先，不跨行拼接；x/y 为命中文字首字符的位置，id 为目标索引，text 为命中的目标文字。模型坐标为估计值。快速版只保留待查字形，可能忽略中间的其他字符；模型快速版仍需完成模型推理。S 与非 S 版本使用相同的结果布局。

<a id="FindStrFastS"></a>

#### FindStrFastS

**函数简介：**

查找目标文字的第一个位置。

**函数原型：**

```cpp
const FsTongyongJieguo* WINAPI FindStrFastS(int x1, int y1, int x2, int y2, const char* text, const char* color_format, double sim);
```

**参数定义：**

- x1、y1、x2、y2：客户区查找区域，右下边界不包含。
- text、color_format、sim：同 FindStrFast。

**返回值：**

- 返回独立的 `FsTongyongJieguo` 指针。`status=0` 正常，`count=0` 无命中；错误为非 0 状态，分配失败为 `nullptr`。
- 条目提供目标索引 id、首字符坐标 x/y 和命中文字 text。最多 1 项。
- 条目有效字段标志：`FS_JG_ID | FS_JG_XY | FS_JG_TEXT`；用完后通过 DLL 释放。fullText 为空字符串。

**示例：**

```text
FindStrFastS(0, 0, 834, 419, "长安|洛阳", "9f2e3f-000000", 1.0)
```

**注意：** 目标列表优先，不跨行拼接；x/y 为命中文字首字符的位置，id 为目标索引，text 为命中的目标文字。模型坐标为估计值。快速版只保留待查字形，可能忽略中间的其他字符；模型快速版仍需完成模型推理。S 与非 S 版本使用相同的结果布局。

<a id="FindStrFastEx"></a>

#### FindStrFastEx

**函数简介：**

查找目标文字的全部位置。

**函数原型：**

```cpp
const FsTongyongJieguo* WINAPI FindStrFastEx(int x1, int y1, int x2, int y2, const char* text, const char* color_format, double sim);
```

**参数定义：**

- x1、y1、x2、y2：客户区查找区域，右下边界不包含。
- text、color_format、sim：同 FindStrFast。

**返回值：**

- 返回独立的 `FsTongyongJieguo` 指针。`status=0` 正常，`count=0` 无命中；错误为非 0 状态，分配失败为 `nullptr`。
- 条目提供目标索引 id、首字符坐标 x/y 和命中文字 text。最多 1500 项。
- 条目有效字段标志：`FS_JG_ID | FS_JG_XY | FS_JG_TEXT`；用完后通过 DLL 释放。fullText 为空字符串。

**示例：**

```text
FindStrFastEx(0, 0, 834, 419, "北京|我|来了", "020202-201020", 1.0)
```

**注意：** 目标列表优先，不跨行拼接；x/y 为命中文字首字符的位置，id 为目标索引，text 为命中的目标文字。模型坐标为估计值。快速版只保留待查字形，可能忽略中间的其他字符；模型快速版仍需完成模型推理。S 与非 S 版本使用相同的结果布局。

<a id="FindStrFastExS"></a>

#### FindStrFastExS

**函数简介：**

查找目标文字的全部位置。

**函数原型：**

```cpp
const FsTongyongJieguo* WINAPI FindStrFastExS(int x1, int y1, int x2, int y2, const char* text, const char* color_format, double sim);
```

**参数定义：**

- x1、y1、x2、y2：客户区查找区域，右下边界不包含。
- text、color_format、sim：同 FindStrFast；颜色串可保留 Ocr 的 `,分行符` 写法，但本函数输出仍采用固定的命中格式。

**返回值：**

- 返回独立的 `FsTongyongJieguo` 指针。`status=0` 正常，`count=0` 无命中；错误为非 0 状态，分配失败为 `nullptr`。
- 条目提供目标索引 id、首字符坐标 x/y 和命中文字 text。最多 1500 项。
- 条目有效字段标志：`FS_JG_ID | FS_JG_XY | FS_JG_TEXT`；用完后通过 DLL 释放。fullText 为空字符串。

**示例：**

```text
FindStrFastExS(0, 0, 834, 419, "北京|我|来了", "020202-201020", 1.0)
```

**注意：** 目标列表优先，不跨行拼接；x/y 为命中文字首字符的位置，id 为目标索引，text 为命中的目标文字。模型坐标为估计值。快速版只保留待查字形，可能忽略中间的其他字符；模型快速版仍需完成模型推理。S 与非 S 版本使用相同的结果布局。

### OCR 诊断

<a id="GetOcrError"></a>

#### GetOcrError

**函数简介：**

获取当前线程最近一次 OCR 操作的错误说明。

**函数原型：**

```cpp
const char* WINAPI GetOcrError(void);
```

**参数定义：**

- 无。

**返回值：**

- UTF-8 错误字符串；空字符串 "" 表示当前线程最近 OCR 操作没有错误。

**示例：**

```text
GetOcrError()
```

**注意：** 应紧接待诊断的 OCR 操作调用；其他成功的 OCR 设置或查询操作可能清除当前诊断，但不会改写已返回的独立数组。结果通过 `FreeAllResults()` 或 `FreeAllStringResults()` 释放。

<a id="GetOcrStatus"></a>

#### GetOcrStatus

**函数简介：**

查询 OCR 设置、最近一次诊断与阶段耗时，以及会话和设备累计计数。

**函数原型：**

```cpp
int WINAPI GetOcrStatus(FsOcrZhuangtai* status);
```

**参数定义：**

- status：FsOcrZhuangtai 输出结构指针，调用前设置 size=sizeof(FsOcrZhuangtai)。

**返回值：**

- 返回 FsJieguo 状态码：0（FS_CHENGGONG）表示查询成功；4（FS_CANSHU_CUOWU）表示指针或结构 size 不正确。查询成功不代表已经绑定或已有有效画面。

**示例：**

```cpp
// C++ 调用；工作台用 status 查看摘要，用 GetOcrError() 查看错误。
FsOcrZhuangtai s{};
s.size = sizeof(s);
int ret = GetOcrStatus(&s);
```

**注意：** 最近诊断和计时按线程隔离；会话/设备计数为共享累计信息。查询本身不执行 OCR，不读取像素。

## 附录一：FsJieguo 状态码

下表用于帧读取、结构体状态查询、坐标换算等接口，以及状态结构中的 `result`。不能套用到所有函数的直接返回值上。

| 数值 | 标识 | 含义 |
|---|---|---|
| 0 | FS_CHENGGONG | 操作成功 |
| 1 | FS_WEIBANGDING | 未绑定窗口 |
| 2 | FS_DENGDAISHOUZHEN | 等待首帧 |
| 3 | FS_HUANCHONG_BUZU | 调用方缓冲区容量不足 |
| 4 | FS_CANSHU_CUOWU | 参数错误 |
| 5 | FS_BUZHICHI | 不支持的方式、格式或能力 |
| 6 | FS_YIBANGDING | 已有绑定，当前操作不允许 |
| 7 | FS_CHUSHIHUA_SHIBAI | 初始化失败 |
| 8 | FS_CAIJI_SHIBAI | 采集或处理失败 |
| 9 | FS_JIAZAI_SHIBAI | 加载资源失败 |
| 10 | FS_CHUSHIHUA_CHAOSHI | 初始化超时 |
| 11 | FS_WEIXUAN_MOSHI | 尚未选择采集方式；当前默认 dx，保留此状态码 |
| 12 | FS_ZIYUAN_MANG | 资源或租约不足、忙碌 |
| 13 | FS_GPU_CHAOSHI | 等待 GPU 完成超时 |
| 14 | FS_OBS_CHONGTU | OBS 会话冲突 |
| 15 | FS_CHUANGKOU_BUPIPEI | OBS 发布窗口与绑定窗口不匹配 |

`detail` 可能保存原始 HRESULT 或 Win32 错误。查找未命中为 status=0/count=0；应结合 detail 和对应诊断接口定位系统错误。

## 附录二：公共结构体

结构声明见交付头文件 `FsDmJiekou.h`，采用 `#pragma pack(push, 8)`，按最大 8 字节对齐。调用方提供的采集和状态结构调用前必须设置 `size=sizeof(对应结构)`，当前 `version=1`。通用识别结果由 DLL 分配并填好，调用方不设置其字段，声明见前文“通用结果与内存管理”。下面列出采集与状态结构，方便查阅；业务代码应包含交付头文件，确保布局一致。

### FsZhen

```cpp
typedef struct FsZhen {
    uint32_t size;             // 调用前设置 sizeof(FsZhen)，返回本版本完整结构。
    uint32_t version;
    uint32_t width, height, stride, backend; // DX/OBS 为 10、11、12；WGC 使用 D3D11 纹理，为 11。
    uint32_t clientWidth, clientHeight;     // 客户区物理像素，不改变原始画面。
    uint64_t requiredBytes, frameNumber, bindingVersion;
    uint64_t captureQpc, qpcFrequency;       // 快照命令提交时的单调时钟，非 GPU 完成时刻。
    double readbackMs;                     // CopyFrame 返回本次读回耗时；GetStatus 中为 0。
    double captureMs;                      // CPU 提交 GPU 快照的耗时，不是 GPU 执行时间。
} FsZhen;
```

### FsZhuangtai

```cpp
typedef struct FsZhuangtai {
    uint32_t size, version;
    int32_t result, detail;                 // detail 保留原始 HRESULT / Win32 错误。
    uint32_t bound, hasFrame, initializing, inputImplemented;
    uint64_t hwnd, bindingVersion, capturedFrames, failedFrames;
    uint32_t ownerThreadId, mode;
    char display[16], mouse[16], keypad[16];
    FsZhen frame;
} FsZhuangtai;
```

### FsGpuZhen

```cpp
typedef struct FsGpuZhen {
    uint32_t size, version;
    uint64_t token, sharedHandle;
    uint32_t textureFormat, captureBackend;
    uint32_t adapterLuidLow;
    int32_t adapterLuidHigh;
    uint32_t flip, reserved;
    FsZhen frame;
} FsGpuZhen;
```

### FsCaijiZhuangtai

```cpp
typedef struct FsCaijiZhuangtai {
    uint32_t size, version, captureBackend, gpuSlots;
    uint32_t activeLeases, textureFormat;
    uint64_t skippedFrames, cpuReadbacks, readbackFrame, readbackBindingVersion;
    double lastReadbackMs;
    int32_t lastReadbackResult;
    uint32_t reserved;
} FsCaijiZhuangtai;
```

### FsChazhaoZhuangtai

```cpp
typedef struct FsChazhaoZhuangtai {
    uint32_t size, version, backend, reserved;
    int32_t result, detail;
    uint64_t deviceSerial, devicesCreated, shaderBuilds, resourceBuilds, templateUploads;
    uint64_t frameNumber, bindingVersion, resultReadbackBytes;
} FsChazhaoZhuangtai;
```

### FsOcrZhuangtai

```cpp
typedef struct FsOcrZhuangtai {
    uint32_t size, version, backend, engine;
    int32_t modelIndex, dictIndex, result;
    uint32_t loadedSessions;
    uint64_t sessionsCreated, devicesCreated, frameNumber, bindingVersion;
    double initializationMs, acquireMs, preprocessMs, detectionMs, recognitionMs, resultMs;
    char message[512];
} FsOcrZhuangtai;
```

补充说明：

- `FsZhen.backend` 为图形 API 类型 10/11/12，**不是** dx/obs/wgc 选择值；WGC 使用 11。实际采集后端请看 `FsCaijiZhuangtai.captureBackend` / `FsGpuZhen.captureBackend`：1=dx、2=obs、3=wgc。
- `width/height` 是原始采集尺寸，`clientWidth/clientHeight` 是客户区物理像素尺寸；WGC 已裁出客户区。
- `frameNumber` 是本模块快照编号，OBS/WGC 不保证它等于源程序实际呈现帧数；`bindingVersion` 用于识别所属绑定。
- `captureQpc/qpcFrequency` 表示快照提交时刻和计时频率；`captureMs` 是 CPU 提交采集命令耗时，不是 GPU 执行耗时。
- `GetStatus` 中 `frame.readbackMs` 为 0；`CopyFrame` 返回当前调用读回耗时。最近读回统计另见 `GetCaptureStatus`。
- `FsChazhaoZhuangtai.backend`：0=cpu、1=gpu；`result/detail` 是调用线程最近查找诊断，其他创建/上传计数为共享资源统计。
- `FsOcrZhuangtai.backend`：0=cpu、1=directml；`engine`：0=model、1=dict；`message` 为当前线程 UTF-8 诊断。OCR 检测/识别计时含必要执行等待和传回，不是纯 GPU 时间。

## 附录三：ZhuaTu.dll 底层导出

`ZhuaTu.dll` 另有 10 个导出函数，供 `ZhaoTu.dll` 协作使用。常规业务代码使用前面的 ZhaoTu 接口，避免混用两层返回值。下面列出实际原型与对应关系。

| 底层入口 | 作用 | 返回约定 |
|---|---|---|
| ZhuaTuBegin | 开始绑定及采集，参数与 BindWindow 一致，display 选择 dx/obs/wgc | FsJieguo，0 成功 |
| ZhuaTuEnd | 结束绑定及采集 | FsJieguo，0 成功 |
| ZhuaTuCopyFrame | 复制 CPU BGRA 帧 | FsJieguo，0 成功 |
| ZhuaTuGetStatus | 查询绑定及最新帧状态 | FsJieguo，0 成功 |
| ZhuaTuAcquireGpuFrame | 获取共享纹理租约 | FsJieguo，0 成功 |
| ZhuaTuReleaseGpuFrame | 释放共享纹理租约 | FsJieguo，0 成功 |
| ZhuaTuGetCaptureStatus | 查询扩展采集统计 | FsJieguo，0 成功 |
| ZhuaTuMapPoint | 客户区及原图坐标换算 | FsJieguo，0 成功 |
| ZhuaTuAcquireProcessor | 内部取得共用 D3D11 处理设备及上下文锁 | HRESULT，按 SUCCEEDED/FAILED 判断 |
| ZhuaTuReleaseProcessor | 内部释放处理设备租约及锁 | HRESULT，按 SUCCEEDED/FAILED 判断 |

```cpp
int WINAPI ZhuaTuBegin(HWND hwnd, const char* display, const char* mouse, const char* keypad, int mode);
int WINAPI ZhuaTuEnd(void);
int WINAPI ZhuaTuCopyFrame(void* pixels, uint64_t capacity, FsZhen* frame);
int WINAPI ZhuaTuGetStatus(FsZhuangtai* status);
int WINAPI ZhuaTuAcquireGpuFrame(FsGpuZhen* frame);
int WINAPI ZhuaTuReleaseGpuFrame(uint64_t token);
int WINAPI ZhuaTuGetCaptureStatus(FsCaijiZhuangtai* status);
HRESULT WINAPI ZhuaTuAcquireProcessor(uint32_t low, int32_t high, FsGpuChuli* processor);
HRESULT WINAPI ZhuaTuReleaseProcessor(uint64_t token);
int WINAPI ZhuaTuMapPoint(double x, double y, double* outX, double* outY, int direction);
```

`ZhuaTuAcquireProcessor(low, high, processor)` 的 low/high 是适配器 LUID 两部分，processor 为内部 `FsGpuChuli` 输出结构，调用前设置 size。返回的设备和上下文指针属于采集 DLL，禁止调用方 Release；token 持有上下文锁，必须由获取线程释放，不能嵌套获取。普通外部调用方应使用 GPU 帧租约及自己的设备，不直接使用此内部入口。

## 附录四：工作台使用

- 交付版 DLL 默认关闭工作台，由外部程序调用接口。需要交互测试时，将两个 DLL 各自 Peizhi.h 中的 `kGongzuotaiKaiqi` 设置为 `true` 并重新编译。本文的工作台示例适用于开启工作台的版本。
- `bind`、`unbind`、`status`、`log`、`help` 是工作台命令，不是额外 DLL 导出函数。
- 工作台对可调用接口自动显示返回值及总耗时；找图找色和 OCR 另显示分阶段耗时。工作台不支持任意结构体和指针参数。
- 不在工作台输入“示例 ret=...”式赋值或嵌套调用；直接输入函数名和参数。需要保存返回值、统计结果数量时由业务程序处理。
