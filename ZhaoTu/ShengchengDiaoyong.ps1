# 从公开头文件生成工作台调用表；遇到不支持的参数类型时仍列出函数，但不尝试调用。
param([string]$Toutou, [string]$Shuchu)
$ErrorActionPreference = 'Stop'
$text = [IO.File]::ReadAllText($Toutou)
$lines = [System.Collections.Generic.List[string]]::new()
$lines.Add('// 自动生成；请修改 ZhaoTu.h 而不是本文件。')
foreach ($match in [regex]::Matches($text, 'ZH_API\s+(int|double|uint64_t|uintptr_t|const\s+char\s*\*|const\s+FsTongyongJieguo\s*\*)\s+WINAPI\s+(\w+)\s*\(([^)]*)\)\s*;')) {
    $name = $match.Groups[2].Value
    $raw = $match.Groups[3].Value.Trim()
    $parameters = if ($raw -eq 'void' -or $raw -eq '') { @() } else { @($raw -split ',') }
    $types = @()
    foreach ($parameter in $parameters) {
        $p = $parameter.Trim() -replace '\s+ZH_CAPTURE_MOREN$', ''
        if ($p -match '^const\s+char\s*\*\s*\w+$') { $types += 'string' }
        elseif ($p -match '^HWND\s+\w+$') { $types += 'hwnd' }
        elseif ($p -match '^int\s+\w+$') { $types += 'int' }
        elseif ($p -match '^uintptr_t\s+\w+$') { $types += 'address' }
        elseif ($p -match '^const\s+FsTongyongJieguo\s*\*\s*\w+$') { $types += 'result' }
        elseif ($p -match '^double\s+\w+$') { $types += 'double' }
        # 只为明确命名的坐标输出分配变量，不能把任意指针当作输出参数。
        elseif ($p -match '^int\s*\*\s*(intX|intY|outX|outY)$') { $types += 'coordinate' }
        else { $types += 'unsupported' }
    }
    $lines.Add("if (name == L`"$name`") {")
    if ($types -contains 'unsupported') {
        $lines.Add('    FsConsoleLine(output, L"此函数含有暂不支持的参数类型，无法从工作台调用。"); return true;')
    } else {
        $inputCount = @($types | Where-Object { $_ -ne 'coordinate' }).Count
        # 三个截图接口允许省略末尾模式，工作台明确补入与头文件一致的默认值。
        $optionalMode = $name -in @('Capture', 'CaptureJpg', 'CapturePng')
        if ($optionalMode) {
            $lines.Add("    if (args.size() != $inputCount && args.size() != $($inputCount - 1)) { FsConsoleLine(output, L`"参数数量错误，请参照公开函数声明。`"); return true; }")
        } else {
            $lines.Add("    if (args.size() != $inputCount) { FsConsoleLine(output, L`"参数数量错误，请参照公开函数声明。`"); return true; }")
        }
        $call = @()
        $coordinates = @()
        $inputIndex = 0
        for ($i = 0; $i -lt $types.Count; $i++) {
            switch ($types[$i]) {
                'string' {
                    $lines.Add("    std::string p$i; if (!ParseText(args[$inputIndex], p$i)) { FsConsoleLine(output, L`"字符串参数必须使用双引号。`"); return true; }")
                    $call += "p$i.c_str()"
                }
                'int' {
                    if ($optionalMode -and $i -eq ($types.Count - 1)) {
                        $lines.Add("    int p$i = 2; if (args.size() > $inputIndex && !ParseInt(args[$inputIndex], p$i)) { FsConsoleLine(output, L`"整数参数无效或超出范围。`"); return true; }")
                    } else {
                        $lines.Add("    int p$i{}; if (!ParseInt(args[$inputIndex], p$i)) { FsConsoleLine(output, L`"整数参数无效或超出范围。`"); return true; }")
                    }
                    if ($name -eq 'GetFrameNumber') {
                        $lines.Add("    if (p$i != 0 && p$i != 1) { FsConsoleLine(output, L`"输出开关只能为 0 或 1。`"); return true; }")
                    }
                    $call += "p$i"
                }
                'hwnd' {
                    $lines.Add("    HWND p$i{}; if (!ParseHwnd(args[$inputIndex], p$i)) { FsConsoleLine(output, L`"窗口句柄无效。`"); return true; }")
                    $call += "p$i"
                }
                'address' {
                    $lines.Add("    uintptr_t p$i{}; if (!ParseAddress(args[$inputIndex], p$i)) { FsConsoleLine(output, L`"内存地址无效或超出范围。`"); return true; }")
                    $call += "p$i"
                }
                'result' {
                    $lines.Add("    uintptr_t p$i{}; if (!ParseAddress(args[$inputIndex], p$i)) { FsConsoleLine(output, L`"结果地址无效。`"); return true; }")
                    $call += "reinterpret_cast<const FsTongyongJieguo*>(p$i)"
                }
                'double' {
                    $lines.Add("    double p$i{}; if (!ParseDouble(args[$inputIndex], p$i)) { FsConsoleLine(output, L`"小数参数无效。`"); return true; }")
                    $call += "p$i"
                }
                'coordinate' {
                    $lines.Add("    int p$i = -1;")
                    $call += "&p$i"
                    $label = [regex]::Match($parameters[$i], '(intX|intY|outX|outY)$').Value
                    $coordinates += "{L`"$label`", p$i}"
                }
            }
            if ($types[$i] -ne 'coordinate') { $inputIndex++ }
        }
        $joined = $call -join ', '
        # 编号监视自行输出纯编号，关闭时静默完成；工作台不再追加返回值和耗时。
        if ($name -eq 'GetFrameNumber') {
            $lines.Add("    (void)GetFrameNumber($joined); return true;")
            $lines.Add('}')
            continue
        }
        # 每次导出调用独立计时，非查找函数也清空记录，避免打印上一次查找的分段。
        $lines.Add('    fsdm::haoshi::Reset();')
        # 识字和找字共用 OCR 流程，工作台在函数调用前收集占用率起点，返回后立即收集终点。
        $ocrCall = $name -in @('Ocr', 'OcrEx', 'OcrExOne', 'OcrInFile',
            'FindStr', 'FindStrS', 'FindStrEx', 'FindStrExS',
            'FindStrFast', 'FindStrFastS', 'FindStrFastEx', 'FindStrFastExS')
        if ($ocrCall) {
            $lines.Add('    fsdm::zhanyong::CallSample usageSample;')
        }
        # 只统计函数本身的执行时间，不包含参数解析和工作台输出。
        $lines.Add('    const auto start = std::chrono::steady_clock::now();')
        $lines.Add("    const auto result = $name($joined);")
        $lines.Add('    const double elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();')
        if ($ocrCall) {
            $lines.Add('    const auto usage = usageSample.Finish(elapsed);')
        }
        # x64 下 uint64_t 与 uintptr_t 可能是同一种 C++ 类型，按公开声明区分编号和地址。
        # 编号完整显示十进制；原有地址返回值继续使用工作台的十六进制显示。
        $printFunction = if ($match.Groups[1].Value -eq 'uint64_t') { 'PrintCallResult<true>' } else { 'PrintCallResult' }
        $usageArgument = if ($ocrCall) { ', &usage' } else { '' }
        $lines.Add("    $printFunction(output, result, elapsed, {$($coordinates -join ', ')}$usageArgument); return true;")
    }
    $lines.Add('}')
}
if ($lines.Count -eq 1) { throw '未找到公开函数声明。' }
[IO.File]::WriteAllLines($Shuchu, $lines, [Text.UTF8Encoding]::new($false))
