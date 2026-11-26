# FsDm GPU 采集与绑定

Windows x64 / MSVC 2022 / C++20。两个 DLL 在目标进程内运行，仅支持该进程自己的窗口。
`ZhaoTu.dll` 负责绑定、工作台、截图保存、CPU/GPU 找图找色与 OCR；`ZhuaTu.dll` 负责 DX/OBS/WGC 采集、
GPU 快照及按需 CPU 读取。不包含注入器、键鼠操作、隐藏、驱动或 CUDA 接口。

## 构建与交付

在 FsDm 目录运行 `./Goujian.ps1`。要求安装 Visual Studio 2022 的 C++ 桌面开发组件和 Windows SDK。
脚本配置、编译、运行 CTest，成功后安装。OBS/WGC 硬件测试需要可用的 Direct3D 硬件设备及交互桌面。
WGC 使用系统 Windows Graphics Capture API，要求 Windows 10 1903 或更新版本，无额外配套 DLL。

统一交付目录 `FsDm/dll`：

- `ZhaoTu.dll`、`ZhuaTu.dll`
- `graphics-hook64.dll`、`get-graphics-offsets64.exe`（参考项目现有 x64 配套版本，未修改）
- `onnxruntime.dll`、`onnxruntime_providers_shared.dll`、`DirectML.dll`、`ocr_models/`（OCR 配套运行库和默认模型）
- `README.md`、`THIRD_PARTY.md`、`Yanzheng.md`、`licenses/`

整目录复制即可。ZhaoTu 根据自身路径加载 ZhuaTu，ZhuaTu 根据自身路径加载 OBS 文件，均不依赖当前工作目录。
保留 `ZhuaTu/dist`、`ZhaoTu/dist` 的 bin、include、lib 输出；OBS 文件同时安装到 `ZhuaTu/dist/bin`。
不附带参考项目的截图宿主、注入助手、32 位程序或调试文件。
所有拦截模块保持加载到进程结束；更新 DLL 前请退出目标进程，不支持用 FreeLibrary 中途卸载。

## 工作台和命令

两个编译期标志分别位于 `ZhuaTu/Peizhi.h`、`ZhaoTu/Peizhi.h`：

```cpp
constexpr bool kGongzuotaiKaiqi = true;
```

发行时手动改为 `false` 并重新编译；对应 DLL 不创建工作台、不输出日志，接口与采集继续工作。
没有环境变量或运行时开关。同进程共享控制台，只有 ZhaoTu 接收命令。
关闭按钮被禁用，避免关闭控制台结束宿主进程。
`BindWindow` 的第二个参数直接选择 `dx/obs/wgc` 采集；找图找色默认 `gpu`。
解绑、重绑及切换采集方式不会重置查找方式。未绑定时状态显示最近成功绑定的采集方式，初始为 dx。

```text
bind 0x窗口句柄 dx normal normal 0
status
log 5
unbind
bind 0x窗口句柄 obs normal normal 0
unbind
bind 0x窗口句柄 wgc normal normal 0
```

- `bind HWND {dx|obs|wgc} {normal|windows|dx} {normal|windows|dx} {0|1}`：绑定并选择采集方式。HWND 支持十进制及 0x 十六进制；切换方式前先 unbind。
- `unbind`：解绑、使当前缓存失效并取消剩余日志。
- `status`：显示一次状态。仅查询元数据，不等待 GPU、不读取像素。
- `log 次数`：每秒一次，默认次数为 0；新次数替换旧次数，`log 0` 取消。
  输入命令期间暂停打印；解绑或绑定版本改变后取消。次数范围 0～4294967295。
- `help`：显示帮助。

`display` 就是采集方式：`"dx"`、`"obs"`、`"wgc"`。`mode=0/1` 仍是绑定参数。
`gdi/gdi2/normal/dx2` 采集尚未实现，传入会返回失败；鼠标参数 `"normal"` 与采集方式无关。
鼠标和键盘参数仅保存配置，`inputImplemented=0` 明确表示操作尚未实现。

## 绑定与状态

包含 `ZhaoTu/ZhaoTu.h`。对外为 C 接口、WINAPI 调用约定及固定布局结构，不传 C++ 容器。
x64 的 HWND 是指针宽度，不得截断成 32 位整数。

`BindWindow` / `UnBindWindow`、模式选择和保存函数成功返回 1，失败返回 0。
`CopyFrame`、状态、GPU 租约和坐标转换接口使用 `FsJieguo`，成功为 `FS_CHENGGONG`，即 0。
查找函数的索引、坐标和字符串返回规则见下文。GetStatus 用于采集及绑定诊断，
不承担查找参数或模板文件错误诊断。调用结构接口前必须设置 size。
原有 FsZhen（88 字节）和 FsZhuangtai（208 字节）布局不变；新增 FsGpuZhen（136 字节）、
FsCaijiZhuangtai（72 字节），返回 version=1。

同一进程一个绑定。相同配置重复绑定成功，保持原绑定版本和所有者线程；修改配置前必须解绑。
重复解绑成功。初始化总等待上限七秒，不等待窗口产生首帧；无呈现时显示“等待首帧”。
采集方式和绑定在同一控制锁内生效；失败绑定不改变已有绑定或最近成功绑定的采集方式。
已移除 `SetCaptureBackend`、`ZhuaTuSetCaptureBackend` 导出及 `capture` 工作台命令。
`BindWindow` 的公开参数列表不变，底层 `ZhuaTuBegin` 新增 display 参数；两个 DLL 必须一起更新，
直接调用底层旧原型的业务代码也须重新编译。
先装本模块的 DX 拦截，再于首次 OBS 绑定加载 OBS 拦截，此后两套拦截保持加载。
未绑定不提交快照，绑定后 DX/OBS 随目标窗口呈现更新，WGC 随系统合成画面更新。
WGC 独立工作线程仅在绑定期间开启系统采集会话，不依赖目标窗口的 DirectX Present。
绑定线程退出或窗口销毁后自动解绑；
后台约每 100ms 检查，读取和发布时也检查生命周期。退出前建议显式解绑。

