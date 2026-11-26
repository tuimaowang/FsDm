#pragma once
#include <windows.h>
#include <cwchar>
// 两个模块共享进程现有控制台。单独打开 CONOUT$，避免更改宿主的标准流。
// 禁用关闭按钮，防止点击控制台关闭按钮导致整个宿主退出。
inline HANDLE FsOpenConsole(bool enabled) {
    if (!enabled) return INVALID_HANDLE_VALUE;
    auto open = [] { return CreateFileW(L"CONOUT$", GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr); };
    HANDLE output = open();
    if (output != INVALID_HANDLE_VALUE) return output;
    HANDLE saved[] = {GetStdHandle(STD_INPUT_HANDLE), GetStdHandle(STD_OUTPUT_HANDLE),
                      GetStdHandle(STD_ERROR_HANDLE)};
    if (!AllocConsole()) return open();
    const DWORD ids[] = {STD_INPUT_HANDLE, STD_OUTPUT_HANDLE, STD_ERROR_HANDLE};
    for (int i = 0; i < 3; ++i)
        if (saved[i] && saved[i] != INVALID_HANDLE_VALUE) SetStdHandle(ids[i], saved[i]);
    SetConsoleTitleW(L"FsDm 绑定与 BGRA 采集工作台");
    if (HWND window = GetConsoleWindow())
        if (HMENU menu = GetSystemMenu(window, FALSE))
            EnableMenuItem(menu, SC_CLOSE, MF_BYCOMMAND | MF_GRAYED);
    return open();
}
inline void FsConsoleLine(HANDLE output, const wchar_t* line) {
    if (!output || output == INVALID_HANDLE_VALUE) return;
    DWORD written{};
    // 全文和文件名不截断；分块写入以避开控制台单次写入的长度限制。
    size_t remaining=wcslen(line);
    while(remaining) {
        DWORD count=static_cast<DWORD>(remaining>8192?8192:remaining);
        // 不把UTF-16代理字符的两个编码单元拆开，保证中文扩展字符正常显示。
        if(count<remaining && line[count-1]>=0xd800 && line[count-1]<=0xdbff) --count;
        if(!WriteConsoleW(output,line,count,&written,nullptr) || !written) return;
        line+=written;remaining-=written;
    }
    WriteConsoleW(output, L"\r\n", 2, &written, nullptr);
}
