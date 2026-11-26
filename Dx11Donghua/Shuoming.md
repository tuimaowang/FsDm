# DirectX 11 动画演示

此项目是独立的 Windows x64 程序，使用 Direct3D 11 和 DXGI 交换链绘制静态网格、动画图元及中文说明。程序通过 DXGI `Present` 提交窗口画面，可用于验证项目的 DXGI 帧采集。

## 编译

在 PowerShell 中运行：

```powershell
.\Goujian.ps1
```

程序输出位置：

```text
bin\Release\DonghuaYanshi.exe
```

也可以在 Visual Studio 中打开 `Dx11Donghua.vcxproj`，选择 x64 和 Release 后生成。

## 操作

- 空格：暂停或继续立方体动画，帧提交仍继续。
- **F8** 或窗口菜单“控制 → 停止帧提交”：停止或恢复场景绘制和 DXGI `Present`。勾选状态及窗口标题会显示开关状态。
- 左方向键：降低动画速度。
- 右方向键：提高动画速度。
- **R**：恢复初始角度、默认速度、动画和帧提交。
- Esc：退出。

停止提交期间窗口仍处理键盘、菜单、缩放及关闭消息。调整过的窗口尺寸会在恢复提交后应用。

## 帧编号测试

在可调用 `GetFrameNumber(1)` 的目标进程中绑定本程序窗口，观察动画运行、按 F8 停止及恢复之后的帧编号。此示例使用 DX11/DXGI；建议选择 `BindWindow(..., "dx", ...)`，不要用 WGC 编号作为 `Present` 是否停止的唯一判断，因为 WGC 接收的是 Windows 合成后的窗口帧。
