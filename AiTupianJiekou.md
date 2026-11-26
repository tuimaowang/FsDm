# OpenCV 找图与 YOLO 接口清单

本表依据《大漠插件接口说明》的 Ai 目录整理。当前项目已实现的 OpenCV 模板匹配接口统一使用 `Cv` 前缀，YOLO 行使用说明文档中的函数名，作为后续接入参考。

| 函数 | 分类 | 功能说明 |
| --- | --- | --- |
| `CvFindPic` | OpenCV 模板匹配 | 在指定区域查找文件模板，返回第一个找到的图片序号和坐标。 |
| `CvFindPicEx` | OpenCV 模板匹配 | 在指定区域查找文件模板，返回最多 1500 个匹配结果的图片序号和坐标。 |
| `CvFindPicMem` | OpenCV 模板匹配 | 使用 `LoadPicByte` 登记的模板别名查找，返回第一个结果。 |
| `CvFindPicMemEx` | OpenCV 模板匹配 | 使用 `LoadPicByte` 登记的模板别名查找，返回最多 1500 个匹配结果。 |
| `AiYoloDetectObjects` | YOLO 检测 | 在指定区域检测对象，返回类别、置信度和检测框坐标及尺寸。 |
| `AiYoloDetectObjectsToDataBmp` | YOLO 检测 | 检测对象，并将带检测结果的 BMP 图像数据输出到内存。 |
| `AiYoloDetectObjectsToFile` | YOLO 检测 | 检测对象，并将带检测结果的 BMP 图像保存到文件。 |
| `AiYoloFreeModel` | YOLO 模型管理 | 卸载指定模型。 |
| `AiYoloObjectsToString` | YOLO 结果处理 | 将检测或排序结果中的类别信息按顺序连接成字符串。 |
| `AiYoloSetModel` | YOLO 模型管理 | 从文件加载模型，文档列出 ONNX 和 DMX 格式。 |
| `AiYoloSetModelMemory` | YOLO 模型管理 | 从内存加载模型，文档注明仅支持 DMX 格式。 |
| `AiYoloSetVersion` | YOLO 配置 | 设置 YOLO 版本，本说明文档列出的可选值为 `v5-7.0`。 |
| `AiYoloSortsObjects` | YOLO 结果处理 | 按从上到下、从左到右排列检测结果。 |
| `AiYoloUseModel` | YOLO 模型管理 | 切换当前检测使用的模型序号。 |

## 说明

- 大漠说明文档中的 AI 找图系列支持多张模板，文档要求 24 位位图，并以四角同色规则处理透明色。原版内存接口要求未压缩的 24 位位图数据。
- 当前项目的 `Cv` 系列使用 OpenCV CPU 模板匹配，支持项目现有的模板格式与透明色规则。`Mem` 版本通过 `LoadPicByte` 登记完整 BMP 文件字节，再使用别名查找。
- YOLO 检测需要先加载 AI 模块和相应模型。文档注明 ONNX 模型需要同目录下同名的 class 文件。
- OpenCV 匹配分数与大漠 AI 找图相似度的具体算法未确认一致，使用时应根据实际图片调整阈值。

当前项目已接入四个 OpenCV CPU 找图入口，参数和使用方法见 `HanshuShuoming.md`。YOLO 接口列为后续接入项。