状态码包括：未绑定、等待首帧、容量不足、参数错误、不支持、已有绑定、初始化/采集/加载失败、
初始化超时、未选择后端、资源忙、GPU 等待超时、OBS 会话冲突、OBS 窗口不匹配。
完整定义见 `Gonggong/Jiekou.h`。不支持的纹理格式及设备丢失通过状态码和原始 HRESULT 诊断。

## GPU 缓存和租约

呈现线程仅提交 GPU 复制，不执行 Map 或 CPU 像素转换；不预分配最大 CPU 像素缓冲区。
快照池最多三个槽位，包括解绑后仍被调用方持有的旧快照。已租出的纹理不会被覆盖或重建。
无可写槽位时跳过本次更新并增加 skippedFrames；呈现不会等待调用方释放。
最新槽位若无人持有也允许复用，避免两个旧租约占用槽位时停止更新。
控制锁忙时也直接跳过呈现更新，此类跳过未计入池满计数。

```cpp
// 第二参数可选 "dx"、"obs"、"wgc"；绑定成功返回 1。
if (!BindWindow(hwnd, "dx", "normal", "normal", 0)) { /* GetStatus 诊断 */ }
FsGpuZhen gpu{};
gpu.size = sizeof(gpu);
if (AcquireGpuFrame(&gpu) == FS_CHENGGONG) {
    // 使用 adapterLuidLow/High 选择同一适配器，创建调用方自己的 D3D11 设备。
    // device->OpenSharedResource(reinterpret_cast<HANDLE>(gpu.sharedHandle), ...);
    // 只读使用；用调用方的事件查询/栅栏确认自己的 GPU 工作已经完成。
    ReleaseGpuFrame(gpu.token);
}
UnBindWindow();
```

AcquireGpuFrame 确认生产方复制完成后才返回，每次 GPU 完成等待上限两秒。
共享句柄是旧式 DXGI 句柄，**禁止 CloseHandle**。调用方持有自己打开的 COM 资源引用。
共享纹理限同进程、同适配器使用；textureFormat 为实际 DXGI_FORMAT，flip=1 表示需上下翻转。
支持的原始像素格式保留，类型不明确的格式使用相容的明确类型；多重采样先 Resolve 为单采样。
GPU API 不保证 BGRA，仅 CPU CopyFrame 保证 BGRA。调用方不得写入共享纹理。

ReleaseGpuFrame 只接收有效 token，重复释放返回参数错误；内部租约表最多 64 个同时读取请求，
超过返回资源忙。调用方必须在自己的 GPU 使用完成后释放，不能只提交命令就立即释放。
解绑立即拒绝新读取并清除当前有效帧；已取得的租约持续有效到释放，可在重新绑定后释放旧 token。
正在读取的调用若遇到解绑，返回未绑定，其输出不得作为成功帧使用。不会将旧绑定发布到新绑定。

## 按需 CPU BGRA

```cpp
FsZhen frame{};
frame.size = sizeof(frame);
int result = CopyFrame(nullptr, 0, &frame);
// 有帧时返回 FS_HUANCHONG_BUZU 和 requiredBytes；此步骤不读取像素。
// 调用方按 requiredBytes 分配内存，再调用：
// result = CopyFrame(pixels, capacity, &frame);
// 尺寸可能变化，第二次调用仍须检查容量不足并重试。
```

CopyFrame 为当前 GPU 快照保留内部租约，首次 CPU 读取时按显卡适配器创建独立 D3D11
读取设备和上下文；同一显卡上的槽位共用该设备，各槽位分别保留打开的纹理、暂存纹理和完成查询。
随后等待 GPU、Map、转换并写入调用方 CPU 缓冲区。只查容量、容量不足和空指针错误不会读回。
同一显卡的 CPU 读回串行保护上下文，其他槽位仍能提交快照；多个线程不会读到半帧。
尺寸或格式变化只重建对应槽位的纹理；适配器改变或设备丢失时重新取得读回设备。
读回设备缓存最多三个条目，健康设备跨解绑重绑复用，无槽位引用的旧条目可被淘汰。
缓存随已固定加载的 DLL 保留至进程结束，退出时由系统回收，避免在 DLL 卸载锁内执行驱动清理。
输出为从上到下、紧密排列 BGRA，每行 width×4 字节。最大 16,777,216 像素，即 64MiB。
当前支持 8 位 RGBA/BGRA/BGRX（含 sRGB）、B5G6R5、R10G10B10A2 及相容 typeless 类型。
不支持的格式/共享资源能力明确报错，不做 CPU 共享内存降级或保存图片。
暂停呈现时保留最近快照，可比较 frameNumber 和 captureQpc 判断是否更新。
OBS 协议没有可靠源帧号；frameNumber 是本模块成功复制的快照编号，不能当作源呈现帧数。
WGC 的编号同样是本模块快照编号；画面不变时，系统不保证产生新帧。

## 工作台函数调用和截图

工作台支持直接输入与公开 C 接口一致的函数名和参数，例如：

```
BindWindow(0x123456, "wgc", "normal", "normal", 0)
SetPath("C:\截图")
Capture(0, 0, 100, 100, "screen.bmp", 2)
CaptureJpg(0, 0, 100, 100, "screen.jpg", 2)
CapturePng(0, 0, 100, 100, "screen.png", 2)
CaptureGif(0, 0, 100, 100, "screen.gif", 100, 3000)
UnBindWindow()
```

函数名区分大小写，字符串用双引号，Windows 路径中的反斜杠直接输入；整数接受十进制或
`0x` 十六进制。工作台会显示函数原始返回值和调用耗时（毫秒）；计时仅包含函数执行，
不包含命令解析和输出，GIF 的耗时包含录制等待。保存和绑定函数返回 1 表示成功、0 表示失败。
绑定本进程窗口，等首帧出现后才能截图；切换采集方式先解绑，再以新的第二参数绑定。

构建时从 `ZhaoTu.h` 自动生成调用表；支持 `int`、`uintptr_t`、`const char*` 和
`const FsTongyongJieguo*` 返回值。工作台完整显示通用结构的 status、detail、count、fullText 和每个条目，
随后只释放本次结果。坐标直接来自 items，不再传入 intX/intY。
普通参数支持整数、小数、HWND、UTF-8 字符串；FreeResult 可接受结果地址，但工作台自己的结果已自动释放，
不要再次使用其地址。其他缓冲区和输出结构参数需用调用程序测试。小数拒绝 NaN 和无穷大。
工作台与外部程序调用相同导出函数；关闭工作台的编译标志不影响这些接口。

