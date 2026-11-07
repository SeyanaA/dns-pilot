// 原生进程选择对话框（Win32，无第三方依赖）
#pragma once
#include <windows.h>

#include <string>

namespace procpick {

struct Picked {
    unsigned long pid = 0;
    std::wstring exeName;  // 进程名，例如 chrome.exe
    std::wstring exePath;  // 完整路径
    std::wstring title;    // 窗口标题，可能为空
};

// 以 owner 为父窗口打开模态选择框；用户点击“确定”时返回 true 并写入 out
bool Show(HWND owner, Picked& out);

// 切换浅色/深色主题（在 Show 之前调用；窗口自身也会记住当前主题）
void SetTheme(bool light);

}  // namespace procpick
