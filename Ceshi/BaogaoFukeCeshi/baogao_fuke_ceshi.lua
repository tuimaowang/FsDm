--[[
    本脚本把 TestReport.md 中的找色、找图和 OCR 性能案例移植到 LuaJIT。
    LuaJIT FFI（外部函数接口）负责按 C ABI 直接调用 jiaofu 目录中的 ZhaoTu.dll；
    本脚本只调用现有 DLL 导出函数，不编译、不替换或修改任何 DLL。

    找色和找图需要提供目标窗口 HWND；OCR 使用报告附带的 BMP 文件，不需要绑定窗口。
    当前 DLL 使用 FindColorEx、FindPic、OcrInFile 等导出名，和报告中的 api_* 脚本包装名不同，
    因此此处按相同坐标、颜色、模板名和循环次数改用当前 DLL 的原生接口。
]]

local ffi = require("ffi")
local bit = require("bit")

ffi.cdef[[
    typedef void* HWND;
    typedef int BOOL;

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

    BOOL __stdcall SetDllDirectoryA(const char* path);
    BOOL __stdcall QueryPerformanceCounter(int64_t* value);
    BOOL __stdcall QueryPerformanceFrequency(int64_t* value);

    int __stdcall BindWindow(HWND hwnd, const char* display, const char* mouse,
                             const char* keypad, int mode);
    int __stdcall UnBindWindow(void);
    int __stdcall SetFindBackend(const char* backend);
    int __stdcall SetPath(const char* path);
    int __stdcall FreeResult(const FsTongyongJieguo* result);
    const FsTongyongJieguo* __stdcall FindColorEx(
        int x1, int y1, int x2, int y2, const char* color, double sim, int dir);
    const FsTongyongJieguo* __stdcall FindPic(
        int x1, int y1, int x2, int y2, const char* pic_name,
        const char* delta_color, double sim, int dir);

    int __stdcall SetOcrModel(int index, const char* model_dir);
    int __stdcall UseOcrModel(int index);
    int __stdcall SetOcrBackend(const char* backend);
    int __stdcall SetOcrEngine(const char* engine);
    const FsTongyongJieguo* __stdcall OcrInFile(
        int x1, int y1, int x2, int y2, const char* pic_name,
        const char* color_format, double sim);
]]

local function path_join(...)
    -- 统一用反斜线构造 Windows 路径；避免当前工作目录不同导致资源找错位置。
    local values = {...}
    local result = table.concat(values, "\\")
    result = result:gsub("\\+", "\\")
    return result
end

local function script_directory()
    -- 从脚本自身的加载路径取目录，不依赖 VS Code 当前终端所在的目录。
    local source = debug.getinfo(1, "S").source
    if source:sub(1, 1) == "@" then
        source = source:sub(2)
    end
    return source:match("^(.*)[/\\][^/\\]+$") or "."
end

local function parse_options(arguments)
    -- 所有参数采用 --name value 形式；布尔选项不带后续值。
    local options = {
        backend = "cpu",
        display = "wgc",
        ["ocr-backend"] = nil,
        ["skip-window"] = false,
        ["skip-ocr"] = false,
    }
    local index = 1
    while index <= #arguments do
        local key = arguments[index]
        if key == "--skip-window" or key == "--skip-ocr" or
           key == "--help" or key == "-h" then
            options[key:sub(3)] = true
            index = index + 1
        elseif key:sub(1, 2) == "--" and index < #arguments then
            options[key:sub(3)] = arguments[index + 1]
            index = index + 2
        else
            error("无法识别的命令行参数: " .. tostring(key))
        end
    end
    if not options["ocr-backend"] then
        options["ocr-backend"] = options.backend == "gpu" and "directml" or "cpu"
    end
    return options
end

local function check(ok, message)
    if not ok then
        error(message, 0)
    end
end

local function release_result(dm, result)
    -- 结果对象由 DLL 分配并持有，必须通过 DLL 的 FreeResult 释放。
    if result ~= nil and result ~= ffi.NULL then
        dm.FreeResult(result)
    end
end