截图坐标是客户区物理像素，右下角不包含；客户区与原图尺寸不同时按比例换算到原图。
文件路径以 UTF-8 传入，工作台自动转换中文；相对路径基于 `SetPath` 设置的已有目录，
未设置时基于进程当前目录。成功时覆盖同名文件。GIF 的 delay 和 time 单位为毫秒，delay=0
保存单帧；动画调用阻塞工作台，实际帧间隔受编码耗时和 GIF 10 毫秒精度影响。
解绑、无有效画面、区域或路径无效、录制期间绑定或尺寸变化都会导致保存失败。

## CPU / GPU 找图找色

抓图后端与查找后端互相独立：`BindWindow` 第二参数选择 dx/obs/wgc，
`SetFindBackend("cpu")` / `SetFindBackend("gpu")` 选择找图找色算法，默认 gpu，允许绑定期间切换。
两种有效选择返回 1；未知名称或空指针返回 0，并保留当前后端。切换查找后端不重启采集或重新绑定。
十个公开接口共用参数解析、TSPlug 比较规则及结果结构：GetColor、CmpColor、FindColor、FindColorEx、
FindMultiColor、FindMultiColorEx、FindPic、FindPicS、FindPicEx、FindPicExS。

CPU 分支按需读取完整 BGRA；GPU 分支使用 D3D11 计算着色器（在显卡上执行比较的小程序），
不调用 CopyFrame，也不把完整画面读回 CPU。GetColor 只读回 4 字节 RGB，
其他查找分批读回命中位集合，再由 CPU 按原扫描顺序整理坐标。
颜色列表优先、方向、离散 sim 容差、透明色和输出数量限制与 CPU 分支相同；
不使用匹配比例或近似抽样代替逐通道比较。

GPU 查找要求同适配器的 D3D11 Feature Level 11.0；不支持时在通用结果中返回明确错误状态，
不自动退回 CPU。首次调用会创建处理资源、编译着色器；后续复用设备、上下文、
着色器、结果缓冲区和纹理。同适配器的 CPU 读回与 GPU 查找共用处理设备及上下文锁。
最多缓存三个处理设备的资源；每个处理设备的 GPU 模板缓存最多 128 项、256 MiB，
按需占用显存，任一上限超出后按最近使用顺序淘汰；文件变化后重新上传。
查找期间固定快照租约，确认显卡使用完成后才归还；解绑重绑后的旧结果被丢弃。

绑定窗口并等到首帧后即可查找；默认 DX 采集、GPU 查找。以下命令同时演示手动切换：

```text
SetFindBackend("cpu")
GetColor(30, 30)
FindColor(0, 0, 500, 500, "123456-000000|ffffff-020202", 1.0, 0)
SetPath("C:\截图")
FindPic(0, 0, 500, 500, "test.bmp|test2.bmp", "000000", 0.9, 0)
FindPicS(0, 0, 500, 500, "test.bmp", "000000", 1.0, 0)
FindPicEx(0, 0, 500, 500, "test.bmp", "000000", 1.0, 0)
FindPicExS(0, 0, 500, 500, "test.bmp", "000000", 1.0, 0)
SetFindBackend("gpu")
FindPic(0, 0, 500, 500, "test.bmp", "000000", 0.9, 0)
status
SetFindBackend("cpu")
```

`status` 显示调用线程最近一次查找的状态码、HRESULT、处理设备编号、设备创建次数、
着色器编译次数、资源创建次数、模板上传次数及本次结果读取字节数，查询本身不执行查找或读回。
C 调用方用 `FsChazhaoZhuangtai s = {0}; s.size = sizeof(s); GetFindStatus(&s);`
查询同线程记录；此查询接口成功返回 `FS_CHENGGONG`（0），与原有状态接口一致。
未命中为 status=0/count=0；设备错误等为非零 status，可结合 detail 和诊断接口区分。

找图、找色、OCR 和找字共 22 个函数统一返回 `const FsTongyongJieguo*`。
本次改变二进制接口，外部程序必须按新头文件重新编译，不能再按整数、字符串或坐标输出指针调用。
结果有 status/detail、count/items 及 fullText；条目提供 fields、id、x、y、color、text、file。
坐标和 RGB 是原生数字，直接读取即可。fields 指明哪些字段有效。

```cpp
#include "ZhaoTu/ZhaoTu.h"
#include <cstdio>
void Ceshi() {
    const FsTongyongJieguo* r = FindPic(0, 0, 834, 419, "test.bmp", "000000", 1.0, 0);
    if (r && r->status == FS_CHENGGONG) {
        for (uint32_t i = 0; i < r->count; ++i) {
            const FsJieguoXiang& item = r->items[i];
            std::printf("id=%d x=%d y=%d file=%s\n", item.id, item.x, item.y, item.file);
        }
    }
    FreeResult(r);
}
```

- `GetColor` 返回一个 XY/COLOR 条目，color 为 `0xRRGGBB` 整数；负坐标归零，越界报参数错误。
- `CmpColor` 匹配时一个 ID/XY/COLOR 条目，不匹配时 count=0。
- `FindColor` 返回至多一个 ID/XY 条目；FindColorEx 返回数组。
- `FindPic` 与 `FindPicS` 返回至多一个 ID/XY/FILE 条目；Ex 与 ExS 返回同布局的数组，最多 1500 项。
  id 是原模板列表索引，file 保留传入名称或路径；保留重叠命中和重复模板。
- `status=0/count=0` 表示正常无命中；非零 status 表示错误；nullptr 仅表示分配失败。
- 每次结果独立分配，后续调用、解绑或原线程退出不会覆盖它；用 `FreeResult(r)` 精确释放。
  `FreeAllResults()` 释放所有结果；`FreeAllStringResults()` 为兼容别名，也处理通用结构。
