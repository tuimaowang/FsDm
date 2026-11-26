# OCR 运行库来源

提取日期：2026-11-14。仅保留本项目需要的原生 x64 DLL、公开头文件、许可证。
首次加载由 ZhaoTu.dll 所在目录定位，编译不从网络下载。

```text
Microsoft.ML.OnnxRuntime.DirectML 1.24.4
https://api.nuget.org/v3-flatcontainer/microsoft.ml.onnxruntime.directml/1.24.4/microsoft.ml.onnxruntime.directml.1.24.4.nupkg
原生 DLL：runtimes/win-x64/native
公开头文件：build/native/include
源码提交：2d924974ef147392ced8409d36bd6d2e7fcc8a74

Microsoft.AI.DirectML 1.15.4
https://api.nuget.org/v3-flatcontainer/microsoft.ai.directml/1.15.4/microsoft.ai.directml.1.15.4.nupkg
DirectML.dll：bin/x64-win/DirectML.dll
头文件：include/DirectML.h、include/DirectMLConfig.h
```

配套版本由 ONNX Runtime 包的 nuspec 声明。没有引入其中的托管 .NET 代码。
licenses 内保留 ONNX Runtime、DirectML 的许可证及第三方声明，交付目录一并复制。
