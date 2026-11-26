# 独立构建导出测试程序；DLL 需事先在原工程编译，本脚本不修改和重建 DLL 工程。
param([switch]$BuCeshi)
$ErrorActionPreference = 'Stop'
# 优先使用 VS 配套的 CMake，并使用同目录 CTest，避免 PATH 中另一版本
# 与已配置的 Visual Studio 构建目录不兼容，导致用户复跑时在配置阶段失败。
$vsCmake = 'C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
$found = Get-Command cmake -ErrorAction SilentlyContinue
$cmakeExe = if (Test-Path -LiteralPath $vsCmake) { $vsCmake } elseif ($found) { $found.Source } else { '' }
if (-not (Test-Path -LiteralPath $cmakeExe)) { throw '未找到 CMake。' }
$buildDir = Join-Path $PSScriptRoot 'out\build'
& $cmakeExe -S $PSScriptRoot -B $buildDir -G 'Visual Studio 17 2022' -A x64
if ($LASTEXITCODE) { throw '独立测试项目配置失败。' }
& $cmakeExe --build $buildDir --config Release --parallel
if ($LASTEXITCODE) { throw '独立测试项目编译失败；若输出文件被占用，请先退出测试程序。' }
if (-not $BuCeshi) {
    & (Join-Path (Split-Path $cmakeExe) 'ctest.exe') --test-dir $buildDir -C Release --output-on-failure
    if ($LASTEXITCODE) { throw '测试未通过，请查看 DaochuCeshiJieguo.txt 和 out\build\ZhilianYanzheng\JietuZhilianJieguo.txt。' }
}