- `MatchPicName` 和 `GetOcrError` 仍返回独立 UTF-8 数组，通过 `FreeAllResults()` 或 `FreeAllStringResults()` 释放，不能传给 `FreeResult`。
  释放与读取不能并发，禁止自行 delete/free。释放结果不清理模板、模型、设备、原始像素或 GPU 租约。
- 工作台显示全部条目后只精确释放本次结果，不影响外部保留的同名结果。
  完整结构、字段标志和 22 个接口名单见 `HanshuShuoming.md` 的“通用结果与内存管理”。

算法与参数对应 TSPlug4.19 `TSFindPicture.cpp`，不是匹配像素比例算法：

| 本项目接口 | 参考源码入口/分支 | 返回 |
| --- | --- | --- |
| GetColor | DXBind::GetColor / processColor | XY、整数 COLOR |
| FindColor | processColor 普通找色分支 | ID、XY |
| FindPic | findPicture，无 retstring | ID、XY、FILE |
| FindPicS | findPicture，type=2 | ID、XY、FILE |
| FindPicEx | findPicture，type=0 | ID、XY、FILE 数组 |
| FindPicExS | findPicture，type=1 | ID、XY、FILE 数组 |

- `FindColor` 每项格式为 `RRGGBB-DRDGDB`，多项用 `|` 分隔，接受大小写十六进制。
  相似度 sim 必须为有限数且位于 [0.1,1.0]。忽略空颜色项；索引按非空颜色项计数。
  先完整扫描第一种颜色，再扫描第二种；返回第一个有命中的颜色索引。
- **找色与四个找图函数共用参考源码的离散相似度表**。每通道容差为
  `显式通道偏色 + 额外容差`，实际颜色与目标颜色的绝对差小于等于容差才通过。

  | sim | 0.5 | 0.6 | 0.7 | 0.8 | 0.9 | 其他合法值（如 1.0、0.95、0.4） |
  | --- | --- | --- | --- | --- | --- | --- |
  | 额外通道容差 | 49 | 38 | 29 | 18 | 9 | 0 |

  不插值；`0.95` 并不比 `0.9` 宽松。`sim=0.9, delta_color="000000"` 要求每个
  有效像素的 R、G、B 差值分别不超过 9，不允许任意 10% 像素完全不匹配。
- 找色方向 0/1/2/3 为逐行遍历：上到下左到右、下到上左到右、上到下右到左、下到上右到左。
  方向 4 从中心向外螺旋，偶数尺寸取靠左上中心，依次向上、右、下、左移动，
  各段长度为 1、1、2、2、3、3……；区域外整段直接跳过，保持区域内访问顺序。
  5/6/7/8 为逐列遍历：左到右上到下、右到左上到下、左到右下到上、右到左下到上。
- 找图方向仅支持 0～3。**图片列表优先**：搜索完第一张再搜索第二张，索引总是对应原列表。
  模板不存在、格式不正确、无法解码或无法放入区域时跳过该项；全部无命中时返回未找到。
  Ex 系列先输出第一张的全部命中，再输出第二张，直到最多 1500 项。
- 磁盘模板支持 24 位 BMP、PNG、JPG/JPEG；BMP 四角 RGB 相同按透明色处理，PNG 完全透明像素跳过、半透明像素按 RGB 比较，JPG 按不透明图处理；
  **全图同色按不透明模板比较**，避免不比较任何像素就误报成功。
  `delta_color` 是六位 RGB 通道偏色，与上述额外容差相加。
  依搜索方向预先记录有效像素和在帧内的字节偏移；候选位置中任一有效像素不合格立即排除，
  全部合格才命中。无抽样近似，不累计错误像素比例；不匹配位置通常只需比较最先几个有效像素。

坐标为客户区物理像素。找色包含右下角；找图和 Capture 系列不包含右下角。
找色/找图的负左上角归零，超出客户区的右下角裁剪；原始空/反向区域、
左上角已经超过客户区均失败。Capture 系列原有严格区域检查不变。
GetColor/FindColor 将每个客户区位置按比例向下取整映射到原图。
模板解码后保留原始像素尺寸，与 Capture 保存的图片一致；不推断应用内部视口或自动缩放模板。
FindPic 系列将区域边界换算到原图后逐原图像素扫描，模板须完整落在该区域，
命中后按比例向下取整返回客户区坐标，确保结果不越过请求区域的左上边界。
客户区小于原图时，多个原图命中可能换算为相同客户区坐标，Ex 接口保留这些命中。

与参考源码的安全修正及边界差异：

- 不复刻反向比较中的宽高下标混用、坐标偏移和 Ex 重复/遗漏；所有方向完整比较模板，
  返回正确左上角。允许模板贴住最后一行/列，不复刻旧源码部分分支漏扫边缘。
- 找色螺旋保留移动顺序，但包含与其他方向一致的右下边缘；旧螺旋分支错误地排除了该边缘。
- 全图同色统一按源码注释的“不透明”意图处理，修正非方形模板可能被当成全透明的错误。
- 保留本项目严格参数/十六进制校验、BMP24 校验及 PNG/JPG 解码、CPU 缓冲区上限、1500 项结果上限、
  UTF-8 路径、独立返回数组及尺寸映射；不复制旧源码的固定数组越界和字符串拼接风险。
- 因上述修正，本实现对应其匹配算法和参数规则，不声称逐项复现旧程序的缺陷。

CPU 查找先按需读取一份完整 BGRA，GPU 查找固定一份 GPU 快照；整个调用使用同一份内容。
暂停呈现时使用缓存的最新帧。选择模式本身不读取像素，GPU 分支也不读取完整 CPU 画面。
找图、找色按线程保留 CPU 帧缓冲区容量；截图保存另按线程保留完整帧和裁剪缓冲区容量。
每次调用仍重新读取最新快照，不会因复用内存而沿用上一次调用的像素。
缓冲区按实际使用尺寸增长，线程退出时释放，不提前分配最大容量。

