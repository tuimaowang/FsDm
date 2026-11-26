param(
    [ValidateSet('Release', 'Debug')]
    [string]$Peizhi = 'Release'
)

$ErrorActionPreference = 'Stop'
$msbuild = 'C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe'
if (-not (Test-Path -LiteralPath $msbuild)) {
    throw '未找到 Visual Studio 2022 MSBuild；请安装“使用 C++ 的桌面开发”组件。'
}

$project = Join-Path $PSScriptRoot 'Dx11Donghua.vcxproj'
& $msbuild $project /m /v:minimal "/p:Configuration=$Peizhi" /p:Platform=x64
if ($LASTEXITCODE -ne 0) {
    throw 'DirectX 11 动画演示编译失败。'
}

$program = Join-Path $PSScriptRoot "bin\$Peizhi\DonghuaYanshi.exe"
Write-Host "编译完成：$program"
