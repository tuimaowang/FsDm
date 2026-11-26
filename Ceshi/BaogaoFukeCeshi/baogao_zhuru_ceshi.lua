--[[
    本脚本用于已经注入 LuaJiekouQiaojie.dll、并且自身运行在目标测试进程内的 LuaJIT。
    脚本通过 FFI 调用独立桥接 DLL；桥接 DLL 再转调同进程内加载的原版 ZhaoTu.dll。
    本脚本不调用 os.exit，不会主动结束宿主测试进程。

    CPU 时间来自 GetProcessTimes，统计的是宿主测试进程中所有线程的 CPU 累计时间；
    因此它包含被测 DLL 和宿主进程其他线程的消耗，不等同于单个函数的独占 CPU 时间。
    测试摘要写入本目录的 baogao_zhuru_jieguo.txt，方便用 VS Code 查看。
]]

local ffi = require("ffi")

ffi.cdef[[
    typedef void* HWND;

    typedef struct FsJieguoXiang {
        uint32_t fields;
        int32_t id;
        int32_t x;
        int32_t y;
        uint32_t color;
        const char* text;
        const char* file;
    } FsJieguoXiang;

    typedef struct FsTongyongJieguo {
        uint32_t size;
        uint32_t version;
        int32_t status;
        int32_t detail;
        uint32_t count;
        const FsJieguoXiang* items;
        const char* fullText;
    } FsTongyongJieguo;

    int __stdcall QjInitialize(const char* jiaofuDirectoryUtf8);
    int __stdcall QjShutdown(void);
    const char* __stdcall QjGetLastError(void);
    int __stdcall QjBindWindow(uintptr_t hwnd, const char* display);
    int __stdcall QjUnbindWindow(void);
    int __stdcall QjSetFindBackend(const char* backend);
    int __stdcall QjSetPath(const char* path);
    const FsTongyongJieguo* __stdcall QjFindColorEx(
        int x1, int y1, int x2, int y2, const char* color, double similarity, int direction);
    const FsTongyongJieguo* __stdcall QjFindPic(
        int x1, int y1, int x2, int y2, const char* name,
        const char* deltaColor, double similarity, int direction);
    const FsTongyongJieguo* __stdcall QjOcr(
        int x1, int y1, int x2, int y2, const char* colorFormat, double similarity);
    int __stdcall QjSetOcrEngine(const char* engine);
    int __stdcall QjSetOcrBackend(const char* backend);
    int __stdcall QjSetOcrModel(int index, const char* modelDirectory);
    int __stdcall QjUseOcrModel(int index);
    int __stdcall QjFreeResult(const FsTongyongJieguo* result);
    int __stdcall QjGetProcessCpuTime100ns(unsigned long long* cpuTime);
    unsigned int __stdcall QjGetLogicalProcessorCount(void);

    int __stdcall QueryPerformanceCounter(int64_t* value);
    int __stdcall QueryPerformanceFrequency(int64_t* value);
]]

local function join_path(...)
    -- Windows 路径统一使用反斜线，并折叠拼接时意外形成的重复分隔符。
    local path = table.concat({...}, "\\")
    return (path:gsub("\\+", "\\"))
end

local function script_directory()
    -- 以脚本实际位置定位桥接 DLL、模型和日志文件，不依赖目标进程当前目录。
    local source = debug.getinfo(1, "S").source
    if source:sub(1, 1) == "@" then
        source = source:sub(2)
    end
    return source:match("^(.*)[/\\][^/\\]+$") or "."
end

local function default_config()
    local root = script_directory()
    local fsdm = join_path(root, "..", "..")
    return {
        -- 把目标窗口 HWND 写成十六进制字符串。必须是当前测试进程拥有的窗口。
        hwnd = "",
        display = "wgc",
        find_backend = "cpu",
        ocr_backend = "cpu",
        bridge_path = join_path(root, "bin", "qiaojie", "x64", "Release",
                                "LuaJiekouQiaojie.dll"),
        jiaofu_dir = join_path(fsdm, "linshi", "jiaofu"),
        template_dir = "C:\\Users\\Administrator\\Desktop\\report",
        model_dir = join_path(fsdm, "third_party", "ocr_models"),
        log_path = join_path(root, "baogao_zhuru_jieguo.txt"),
        find_color_iterations = 100,
        find_pic_iterations = 5000,
        ocr_iterations = 100,
    }
end

local function get_config()
    -- 若宿主 Lua 已设置全局 BAOGAO_CESHI_CONFIG，则用其中的字段覆盖默认参数。
    local config = default_config()
    local override = rawget(_G, "BAOGAO_CESHI_CONFIG")
    if type(override) == "table" then
        for key, value in pairs(override) do
            config[key] = value
        end
    end
    return config