磁盘图片模板按解析后的完整路径缓存，比较路径时忽略大小写；SetPath 切换到不同文件会使用不同缓存项。
普通自动缓存每次使用前检查文件大小和最后修改时间，变化或文件消失时使旧缓存失效；解码期间变化的文件不入缓存。
内容改变但人为保持大小和修改时间完全相同的情况无法据此识别。
CPU 模板缓存最多保留 128 张、256 MiB（解码像素与比较样本容量），按需占用内存，
任一上限超出后按最近使用顺序淘汰普通自动缓存，显式加载的项不参与自动淘汰。
超过单项容量限制的模板仍可正常查找，但不驻留缓存。
帧行跨度或方向变化时从已解码像素重建比较样本，无需重复解码；返回文件名保留本次参数的写法。
并发查找共享只读样本，淘汰或更新不影响已开始的匹配；正在匹配的旧样本可暂时使实际内存超过缓存上限。
未实现图像缩放算法。

### 模板预加载和释放

以下三个 C 接口返回 1 成功、0 失败，无需先绑定窗口。CPU / GPU 四个 FindPic
接口共用同一份预加载模板。名称采用 UTF-8，相对路径按照当前 `SetPath` 解析。

```cpp
int WINAPI LoadPic(const char* pic_name);
int WINAPI FreePic(const char* pic_name);
int WINAPI LoadPicByte(uintptr_t addr, int size, const char* pic_name);
int WINAPI Capture(int x1, int y1, int x2, int y2, const char* name, int mode);
int WINAPI CaptureJpg(int x1, int y1, int x2, int y2, const char* name, int mode);
int WINAPI CapturePng(int x1, int y1, int x2, int y2, const char* name, int mode);
```

- `LoadPic("1.bmp|2.png|3.jpg")` 从文件加载 BMP24、PNG 或 JPG/JPEG，支持文件名中的 `*`、`?` 通配符，
  例如 `LoadPic("*.png")`。目录部分不能带通配符；没有匹配文件返回 0。
  显式加载后不再检查或读取源文件，删除、覆盖源文件也继续使用已加载内容；
  重复 LoadPic 同一显式名称直接成功。更新时先 FreePic，再 LoadPic。
- `LoadPicByte(addr, size, "memory.bmp")` 从本进程内存复制并解码完整 BMP24 文件数据，
  包含 BMP 文件头，不能传入裸 BGRA 像素。`size` 为文件字节数，最多 256 MiB，
  解码图像仍受 16,777,216 像素上限约束；地址使用 `uintptr_t`，适配 x64。
  成功后调用方可立即释放原内存。名称是单个别名，无需存在磁盘文件，
  不接受列表或通配符；同名调用成功后替换原模板，失败则保留原内容。
- `FreePic("1.bmp|memory.bmp")` 释放对应 CPU 及 GPU 模板缓存，也支持 `*.bmp` 等通配符。
  匹配缓存中的名称，因此可以释放已删除的文件和虚拟别名。合法名称没有缓存也返回 1；
  空名称等错误参数返回 0。已经开始的查找可继续完成，旧资源在使用结束后清理。
  普通文件被释放后再次 FindPic 会自动从磁盘加载；虚拟别名需再次 LoadPicByte 才能使用。
- 三个 Capture 函数的 `mode` 均为必填参数：`1` 仅把截图像素登记为模板，`2` 仅保存图片文件，
  `3` 同时登记模板并保存文件。`name` 在模式 1 中是模板别名；模式 2 中是文件路径；
  模式 3 中同时作为模板别名和文件路径。模板不要求有扩展名，按原始截图 RGB 匹配，不受 JPEG 压缩影响。
  截图模板与 LoadPic 使用相同缓存预算及名称空间，同名时替换旧模板，可通过 FreePic 释放。
  模式 3 若文件保存成功但模板缓存预算不足，会返回 0；已经保存的文件不会回滚。

显式加载和普通自动缓存合计最多 128 项 / 256 MiB（解码像素及比较样本）。
显式条目保留到 FreePic 或进程结束，容量不足时先淘汰普通条目；
如果显式条目自身已占满预算，则新加载返回 0。列表逐项加载，
若中途失败，已经成功的项仍然保留。被正在执行的查找引用的旧对象可延迟释放。
当显式条目占满预算时，普通文件仍可临时加载查找，但不会挤掉预加载内容。
相对名称切换 `SetPath` 后解析成另一缓存键；切回原目录可再次使用原缓存。

LoadPic / LoadPicByte 预先完成 CPU 解码，GPU 设备及模板缓冲区在首次 GPU 查找时按需准备，
后续复用设备和已上传模板；GPU 缓存仍按每设备 128 项 / 256 MiB 淘汰。
FreePic 和模板替换只清理模板，不重建处理设备、着色器或采集会话。

工作台直接输入函数即可，输出返回值和耗时：

```text
SetPath("C:\模板")
LoadPic("test.bmp")
FindPic(0, 0, 1172, 590, "test.bmp", "000000", 0.9, 0)
FreePic("test.bmp")
```

FindPic 前仍须绑定并取得画面。LoadPicByte 也可在工作台调用，例如
`LoadPicByte(0x实际内存地址, 实际BMP字节数, "memory.bmp")`，
地址必须由被注入进程提供；它不是磁盘路径，也不是另一个进程的地址。

工作台为 GetColor、FindColor、FindColorEx、FindMultiColor、FindMultiColorEx、
FindPic、FindPicS、FindPicEx、FindPicExS 显示总耗时及分段耗时。
以下是输出格式示例，数值仅为示意：

```text
返回值: status=0，detail=0x00000000，count=1，总耗时: 30.500 毫秒
items[0]: id=0 x=463 y=137 file="test.bmp"
采集取帧: 8.500 毫秒，模板准备: 1.200 毫秒，找图: 20.500 毫秒，其他: 0.300 毫秒
```

- **采集取帧（CPU）**：本次调用查询容量、准备 CPU 缓冲区、GPU→CPU 读回及 BGRA 转换的时间，
  含必要的等待和读回资源初始化。取得的是缓存的最新 GPU 快照，不等待下一次呈现。
  这不是后台持续采集的 GPU 执行时间，也不是 status 中的“提交”时间。
- **模板准备**：找图专有，累加本次实际尝试的各张 BMP 的路径解析、文件属性检查、
  缓存选择，以及必要时的读取、WIC 解码、透明色处理和比较样本准备，包含失败的加载尝试。
- **找图 / 找色 / 取色**：扫描匹配、命中坐标收集或单像素读取的时间。
  找图累加各模板的扫描时间，不包含模板准备和最终结果结构整理。
