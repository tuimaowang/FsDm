# 导出接口测试

这是独立项目，使用自己的 CMake，只读取对外头文件，不包含或重建 DLL 工程。包含动态加载测试和直接链接交付 LIB 的截图测试。

源码：`DaochuCeshi.cpp`。

编译并运行：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\Goujian.ps1
```

生成位置：`FsDm\out\build\bin\Release\DaochuCeshi.exe`。双击运行后显示逐项结果，按回车退出；报告保存在同目录的 `DaochuCeshiJieguo.txt`。两个 DLL 的 `Peizhi.h` 中工作台标志已改为 `false`，测试程序仍显示测试结果。

命令行运行：

```powershell
.\DaochuCeshi.exe --no-pause
.\DaochuCeshi.exe --no-pause --dll-dir "C:\your\DLL目录"
```

退出码为 0 表示通过，1 表示检查失败，2 表示程序参数或运行异常。不要同时对同一目录运行多个实例，以免报告互相覆盖。

检查内容：

- 按 `FsDmJiekou.h` 检查全部公开名称能否通过 `GetProcAddress` 获取，反查 DLL 导出表是否有未声明接口，并检查已删除名称不存在。
- 通过头文件的真实函数指针类型调用查询、参数校验、未绑定读帧、全部 22 个识别入口和结果释放接口；22 个入口包括文件 OCR 的空路径校验。
- 在 CPU/GPU 两种查找设置下检查未绑定状态，核对结果结构的大小、版本、状态、条目数和空结果全文。
- 检查同函数两次结果相互独立、指定结构释放、重复释放、跨线程读取与释放、全量释放及兼容释放别名。
- 以不带控制台的子进程加载两个 DLL，等待采集初始化结束，确认没有创建或连接工作台。

这是导出与调用约定测试，不会绑定用户窗口，不验证实际采集画面、算法命中率、OCR 识别精度或性能。DLL 在本进程运行，模块固定和钩子随测试进程退出释放。

新增接口后重新运行构建脚本，导出检查表会从交付头文件重新生成；实际功能用例仍需按接口行为补充。

## 交付 LIB 与实际截图测试

新增源码 `JietuZhilianCeshi.cpp`，只包含 `Jiaofu/FsDmJiekou.h`，链接交付目录的 `ZhaoTu.lib` 和 `ZhuaTu.lib`。配置参数 `FSDM_JIAOFU_DIR` 可以指定另一个交付目录。

生成位置：`DaochuCeshi/out/build/ZhilianYanzheng/JietuZhilianCeshi.exe`。构建会把本次交付的两个 DLL 复制到 EXE 旁边；运行前可用 SHA256 与交付目录比对。EXE 使用 Windows 子系统，不显示控制台，报告保存为同目录的 `JietuZhilianJieguo.txt`，退出码 0 表示全部通过。`ctest` 会同时运行原测试和新增测试。

本测试只绑定程序自己创建的 DX11 WARP 窗口。WARP 是 Windows 软件图形设备；固定图案用于验证实际呈现、采集、裁剪和找图，不涉及用户正在运行的其他窗口。

- 核对全部公开函数能通过交付 LIB 直接导入，验证两个 DLL 的实际加载路径。
- 验证三个截图函数省略 mode 时仅保存文件，以及显式 mode=1/2/3 的行为。
- 独立解码 BMP/JPG/PNG 文件，核对格式、尺寸；BMP/PNG 逐像素核对。
- 删除 mode=3 的文件后，CPU/GPU 仍通过四个找图接口精确命中内存模板；mode=1 不生成文件。
- 核对 `FreePic` 释放后不能继续加载无文件的内存别名，并验证模板容量设置和工作台关闭。

样本使用每次运行独有的 `JietuYangben_*` 子目录，文件删除仅限本次生成的样本。测试不覆盖 OCR、OBS/WGC、其他图形设备或其他应用场景。
