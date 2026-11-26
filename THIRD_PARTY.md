# 来源与第三方依赖

采集拦截、画面读回、测试渲染夹具和事务分配器修复改编自本地参考项目：
`DmDxReplica-v1.0.4-source/DmDxImageSearch`，保留其 DX10/11/12 技术路线。
参考项目本身未发现独立许可证；本次按用户要求在同一工作区复用，
对外发行前应核实参考项目授权。

Microsoft Detours 使用参考项目附带的离线源码，版本修订：
`e4bfd6b03e50de46b47abfbd1e46b384f0c5f833`。
原文件名、版权和 MIT 许可证保留在 `third_party/Detours`。
CMake 生成独立事务分配器版本，不修改第三方原文件。
DetoursThread / DetourOperation 使用 VirtualAlloc，避免事务挂起线程时等待其持有的堆锁。


## OBS 配套文件

从工作区参考项目 `DmDxReplica-v1.0.4-source/obs-self` 复制以下原文件，未修改：

- `x64/Release/graphics-hook64.dll`，文件版本 1.8.7.0；SHA256：
  `F89ADE59480DE0D55FCC100495D78E8E491BED469FAB4DBD3D430837B58B27A5`
- `x64/Release/get-graphics-offsets64.exe`，SHA256：
  `9F53513188D545DDFDFEC6C280380BE896E44E4A44958E6B7BACB84C57746F54`
- `Obs-Screenshot/graphics_hook_protocol.h`，保留原名及 648 字节 hook_info 布局。

OBS Studio 上游为 obsproject/obs-studio。许可文本保存为 `third_party/obs/COPYING`，
交付目录中为 `licenses/OBS-COPYING.txt`。该文本来自上游 32.0.4 标签的 COPYING；
本次没有将现有二进制声称为 32.0.4 构建。参考项目未提供与这两个二进制对应的确切源码提交，
所以未验证逐字节对应的构建来源，现有上游代码仅用于核对协议和生命周期。
源代码及许可出处：

```text
https://github.com/obsproject/obs-studio
https://github.com/obsproject/obs-studio/blob/32.0.4/COPYING
https://github.com/obsproject/obs-studio/tree/32.0.4/plugins/win-capture
```

这两个 OBS 二进制仅依赖 Windows 系统 DLL；统一交付目录不包含注入助手、截图宿主、
32 位文件或 PDB。第三方文件名、许可文本和来源记录均保留。

## WGC

WGC 适配代码位于 `ZhuaTu/WgcCaiji.cpp`，直接调用 Windows SDK 的
Windows Graphics Capture / C++/WinRT API，使用操作系统组件，不新增第三方二进制。
已检查用户指定的 `DmDxReplica-v1.0.4-source/obs-handle-realese`：
其中现有采集路径为 OBS 共享纹理，未找到可复用的 WGC 实现，因此没有复制其截图、
匹配或 OpenCV 文件。此次 WGC 适配与测试为本项目新增代码。

## TSPlug 查找逻辑参考

本次阅读工作区 `DmDxReplica-v1.0.4-source/TSPlug4.19开源/TSPlug/TSFindPicture.cpp`，
对照其 findPicture / processColor 及 DXBind.cpp 的对应入口，使用本项目的 BGRA 帧和 WIC
重新编写六个接口。未直接复制源码文件、二进制或依赖；算法语义参考包括离散相似度表
（.5/.6/.7/.8/.9 对应 49/38/29/18/9）、逐通道判断、任一有效像素不合格即排除、
颜色/图片列表优先、方向遍历、螺旋及四角透明规则、FindPicS 的实际 file,x,y 格式。
源码分支对应关系及有意修正的越界、反向索引和边缘差异见 README，
不声称完整兼容旧源码缺陷。先前版本的连续相似度/像素匹配比例规则已被替换。

## OCR 运行库与模型

原生 x64 运行库从官方 NuGet 包提取，保留对应头文件和许可证，
无 Python、.NET managed 组件或 CUDA 依赖：

- `Microsoft.ML.OnnxRuntime.DirectML` 1.24.4，
  nuspec 记录提交 `2d924974ef147392ced8409d36bd6d2e7fcc8a74`，MIT。
- `Microsoft.AI.DirectML` 1.15.4，为上述包声明的配套依赖；
  保留其原始许可证和第三方材料，不将其许可概括为 MIT。
- 官方下载地址及所选版本固定记录于 `third_party/onnxruntime/Laiyuan.md`。

SHA256：

```text
onnxruntime.dll E7EEDEC6A6F26DC39DC948276A75EF6D2BEE3FFF944D874CEED0BBD3B97BFF40
onnxruntime_providers_shared.dll 265C8DAF29637CB259CAC8BE9F08F2CD45F3883F0F0E4949CBFDDD5B4CBEC3B6
DirectML.dll 9C9E6D822561C6C41B90E6994B3E8857CF1D66DBFB1E0C4C799C7C89B4E92DA1
```

PP-OCRv5 mobile 检测和识别 ONNX、YAML 从用户既有
`ocr识别/CMakeWorkspace/out/onnx_ocr_models` 复制，文件未修改。
模型结构和原生管线参考该项目 Git 中的 OnnxOcrBridge.cpp，
不恢复或修改用户已删除的原项目文件。模型来源为 PaddleOCR（Apache-2.0），
许可保留在 `ocr_models/LICENSE`；既有 ONNX 转换环境/提交未提供，不声称可逐字节复现导出。

```text
PP-OCRv5_mobile_det/model.onnx A431985659DC921974177A95ADCFBB90FD9E51989A5E04D70D0B75F597B6E61D
PP-OCRv5_mobile_rec/model.onnx DA72DC72CA4DC220DF0DFDE68C1DEDC31C58D3E76A25871122E5056227D50092
https://github.com/PaddlePaddle/PaddleOCR
```

字库格式依据 TSPlug 的 dicthextobyte/byteTopic 解析代码。参考中的
comparaStr/GetStringInfo 为占位实现；本项目另写边界受控的二值匹配。
没有复制或引入 TSPlug DLL，不声称支持所有大漠/TSPlug 加密字库。