- **其他**：总耗时减去上述阶段，包含参数检查、区域换算、结果整理及资源释放等。
- **总耗时**：整个导出函数调用，排除工作台的命令解析和输出。

GPU 模式额外显示 **GPU资源准备** 和 **GPU 等待及结果读取**。采集取帧用于取得快照租约、等待生产方
复制完成及取得共用处理上下文；资源准备包含着色器、打开纹理和结果缓冲区等准备，以及复制到
可供着色器读取的纹理。模板准备还包含必要的比较样本上传。
GPU 的操作字段显示为 **找图提交 / 找色提交 / 取色提交**，记录 CPU 提交工作所用时间；
**GPU 等待及结果读取** 包含等待 GPU 完成计算、结果复制和 CPU 读取整理。
这一合计时间不能单独表示 GPU 到 CPU 的传输耗时，也不能单独表示 GPU 执行时长。
嵌套阶段暂停外层计时，其他时间扣除全部已显示阶段。CPU 模式的字段名称保持不变。

没有进入的阶段显示“未执行”；进入后失败的阶段仍显示本次尝试耗时。
例如参数检查失败时各阶段均未执行；模板不存在时仍有取帧和模板准备时间，找图未执行。
记录按线程隔离，每次调用重新开始；其他线程或上一次调用的统计不会混入本次结果。
各项保留三位小数，求和可能有舍入误差；计时只在阶段边界进行，不在逐像素比较中计时。
首次调用可能包含读取设备及 WIC 解码器初始化，不能把总耗时当作纯算法或 GPU 耗时。
其他导出函数继续使用原来的“耗时”输出；status/log 的含义和不触发像素读回的行为保持不变。

## 多点找色与全部颜色坐标

这些接口也默认使用 GPU，并共用 SetFindBackend 的选择。
CPU 每次调用只读取一份最新 BGRA；GPU 每次调用固定一份快照，不为每个颜色或偏移点重新取帧。
返回通用结果中的条目数组，后续调用不覆盖；使用完后调用 `FreeResult` 精确释放，或调用 `FreeAllResults` 全量释放。
以下命令可直接输入工作台，不需要提供 intX/intY：

```text
FindColorEx(0, 0, 499, 499, "123456-000000|abcdef-202020", 1.0, 0)
FindMultiColor(0, 0, 499, 499, "cc805b-020202", "9|2|-00ff00,15|2|2dff1c-010101,6|11|a0d962,11|14|-ffffff", 1.0, 1)
FindMultiColorEx(0, 0, 499, 499, "cc805b-020202", "9|2|-00ff00,15|2|2dff1c-010101,6|11|a0d962,11|14|-ffffff", 1.0, 1)
```

- `FindColorEx` 返回 ID/XY 数组，按颜色列表优先：列完第一种颜色，再列下一种。
  同一位置匹配不同列表项时保留各项索引。支持方向 0～3、5～8，不接受 4。
  保留旧 1024 个 ASCII 字符的坐标预算限制，以数字位数计算前缀上限，运行时不生成坐标串。
  密集命中时返回扫描顺序中的前缀。
- `FindMultiColor` 的首色只接受一种 `RRGGBB` 或 `RRGGBB-DRDGDB`；
  偏移串为 `dx|dy|颜色,dx|dy|颜色,...`，至少一个点，支持十进制负偏移及偏色。
  所有点同时符合才算命中；命中时 count=1，条目为整组首点坐标；无命中 count=0。
- `FindMultiColorEx` 使用同样的匹配规则，返回首点条目数组，最多 1500 项。
  每项 id=0 沿用 TSPlug 首色序号，多个条目表示多组完整图案的首点。两个多点接口只支持方向 0～3。
- 偏移颜色前的 `-` 为反色：`2|1|-aabbcc-010101` 表示该点不在指定 RGB 容差范围内；
  任意一个通道超出容差即满足反色，不是求 RGB 补色。负偏移写在 dx/dy，和反色符号独立。
- 上述三个接口允许省略偏色，等价于 `000000`；sim 沿用已有离散额外通道容差表，
  也参与反色判断。格式错误、NaN、非法方向均失败；字符串接口失败/未找到返回 `""`。
- 搜索区域沿用找色的右下角包含及客户区裁剪规则。偏移和结果均为客户区物理像素，
  客户区与渲染尺寸不同时复用现有映射。所有偏移点必须落在裁剪后的搜索区域内。
  参考源码对负偏移及边缘有越界或误报风险，本实现预先缩小合法首点范围，不复制这些缺陷。

三个接口工作台输出均包含总耗时、采集取帧、找色和其他；没有模板准备阶段。
参数解析计入其他时间，候选扫描、各偏移点检查及结果收集计入找色时间。
工作台通过头文件自动生成函数调用表，没有新增专用命令分支。

## 坐标和时间

width/height 保留原始渲染尺寸；clientWidth/clientHeight 为采集时客户区物理像素尺寸。
WGC 的系统帧包含完整窗口，本模块在 GPU 上裁出客户区，所以 WGC 的 width/height 为客户区尺寸。
MapPoint 方向 0 是客户区到原图，方向 1 是反向；支持小数，合法范围 [0,宽)、[0,高)。
按完整画面对应完整客户区缩放，不推断应用内部视口。

captureQpc/qpcFrequency 为快照命令提交时的单调时钟，可计算缓存年龄，**不是 GPU 完成时刻**。
captureMs 是 CPU 识别队列、提交 GPU 复制和准备元数据的时间，不代表 GPU 执行耗时，
不包含原始 Present。GetStatus 的 frame.readbackMs 恒为 0；CopyFrame 返回本次读取的 readbackMs，
包含等待、首次初始化、像素转换及同槽位读取排队。
GetCaptureStatus 单独返回最近一次 CPU 读取尝试的耗时、结果、帧号、绑定版本及尝试次数。
尚未调用 CopyFrame 时日志显示“未执行”。status/log 不调用 CopyFrame 或 AcquireGpuFrame。

## OBS 限制与错误