end

local function assert_call(ok, bridge, message)
    if ok == 0 then
        local detail = bridge and bridge.QjGetLastError() or nil
        if detail ~= nil and detail ~= ffi.NULL then
            message = message .. "；桥接错误：" .. ffi.string(detail)
        end
        error(message, 0)
    end
end

local function get_clock()
    -- 单调高精度时钟用来计算墙钟耗时；与进程 CPU 时间配对后可比较 CPU 使用情况。
    local frequency = ffi.new("int64_t[1]")
    assert_call(ffi.C.QueryPerformanceFrequency(frequency),
                nil, "读取高精度计时器频率失败。")
    return function()
        local counter = ffi.new("int64_t[1]")
        assert_call(ffi.C.QueryPerformanceCounter(counter),
                    nil, "读取高精度计时器计数失败。")
        return tonumber(counter[0]) / tonumber(frequency[0])
    end
end

local function read_process_cpu(bridge)
    -- 进程累计 CPU 时间以 100 纳秒为单位，包含进程内全部线程的用户态和内核态用时。
    local value = ffi.new("unsigned long long[1]")
    assert_call(bridge.QjGetProcessCpuTime100ns(value), bridge,
                "读取目标进程 CPU 时间失败。")
    return tonumber(value[0]) / 10000000
end

local function create_report(config)
    local file, err = io.open(config.log_path, "w")
    if not file then
        error("无法创建测试日志：" .. tostring(err) .. "；路径：" .. config.log_path, 0)
    end
    local report = {file = file}
    function report:write(text)
        io.write(text)
        self.file:write(text)
        self.file:flush()
    end
    return report
end

local function run_timed_case(report, bridge, clock, processor_count,
                              label, iterations, operation)
    -- 计时区间覆盖完整循环。每轮即刻释放结果，避免测试本身造成结果对象堆积。
    local failed = 0
    local first_error = nil
    local wall_start = clock()
    local cpu_start = read_process_cpu(bridge)

    for _ = 1, iterations do
        local ok, err = pcall(operation)
        if not ok then
            failed = failed + 1
            first_error = first_error or tostring(err)
        end
    end

    local cpu_end = read_process_cpu(bridge)
    local wall_seconds = clock() - wall_start
    local cpu_seconds = math.max(0, cpu_end - cpu_start)
    local one_core_percent = wall_seconds > 0 and cpu_seconds / wall_seconds * 100 or 0
    local machine_percent = one_core_percent / processor_count
    local line = string.format(
        "%s：%d 轮，墙钟 %.3f 秒，进程 CPU %.3f 秒，平均 %.3f 毫秒/轮，" ..
        "约 %.1f%% 单核 / %.1f%% 整机",
        label, iterations, wall_seconds, cpu_seconds,
        wall_seconds * 1000 / iterations, one_core_percent, machine_percent)
    if failed > 0 then
        line = line .. string.format("；失败 %d 轮；首错：%s", failed, first_error or "未知错误")
    end
    report:write(line .. "\n")
end

local function check_and_release(bridge, result, operation_name)
    -- 通用结果的内存归属 ZhaoTu.dll，必须由同一 DLL 的 FreeResult 释放。
    if result == nil or result == ffi.NULL then
        error(operation_name .. " 返回空结果。", 0)
    end
    local status = tonumber(result.status)
    local detail = tonumber(result.detail)
    local free_ok = bridge.QjFreeResult(result)
    if free_ok == 0 then
        error(operation_name .. " 的结果释放失败。", 0)
    end
    if status ~= 0 then
        error(string.format("%s 执行失败：status=%d detail=%d",
                            operation_name, status, detail), 0)
    end
end