local function high_precision_clock()
    -- 使用 Windows 单调高精度计时器，避免 os.time() 只有整秒精度。
    local frequency = ffi.new("int64_t[1]")
    check(ffi.C.QueryPerformanceFrequency(frequency) ~= 0,
          "读取高精度计时器频率失败。")
    return function()
        local counter = ffi.new("int64_t[1]")
        check(ffi.C.QueryPerformanceCounter(counter) ~= 0,
              "读取高精度计时器计数失败。")
        return tonumber(counter[0]) / tonumber(frequency[0])
    end
end

local function run_timed_case(clock, dm, label, iterations, operation)
    -- 每次调用都立即释放 DLL 返回的结果，避免长循环因测试代码未释放结果而累积内存。
    local failures = 0
    local first_error = nil
    local start_time = clock()
    for _ = 1, iterations do
        local result = operation()
        if result == nil or result == ffi.NULL then
            failures = failures + 1
            first_error = first_error or "DLL 返回空结果"
        else
            if result.status ~= 0 then
                failures = failures + 1
                first_error = first_error or
                    ("DLL 状态错误 status=" .. tonumber(result.status) ..
                     " detail=" .. tonumber(result.detail))
            end
            release_result(dm, result)
        end
    end
    local elapsed = clock() - start_time
    io.write(string.format("%s：%d 次，%.4f 秒，平均 %.3f 毫秒/次",
        label, iterations, elapsed, elapsed * 1000 / iterations))
    if failures > 0 then
        io.write(string.format("；失败 %d 次；首个错误：%s", failures, first_error))
    end
    io.write("\n")
end

local function get_window_handle(value)
    -- tonumber 支持十进制和 0x 开头的十六进制 HWND，再转成指针避免 x64 截断。
    if not value then
        return nil
    end
    local number = tonumber(value)
    if not number then
        error("窗口句柄无效，请传十进制或 0x 开头的十六进制数。")
    end
    return ffi.cast("HWND", ffi.cast("uintptr_t", number))
end

local function print_usage()
    io.write([[
用法：
  luajit baogao_fuke_ceshi.lua --hwnd 0x00123456 [选项]
  luajit baogao_fuke_ceshi.lua --skip-window [选项]

选项：
  --dll <路径>                 ZhaoTu.dll；默认使用 FsDm\linshi\jiaofu 中的 DLL
  --template-dir <目录>        找图模板目录；默认桌面 report 目录
  --image <BMP路径>            OCR 图片；默认本项目 assets\test.bmp
  --model-dir <目录>           OCR 模型；默认 FsDm\third_party\ocr_models
  --backend cpu|gpu            找色/找图后端，默认 cpu
  --ocr-backend cpu|directml   OCR 后端；默认随 --backend 选择
  --display dx|obs|wgc         窗口采集方式，默认 wgc
  --skip-window                跳过窗口绑定、找色和找图，只运行文件 OCR
  --skip-ocr                   跳过 OCR

找图模板目录需要包含 主页.bmp 和 应用.bmp；没有模板时程序会跳过找图。
]])
end