仅接入 DXGI 共享纹理采集。严格比对 OBS 发布的 window 与绑定 HWND，发现其他窗口画面直接报错，
不会枚举其他窗口或附着其纹理。已有外部 HookInfo、同名 hook 模块或 keepalive 会话时拒绝绑定，
不接管其他 OBS 会话。同一进程多个渲染窗口时，OBS 可能先选中另一个窗口，此时会报窗口不匹配。
缺少 DLL/偏移助手返回加载失败及 ERROR_FILE_NOT_FOUND。依赖从 ZhuaTu 所在目录定位。

解析器保留 d3d12.execute_command_lists 字段；配套助手不输出该段时保持 0，使用 hook 内部队列探测。
本机 DX12 OBS 实测成功。偏移结果在本进程内复用。OBS hook 自身重建源纹理，
本模块按共享句柄和设备复用已打开的源纹理；协议映射或会话变化时清空，避免句柄复用后误读旧纹理。
模块的快照池复用设备和纹理。
此协议不能保证外部 OBS 同时抢占的原子排他，也不能控制 OBS 首选哪个交换链。

## WGC 行为

WGC 通过 CreateForWindow 精确指定绑定窗口，使用独立 D3D11 硬件设备和 FreeThreaded 帧池。
帧池在专用线程上运行，不要求宿主创建消息分发器；未绑定不会创建 WGC 会话。
GPU 上复制客户区矩形到同一组三槽快照池；CopyFrame 才读取 CPU，status/log 不读取像素。
AcquireGpuFrame 的 captureBackend 为 FS_CAIJI_WGC（3）；FsZhen.backend 为 11，表示使用 D3D11 纹理。
租约、容量检查、坐标换算、解绑失效规则与其他后端相同，旧结构布局不变。

客户区裁剪按物理像素计算；调整尺寸期间，源尺寸尚未匹配窗口边界时跳过过渡帧，
随后重建帧池并恢复。最小化时停止发布、保留最近有效快照，恢复后的更新取决于系统和目标程序。
系统可能显示采集边框，本模块保留系统默认行为；在系统支持时关闭鼠标指针采集。
不支持系统、设备丢失、会话关闭等情况通过结果码及原始 HRESULT 报告，不自动切换后端。
被系统限制捕获的窗口可能无画面；本模块不绕过限制。

## 验证

CTest 包含 C ABI、Detours 分配器、DX10/11/12 WARP 生命周期与完整帧读取、GPU 租约/池满/尺寸变化、
OBS DX10/11/12 硬件、OBS 缺依赖/窗口不匹配/已有会话，以及 WGC 普通 GDI 窗口像素、裁剪、
租约稳定性、池满恢复、尺寸变化、三后端切换和自动解绑等测试。
另有 `cpu_search`、`gpu_search` 和 `gpu_search_hardware`，通过 DX11 WARP 及硬件图案
验证九个函数、透明色、相似度、方向、坐标换算、并发字符串、Ex 上限及工作台计时。
`gpu_resource_reuse` 验证 CPU/GPU 共用处理设备、资源及模板复用、解绑并发、尺寸变化和格式一致性。
DX10/11/12、OBS 和 WGC 集成测试也调用九个 GPU 函数，并核对没有完整画面 CPU 读回。
本次记录另见 `Yanzheng.md`。特定游戏的注入兼容性、最小化行为和长期大分辨率性能需要单独实测。

## OCR、模型与字库

默认设置为 `SetOcrEngine("model")`、`SetOcrBackend("cpu")`、0 号模型，
与找图找色默认 GPU 的设置独立。0 号模型从 ZhaoTu.dll 同目录的 `ocr_models` 自动登记，
首次实际识别才加载会话。普通 OCR 不需要 Python、OpenCV 或 CUDA。
运行库和模型完整复制后不依赖原来的“ocr识别”目录。

窗口接口必须先绑定，文件接口 `OcrInFile` 无需绑定。一次调用只读取一帧；
区域采用客户区物理像素，右下角不包含。识别期间解绑或换绑会丢弃旧结果。
本期 CPU 和 DirectML OCR 都先按需取得 CPU BGRA、裁剪、预处理，再运行模型；
DirectML 加速模型推理，尚未把采集 GPU 纹理直接交给神经网络。

```text
BindWindow(0x实际窗口句柄, "dx", "normal", "normal", 0)
Ocr(0, 0, 1172, 590, "", 0.5)
OcrEx(0, 0, 1172, 590, "", 0.5)
OcrExOne(0, 0, 1172, 590, "", 0.5)
FindStr(0, 0, 1172, 590, "开始游戏|确定", "", 0.5)
FindStrS(0, 0, 1172, 590, "开始游戏|确定", "", 0.5)
FindStrEx(0, 0, 1172, 590, "开始游戏|确定", "", 0.5)
FindStrExS(0, 0, 1172, 590, "开始游戏|确定", "", 0.5)
SetOcrBackend("directml")
Ocr(0, 0, 1172, 590, "", 0.5)
GetOcrError()
```

工作台与外部代码使用同一组输入参数；不再有 intX/intY 输出指针。
全部 OCR 和找字入口返回通用结果：

| 接口 | 原生结果内容 |
|---|---|
| Ocr / OcrEx / OcrExOne / OcrInFile | fullText 全文，items 提供每个字符的 id、x、y、text |
| FindStr / FindStrS / FindStrFast / FindStrFastS | 0 或 1 个条目，提供目标 id、x、y、text |
| FindStrEx / FindStrExS / FindStrFastEx / FindStrFastExS | 全部命中条目，最多 1500 项 |

找字目标用 `|` 分隔，列表优先，不跨行拼接；坐标属于命中文字的第一个字符。
status=0/count=0 为正常无命中，错误为非零 status。结果独立分配，用 FreeResult 精确释放指定结构体，或用 FreeAllResults 全量释放。

快速版在字库引擎中只匹配待查目标涉及的字形；精确匹配按高度、首列点阵建立候选索引，
模糊匹配在超过允许误差时提前结束。点阵与索引临时内存按调用线程复用。
画面“张和三”可能被快速版命中为“张三”，因为无关的“和”未参与识别；
各行仍独立匹配。模型引擎复用既有完整推理后再过滤无关字符，不保证推理加速。
这些函数均可直接在工作台输入，返回格式及阶段计时见 HanshuShuoming.md。
fullText 为空也可能只是没有识别到文字，应先检查结果 status；GetOcrError 或 GetOcrStatus 提供详细诊断。
设置、加载、释放函数返回 1 成功 / 0 失败；GetOcrStatus 继续使用 FsJieguo，
即 0 成功，不是设置函数的布尔返回约定。

