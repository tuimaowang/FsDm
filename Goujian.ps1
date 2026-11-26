# 统一构建两个 DLL、执行测试，再把交付文件安装到统一 dll 和各自 dist 目录。
param(
    [ValidateSet('Release', 'Debug')][string]$Peizhi = 'Release',
    [string]$GoujianMulu = 'out/build'
)
$ErrorActionPreference = 'Stop'
$cmakePath = Get-Command cmake -ErrorAction SilentlyContinue
if ($cmakePath) { $cmakeExe = $cmakePath.Source }
else {
    $cmakeExe = 'C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
}
if (-not (Test-Path -LiteralPath $cmakeExe)) { throw '未找到 CMake，请安装 Visual Studio C++ 桌面开发组件。' }
$buildPath = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot $GoujianMulu))
& $cmakeExe -S $PSScriptRoot -B $buildPath -G 'Visual Studio 17 2022' -A x64
if ($LASTEXITCODE) { throw 'CMake 配置失败。' }
& $cmakeExe --build $buildPath --config $Peizhi --parallel
if ($LASTEXITCODE) { throw '编译失败。' }
$ctestExe = Join-Path (Split-Path $cmakeExe) 'ctest.exe'
& $ctestExe --test-dir $buildPath -C $Peizhi --output-on-failure
if ($LASTEXITCODE) { throw '测试失败，未生成交付文件。' }
& $cmakeExe --install $buildPath --config $Peizhi --prefix $PSScriptRoot
if ($LASTEXITCODE) { throw '交付文件安装失败。' }