local function run()
    local config = get_config()
    local report = create_report(config)
    local bridge = nil
    local is_bound = false
    local result_code = 0

    local ok, err = xpcall(function()
        -- 加载的是新建的桥接 DLL；桥接层之后再从 jiaofu 目录加载原版 ZhaoTu.dll。
        bridge = ffi.load(config.bridge_path)
        assert_call(bridge.QjInitialize(config.jiaofu_dir), bridge,
                    "初始化桥接 DLL 或加载 jiaofu 中的 ZhaoTu.dll 失败。")

        local clock = get_clock()
        local processor_count = tonumber(bridge.QjGetLogicalProcessorCount())
        if processor_count < 1 then processor_count = 1 end

        local hwnd_number = tonumber(config.hwnd)
        assert_call(hwnd_number ~= nil and hwnd_number ~= 0 and 1 or 0,
                    bridge, "请在脚本配置中填写有效的目标窗口 HWND。")
        assert_call(bridge.QjBindWindow(
            ffi.cast("uintptr_t", hwnd_number), config.display), bridge,
            "绑定当前测试进程窗口失败。")
        is_bound = true

        assert_call(bridge.QjSetFindBackend(config.find_backend), bridge,
                    "设置找色/找图后端失败。")
        assert_call(bridge.QjSetPath(config.template_dir), bridge,
                    "设置模板目录失败。")

        report:write("测试进程内 LuaJIT 桥接测试\n")
        report:write("处理器逻辑数：" .. processor_count .. "\n")
        report:write("HWND：" .. tostring(config.hwnd) ..
                     "；采集方式：" .. tostring(config.display) .. "\n")
        report:write("CPU 百分比为区间内进程 CPU 时间 / 墙钟时间；整机比例再除以逻辑处理器数。\n\n")

        -- 沿用报告的 100 轮、每轮两种颜色串；CPU 时间取自注入的目标进程。
        run_timed_case(report, bridge, clock, processor_count,
            "找色 FindColorEx（每轮两次调用）", config.find_color_iterations,
            function()
                local result = bridge.QjFindColorEx(
                    61, 34, 80, 53, "F5F5F5-0A0A0A", 1.0, 0)
                check_and_release(bridge, result, "FindColorEx 第一组颜色")
                result = bridge.QjFindColorEx(
                    61, 34, 80, 53, "37843D-27242F|63A336-1A1D1A", 1.0, 0)
                check_and_release(bridge, result, "FindColorEx 第二组颜色")
            end)

        local home = io.open(join_path(config.template_dir, "主页.bmp"), "rb")
        local apps = io.open(join_path(config.template_dir, "应用.bmp"), "rb")
        local has_home = home ~= nil
        local has_apps = apps ~= nil
        if home then home:close() end
        if apps then apps:close() end

        if has_home and has_apps then
            -- 报告为 5000 轮，每轮各找一张模板，模板目录沿用上面的 SetPath。
            run_timed_case(report, bridge, clock, processor_count,
                "找图 FindPic（每轮两次调用）", config.find_pic_iterations,
                function()
                    local result = bridge.QjFindPic(
                        97, 108, 130, 129, "主页.bmp", "000000", 1.0, 0)
                    check_and_release(bridge, result, "FindPic 主页.bmp")
                    result = bridge.QjFindPic(
                        162, 162, 195, 183, "应用.bmp", "000000", 1.0, 0)
                    check_and_release(bridge, result, "FindPic 应用.bmp")
                end)
        else
            report:write("跳过找图：模板目录缺少 " ..
                (not has_home and "主页.bmp " or "") ..
                (not has_apps and "应用.bmp" or "") .. "\n")
        end

        assert_call(bridge.QjSetOcrEngine("model"), bridge,
                    "选择 OCR 模型引擎失败。")
        assert_call(bridge.QjSetOcrBackend(config.ocr_backend), bridge,
                    "设置 OCR 后端失败。")
        assert_call(bridge.QjSetOcrModel(0, config.model_dir), bridge,
                    "登记 PP-OCRv5 模型目录失败。")
        assert_call(bridge.QjUseOcrModel(0), bridge,
                    "选择 OCR 模型失败。")

        -- 直接对目标进程当前绑定窗口做 OCR，不再读取静态图片；坐标对应报告区域。
        run_timed_case(report, bridge, clock, processor_count,
            "窗口 OCR 区域一（Ocr）", config.ocr_iterations,
            function()
                local result = bridge.QjOcr(96, 108, 131, 129, "", 0.3)
                check_and_release(bridge, result, "Ocr 区域一")
            end)
        run_timed_case(report, bridge, clock, processor_count,
            "窗口 OCR 区域二（Ocr）", config.ocr_iterations,
            function()
                local result = bridge.QjOcr(96, 343, 198, 366, "", 0.3)
                check_and_release(bridge, result, "Ocr 区域二")
            end)
    end, debug.traceback)

    if bridge ~= nil then
        if is_bound then
            local unbind_ok, unbind_err = pcall(function()
                assert_call(bridge.QjUnbindWindow(), bridge, "解绑窗口失败。")
            end)
            if not unbind_ok then
                report:write("解绑错误：" .. tostring(unbind_err) .. "\n")
                result_code = 1
            end
        end
        pcall(function() bridge.QjShutdown() end)
    end

    if not ok then
        report:write("测试错误：" .. tostring(err) .. "\n")
        result_code = 1
    end

    report.file:close()
    return result_code
end

return run()