模型管理：

```text
SetOcrModel(1, "C:\模型目录")
UseOcrModel(1)
FreeOcrModel(1)
UseOcrModel(0)
SetOcrBackend("cpu")
SetOcrEngine("model")
```

索引 0～99。0 号默认目录包含 `PP-OCRv5_mobile_det`、`PP-OCRv5_mobile_rec`，
各自必须有 `model.onnx` 和 `inference.yml`。`SetOcrModel` 另外支持 PP-OCRv3 中文模型：
`models` 布局使用 `ch_PP-OCRv3_det_infer.onnx`、`ch_PP-OCRv3_rec_chs.onnx`、
`ch_ppocr_mobile_v2.0_cls_infer.onnx`、`ppocr_keys_chs.txt`；`models10` 布局使用
`detCN.onnx`、`RecCN.onnx`、`cls.onnx`、`KeysCN.txt`。v3 的英文专用识别模型当前不参与推理。
模型登记校验必要文件，实际张量结构在首次加载时校验；不是任意 ONNX 模型都可替换。
识别模型字符表随模型目录一起使用，不能用 SetDict 替换；目录版不会自动选择英文专用模型。

也可通过 `SetOcrModelFiles(index, classifier, detector, recognizer, dictionary)` 分别指定模型文件。
`classifier` 可传空字符串表示不使用方向分类；其他三个路径必需，字符字典须与识别模型对应。
该函数可显式指定英文识别模型，但适用于符合本项目 PP-OCRv3 输入、输出和前后处理约定的模型；
首次 OCR 时校验张量形状及字典类别数，识别准确度仍需用目标场景图片验证。

每个模型保留 CPU / DirectML 会话；v5 各有检测、识别两个会话，v3 各有检测、识别、方向分类三个会话。重复调用、切换采集方式或解绑不重建。
设置为相同规范目录时也不重建；修改目录内模型文件后应 FreeOcrModel，再 SetOcrModel。
DirectML 设备、队列在进程中复用；不支持的设备会明确失败，不自动切回 CPU。
FreeOcrModel 移除登记，正在进行的调用可完成，结束后释放会话；再次使用需重新登记。
CPU 和 DirectML 会话各自首次初始化，首次推理还可能触发图编译及资源分配，
因此“首次模型准备/初始化”不涵盖检测、识别阶段内部发生的所有首次开销。

点阵字库管理：

```text
SetDict(0, "C:\字库目录\ziku.txt")
UseDict(0)
GetNowDict()
GetDictCount(0)
SetOcrEngine("dict")
Ocr(0, 0, 1172, 590, "ffffff-202020", 0.9)
ClearDict(0)
SetOcrEngine("model")
```

SetDict 成功同时选中字库，但不自动切换 model/dict 引擎。索引 0～99，
路径按 SetPath 解析；替换加载失败保留旧字库，ClearDict 不删除文件。
当前字库支持参考 TSPlug 的**未加密十六进制点阵文本**，列顺序、每列固定11位、顶部为最高位，
记录为 `十六进制$文字$...$实际高度`，实际高度仅 1～11，UTF-8 或 GB18030 编码。
不是大漠加密字库的通用解析器；没有真实用户字库样本前不承诺其他编码布局。
本期用独立生成字形验证了解析和匹配，未核验用户现有字库。

颜色格式支持 `RRGGBB-DRDGDB`、`H.S.V-dH.dS.dV`、`#灰度-容差`，
最多10种，用 `|` 分隔；HSV 为 H=0～360、S/V=0～100，灰度为0～255。
前缀 `b@` 表示背景色，匹配之外的像素视为文字。模型使用空颜色保留原图；
字库空颜色使用自动阈值和边缘背景推断。模型 sim 为整行平均置信度阈值，
字库 sim 为字形笔画误差阈值，范围均为0.1～1.0，和找色的离散相似度表不同。

模型检测框采用轴对齐补边，不做倾斜文字框矫正；PP-OCRv3 分类器只负责置信度达到阈值的 180 度倒置文字。
模型字符坐标依据 CTC 预测位置估计，适合定位文字，不能视为像素精确字形框。
小字、描边、特效和低对比度画面需要用实际截图验证准确率。
工作台分开显示采集取帧、初始化、预处理、文字检测、文字识别、结果整理/找字和其他；
DirectML 检测/识别包含执行等待及数据传回，不是单纯命令提交时间。
status 显示模型和设备创建次数，不触发 OCR 或 CPU 像素读回；
GetOcrStatus 的最近结果、分段和 GetOcrError 均属于当前线程。

### 区域数据、卡屏与比色

新增 `CmpColor`、`GetScreenData`、`IsDisplayDead`、`MatchPicName`，均可直接在工作台调用。
`CmpColor` 保留原接口的 **0 匹配、1 不匹配**，支持最多10组 RGB 颜色；
按照当前 CPU/GPU 找色设置执行，GPU 分支不读回完整画面。
`GetScreenData` 返回本线程 CPU 区域像素地址（64位 `uintptr_t`），格式为紧密排列 B,G,R,0，
不含 BMP 文件头；保留到同线程下次调用或线程退出，不能释放，也不能直接作为 LoadPicByte 输入。
区域为客户区物理像素、右下角不包含，整个区域必须有效。
`IsDisplayDead` 持续 t 秒检查同一位置的像素，变化返回0、不变返回1；
解绑、重绑、销毁或采集失败返回0，具体错误查 GetFindStatus。按需复用 CPU 读回，
每约50ms检查，主动等待时间单列，不能保证发现检查之间短暂变化。
`MatchPicName` 根据 SetPath 展开文件名中的 *、? 与 |，枚举 BMP、PNG、JPG/JPEG 文件，不要求绑定。
完整原型、参数、生命周期及示例见 [HanshuShuoming.md](HanshuShuoming.md)。