local function main()
    local options = parse_options(arg)
    if options.help or options.h then
        print_usage()
        return 0
    end

    local project_dir = script_directory()
    local fsdm_dir = path_join(project_dir, "..", "..")
    local jiaofu_dir = path_join(fsdm_dir, "linshi", "jiaofu")
    local report_dir = "C:\\Users\\Administrator\\Desktop\\report"
    local dll_path = options.dll or path_join(jiaofu_dir, "ZhaoTu.dll")
    local template_dir = options["template-dir"] or report_dir
    local image_path = options.image or path_join(project_dir, "assets", "test.bmp")
    local model_dir = options["model-dir"] or path_join(fsdm_dir, "third_party", "ocr_models")

    -- ZhaoTu.dll 的依赖 DLL 与它放在同一个 jiaofu 目录；先把该目录加入 Windows
    -- 的 DLL 搜索目录，再加载 ZhaoTu.dll，确保使用的是交付目录中的配套版本。
    check(ffi.C.SetDllDirectoryA(jiaofu_dir) ~= 0,
          "设置 jiaofu DLL 搜索目录失败: " .. jiaofu_dir)
    local dm = ffi.load(dll_path)
    local clock = high_precision_clock()
    local bound = false
    local exit_code = 0

    local ok, message = xpcall(function()
        if not options["skip-window"] then
            local hwnd = get_window_handle(options.hwnd)
            check(hwnd ~= nil,
                  "找色和找图需要 --hwnd；若只测 OCR，请加 --skip-window。")
            check(dm.BindWindow(hwnd, options.display, "normal", "normal", 0) ~= 0,
                  "绑定窗口失败；请检查 HWND、采集方式和目标画面。")
            bound = true
            check(dm.SetFindBackend(options.backend) ~= 0,
                  "设置找色/找图后端失败: " .. options.backend)
            check(dm.SetPath(template_dir) ~= 0,
                  "设置模板目录失败: " .. template_dir)

            -- 与报告一致：100 轮，每轮用两个颜色表达式各查一次。
            run_timed_case(clock, dm, "找色案例 FindColorEx（每轮两次调用）", 100,
                function()
                    local result = dm.FindColorEx(
                        61, 34, 80, 53, "F5F5F5-0A0A0A", 1.0, 0)
                    release_result(dm, result)
                    result = dm.FindColorEx(
                        61, 34, 80, 53, "37843D-27242F|63A336-1A1D1A", 1.0, 0)
                    return result
                end)

            local home_template = path_join(template_dir, "主页.bmp")
            local apps_template = path_join(template_dir, "应用.bmp")
            local home_file = io.open(home_template, "rb")
            local apps_file = io.open(apps_template, "rb")
            local has_home = home_file ~= nil
            local has_apps = apps_file ~= nil
            if home_file then home_file:close() end
            if apps_file then apps_file:close() end

            if has_home and has_apps then
                -- 与报告一致：5000 轮，每轮各查找两张模板，共 10000 次找图调用。
                run_timed_case(clock, dm, "找图案例 FindPic（每轮两次调用）", 5000,
                    function()
                        local result = dm.FindPic(
                            97, 108, 130, 129, "主页.bmp", "000000", 1.0, 0)
                        release_result(dm, result)
                        result = dm.FindPic(
                            162, 162, 195, 183, "应用.bmp", "000000", 1.0, 0)
                        return result
                    end)
            else
                io.write("跳过找图：模板目录缺少 ",
                    not has_home and "主页.bmp " or "",
                    not has_apps and "应用.bmp" or "", "\n")
            end
        end

        if not options["skip-ocr"] then
            check(dm.SetOcrEngine("model") ~= 0, "选择模型 OCR 引擎失败。")
            check(dm.SetOcrBackend(options["ocr-backend"]) ~= 0,
                  "设置 OCR 后端失败: " .. options["ocr-backend"])
            check(dm.SetOcrModel(0, model_dir) ~= 0,
                  "登记 OCR 模型失败: " .. model_dir)
            check(dm.UseOcrModel(0) ~= 0, "选择 OCR 模型失败。")

            -- 报告给出的是旧 Rapid10 文件模型接口。当前 jiaofu DLL 使用 PP-OCRv5，
            -- 因此读同一张 BMP 的对应区域，并以 OcrInFile 复现每区域 100 次识别。
            run_timed_case(clock, dm, "OCR 案例区域一 OcrInFile", 100,
                function()
                    return dm.OcrInFile(
                        96, 108, 131, 129, image_path, "", 0.3)
                end)
            run_timed_case(clock, dm, "OCR 案例区域二 OcrInFile", 100,
                function()
                    return dm.OcrInFile(
                        96, 343, 198, 366, image_path, "", 0.3)
                end)
        end
    end, debug.traceback)

    if bound then
        local unbind_ok, unbind_error = pcall(function()
            check(dm.UnBindWindow() ~= 0, "解绑目标窗口失败。")
        end)
        if not unbind_ok then
            io.stderr:write(tostring(unbind_error), "\n")
            exit_code = 1
        end
    end

    if not ok then
        io.stderr:write(tostring(message), "\n")
        exit_code = 1
    end
    return exit_code
end

local exit_code = main()
os.exit(exit_code)
