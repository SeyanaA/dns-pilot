// 原生界面：最近 60 秒请求量柱状图控件
#pragma once
#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

namespace uichart {

// 注册窗口类 "DNSPilotChart"；重复调用安全
void Register(HINSTANCE inst);

// 创建子窗口控件；尺寸由父窗口通过 SetWindowPos 布局
HWND Create(HWND parent, int id);

// 更新数据（最旧 -> 最新）与右上角说明文字
void SetData(HWND chart, const std::vector<uint32_t> &data, const std::wstring &rightLabel);

// 切换浅色/深色主题；调用方负责随后 InvalidateRect 重绘
void SetTheme(bool light);

}  // namespace uichart
