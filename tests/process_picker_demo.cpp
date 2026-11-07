// 进程选择对话框的演示与视觉测试程序
//
// 用法：picker_demo.exe [--open]
//   --open  启动 500ms 后自动打开选择框，便于截图/自动化测试。

#include "../src/process_picker.h"

#include <commctrl.h>
#include <dwmapi.h>

#include <algorithm>
#include <cstring>
#include <string>

namespace {

constexpr COLORREF kDemoBack = RGB(7, 11, 21);
constexpr COLORREF kDemoPanel = RGB(13, 20, 33);
constexpr COLORREF kDemoBorder = RGB(30, 41, 61);
constexpr COLORREF kDemoText = RGB(226, 232, 240);
constexpr COLORREF kDemoDim = RGB(148, 163, 184);
constexpr COLORREF kDemoMuted = RGB(100, 116, 139);
constexpr COLORREF kDemoAccent = RGB(34, 211, 238);
constexpr COLORREF kDemoAccentDark = RGB(4, 15, 24);

constexpr int kIdPick = 2001;
constexpr UINT_PTR kTimerOpen = 1;

HWND g_pick = nullptr;
HWND g_name = nullptr;
HWND g_path = nullptr;
HFONT g_fontUi = nullptr;
HFONT g_fontName = nullptr;
HFONT g_fontSmall = nullptr;
HBRUSH g_back = nullptr;
HBRUSH g_panelBrush = nullptr;
bool g_opening = false;

void OpenPicker(HWND hwnd) {
    if (g_opening) return;
    g_opening = true;
    procpick::Picked picked;
    if (procpick::Show(hwnd, picked)) {
        std::wstring name = picked.exeName.empty() ? L"(未知进程)" : picked.exeName;
        name += L"     PID " + std::to_wstring(picked.pid);
        SetWindowTextW(g_name, name.c_str());
        SetWindowTextW(g_path, picked.exePath.empty() ? L"(无法读取完整路径)" : picked.exePath.c_str());
    }
    g_opening = false;
}

void PaintDemo(HWND hwnd) {
    PAINTSTRUCT ps{};
    HDC dc = BeginPaint(hwnd, &ps);
    if (!dc) return;

    RECT client{};
    GetClientRect(hwnd, &client);
    FillRect(dc, &client, g_back);

    SetBkMode(dc, TRANSPARENT);
    if (g_fontSmall) SelectObject(dc, g_fontSmall);
    SetTextColor(dc, kDemoMuted);
    RECT top{32, 14, client.right - 32, 38};
    DrawTextW(dc, L"DNS PILOT · 进程选择器演示", -1, &top,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

    RECT panel{24, 116, client.right - 24, 286};
    HBRUSH fill = CreateSolidBrush(kDemoPanel);
    HPEN pen = CreatePen(PS_SOLID, 1, kDemoBorder);
    HGDIOBJ oldPen = SelectObject(dc, pen);
    HGDIOBJ oldBrush = SelectObject(dc, fill);
    RoundRect(dc, panel.left, panel.top, panel.right, panel.bottom, 18, 18);
    SelectObject(dc, oldPen);
    SelectObject(dc, oldBrush);
    DeleteObject(pen);
    DeleteObject(fill);

    SetTextColor(dc, kDemoMuted);
    RECT caption{48, 126, client.right - 48, 150};
    DrawTextW(dc, L"已选择进程", -1, &caption,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

    SetTextColor(dc, kDemoMuted);
    RECT footer{32, 306, client.right - 32, 330};
    DrawTextW(dc, L"对话框为模态；支持搜索、双击或回车确定，Esc 取消。", -1, &footer,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);

    EndPaint(hwnd, &ps);
}

void DrawPickButton(const DRAWITEMSTRUCT *dis) {
    HDC dc = dis->hDC;
    RECT rc = dis->rcItem;
    bool pressed = (dis->itemState & ODS_SELECTED) != 0;
    COLORREF fill = pressed ? RGB(14, 165, 233) : kDemoAccent;
    HBRUSH brush = CreateSolidBrush(fill);
    HPEN pen = CreatePen(PS_SOLID, 1, fill);
    HGDIOBJ oldBrush = SelectObject(dc, brush);
    HGDIOBJ oldPen = SelectObject(dc, pen);
    RoundRect(dc, rc.left, rc.top, rc.right, rc.bottom, 18, 18);
    SelectObject(dc, oldBrush);
    SelectObject(dc, oldPen);
    DeleteObject(pen);
    DeleteObject(brush);

    SetBkMode(dc, TRANSPARENT);
    if (g_fontUi) SelectObject(dc, g_fontUi);
    SetTextColor(dc, kDemoAccentDark);
    DrawTextW(dc, L"选择进程…", -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
}

LRESULT CALLBACK DemoWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE: {
            g_back = CreateSolidBrush(kDemoBack);
            g_panelBrush = CreateSolidBrush(kDemoPanel);

            auto makeFont = [](int px, int weight) {
                return CreateFontW(-px, 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                   OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                   DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
            };
            g_fontUi = makeFont(15, FW_NORMAL);
            g_fontName = makeFont(20, FW_SEMIBOLD);
            g_fontSmall = makeFont(13, FW_NORMAL);

            g_pick = CreateWindowExW(0, L"BUTTON", L"选择进程…",
                                     WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, 32, 38, 200,
                                     46, hwnd, (HMENU)(INT_PTR)kIdPick, nullptr, nullptr);
            if (g_pick) SendMessageW(g_pick, WM_SETFONT, (WPARAM)g_fontUi, TRUE);

            g_name = CreateWindowExW(0, L"STATIC", L"尚未选择进程",
                                     WS_CHILD | WS_VISIBLE | SS_LEFT | SS_ENDELLIPSIS, 48, 152, 640,
                                     32, hwnd, nullptr, nullptr, nullptr);
            if (g_name) SendMessageW(g_name, WM_SETFONT, (WPARAM)g_fontName, TRUE);

            g_path = CreateWindowExW(0, L"STATIC", L"选择后这里会显示完整路径",
                                     WS_CHILD | WS_VISIBLE | SS_LEFT | SS_ENDELLIPSIS, 48, 192, 640,
                                     24, hwnd, nullptr, nullptr, nullptr);
            if (g_path) SendMessageW(g_path, WM_SETFONT, (WPARAM)g_fontSmall, TRUE);
            return 0;
        }

        case WM_SIZE:
            if (g_name)
                MoveWindow(g_name, 48, 152, (std::max)(80, (int)LOWORD(lParam) - 96), 32, TRUE);
            if (g_path)
                MoveWindow(g_path, 48, 192, (std::max)(80, (int)LOWORD(lParam) - 96), 24, TRUE);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;

        case WM_ERASEBKGND:
            return 1;

        case WM_PAINT:
            PaintDemo(hwnd);
            return 0;

        case WM_CTLCOLORSTATIC: {
            HWND child = (HWND)lParam;
            HDC dc = (HDC)wParam;
            SetBkColor(dc, kDemoPanel);
            SetTextColor(dc, child == g_path ? kDemoDim : kDemoText);
            return (LRESULT)g_panelBrush;
        }

        case WM_DRAWITEM: {
            DRAWITEMSTRUCT *dis = (DRAWITEMSTRUCT *)lParam;
            if (dis && dis->CtlID == kIdPick) {
                DrawPickButton(dis);
                return TRUE;
            }
            break;
        }

        case WM_COMMAND:
            if (LOWORD(wParam) == kIdPick && HIWORD(wParam) == BN_CLICKED) {
                OpenPicker(hwnd);
                return 0;
            }
            break;

        case WM_TIMER:
            if (wParam == kTimerOpen) {
                KillTimer(hwnd, kTimerOpen);
                OpenPicker(hwnd);
                return 0;
            }
            break;

        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;

        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

}  // namespace

int main(int argc, char **argv) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    INITCOMMONCONTROLSEX icc{};
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icc);

    HINSTANCE inst = GetModuleHandleW(nullptr);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = DemoWndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, (LPCWSTR)IDC_ARROW);
    wc.hIcon = LoadIconW(nullptr, (LPCWSTR)IDI_APPLICATION);
    wc.lpszClassName = L"DnsPilot.ProcessPickerDemo";
    if (!RegisterClassExW(&wc)) return 1;

    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"进程选择器演示",
                                WS_OVERLAPPEDWINDOW & ~WS_MAXIMIZEBOX, CW_USEDEFAULT, CW_USEDEFAULT,
                                760, 400, nullptr, nullptr, inst, nullptr);
    if (!hwnd) return 1;

    // 主窗口居中，方便观察选择框相对 owner 的居中行为
    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    RECT wr{};
    GetWindowRect(hwnd, &wr);
    int winW = wr.right - wr.left;
    int winH = wr.bottom - wr.top;
    int winX = work.left + (work.right - work.left - winW) / 2;
    int winY = work.top + (work.bottom - work.top - winH) / 2;
    SetWindowPos(hwnd, nullptr, winX, winY, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);

    BOOL dark = TRUE;
    if (FAILED(DwmSetWindowAttribute(hwnd, 20, &dark, sizeof(dark)))) {
        DwmSetWindowAttribute(hwnd, 19, &dark, sizeof(dark));
    }

    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    bool autoOpen = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--open") == 0) autoOpen = true;
    }
    if (autoOpen) SetTimer(hwnd, kTimerOpen, 500, nullptr);

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (g_fontUi) DeleteObject(g_fontUi);
    if (g_fontName) DeleteObject(g_fontName);
    if (g_fontSmall) DeleteObject(g_fontSmall);
    if (g_back) DeleteObject(g_back);
    if (g_panelBrush) DeleteObject(g_panelBrush);
    return 0;
}
