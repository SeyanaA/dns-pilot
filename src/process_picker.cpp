// 原生进程选择对话框（Win32/GDI，无第三方依赖）
//
// 交互参考 OBS 的“选择进程”窗口：顶部搜索、中央进程列表、底部状态与操作按钮。
// 所有尺寸以 96 DPI 为基准，运行时按 GetDpiForWindow 缩放；窗口为模态，关闭后
// 由调用方的消息循环继续处理。源文件使用 UTF-8，界面文案为中文。

#include "process_picker.h"

#include <commctrl.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <tlhelp32.h>
#include <uxtheme.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace procpick {
namespace {

// ── 配色（默认暗色主题，SetTheme 在 Show 之前整体切换） ──────────────
COLORREF kBack = RGB(7, 11, 21);
COLORREF kPanel = RGB(14, 21, 35);
COLORREF kListBg = RGB(11, 17, 29);
COLORREF kRow = RGB(12, 19, 31);
COLORREF kRowAlt = RGB(14, 22, 36);
COLORREF kRowHover = RGB(19, 31, 48);
COLORREF kRowSelected = RGB(16, 45, 60);
COLORREF kHeader = RGB(18, 27, 44);
COLORREF kBorder = RGB(30, 41, 61);
COLORREF kRowLine = RGB(18, 26, 40);
COLORREF kText = RGB(226, 232, 240);
COLORREF kTextDim = RGB(148, 163, 184);
COLORREF kTextMuted = RGB(100, 116, 139);
COLORREF kAccent = RGB(34, 211, 238);
COLORREF kAccentHover = RGB(103, 232, 249);
COLORREF kAccentPressed = RGB(14, 165, 233);
COLORREF kAccentText = RGB(4, 15, 24);
COLORREF kDisabledFill = RGB(18, 26, 40);
COLORREF kDisabledBorder = RGB(30, 40, 58);
COLORREF kDisabledText = RGB(92, 105, 125);
// 自绘滚动条滑块（悬停/拖动时用 Active）
COLORREF kScrollThumb = RGB(55, 70, 94);
COLORREF kScrollThumbActive = RGB(82, 101, 129);
// 次级按钮（刷新/取消）底色与描边：浅色下不能沿用固定的深蓝，否则深色文字不可读
COLORREF kButtonFill = RGB(18, 27, 43);
COLORREF kButtonFillHover = RGB(24, 35, 53);
COLORREF kButtonFillPressed = RGB(14, 22, 36);
COLORREF kButtonBorderHover = RGB(49, 66, 94);
// 无图标行的占位色块
COLORREF kIconPlaceholder = RGB(22, 31, 48);

// 当前是否为浅色主题；由 SetTheme 维护，绘制时配色变量已随之更新
bool gLightTheme = false;

// 控件 ID
constexpr int kIdSearch = 1001;
constexpr int kIdRefresh = 1002;
constexpr int kIdList = 1003;
constexpr int kIdCancel = 1004;
constexpr int kIdOk = 1005;

// DWM 属性（Windows 11 为 34，旧版本返回失败时忽略）
constexpr DWORD kDwmUseImmersiveDarkMode = 20;
constexpr DWORD kDwmUseImmersiveDarkModeOld = 19;
constexpr DWORD kDwmBorderColor = 34;

// ── 基础工具 ──────────────────────────────────────────────────────
wchar_t LowerAscii(wchar_t c) {
    if (c >= L'A' && c <= L'Z') return (wchar_t)(c - L'A' + L'a');
    return c;
}

std::wstring ToLower(std::wstring s) {
    for (wchar_t &c : s) c = LowerAscii(c);
    return s;
}

std::wstring BaseName(const std::wstring &path) {
    size_t pos = path.find_last_of(L"\\/");
    return pos == std::wstring::npos ? path : path.substr(pos + 1);
}

bool IContains(const std::wstring &haystack, const std::wstring &needleLower) {
    if (needleLower.empty()) return true;
    if (haystack.size() < needleLower.size()) return false;
    for (size_t i = 0; i + needleLower.size() <= haystack.size(); ++i) {
        size_t j = 0;
        for (; j < needleLower.size(); ++j) {
            if (LowerAscii(haystack[i + j]) != needleLower[j]) break;
        }
        if (j == needleLower.size()) return true;
    }
    return false;
}

int IcaseCompare(const std::wstring &a, const std::wstring &b) {
    size_t n = (std::min)(a.size(), b.size());
    for (size_t i = 0; i < n; ++i) {
        wchar_t ca = LowerAscii(a[i]);
        wchar_t cb = LowerAscii(b[i]);
        if (ca < cb) return -1;
        if (ca > cb) return 1;
    }
    if (a.size() < b.size()) return -1;
    if (a.size() > b.size()) return 1;
    return 0;
}

// 动态取 uxtheme!SetWindowTheme，避免额外链接 uxtheme.lib（构建命令未提供该库）
using SetWindowThemeFn = decltype(&SetWindowTheme);

SetWindowThemeFn ThemeFn() {
    static SetWindowThemeFn fn = []() -> SetWindowThemeFn {
        HMODULE ux = LoadLibraryW(L"uxtheme.dll");
        if (!ux) return nullptr;
        union {
            FARPROC proc;
            SetWindowThemeFn fn;
        } u{};
        u.proc = GetProcAddress(ux, "SetWindowTheme");
        return u.fn;
    }();
    return fn;
}

void ThemeSet(HWND hwnd, LPCWSTR subApp, LPCWSTR subId) {
    if (SetWindowThemeFn fn = ThemeFn()) fn(hwnd, subApp, subId);
}

// ListView 没有独立的 LVM_GETITEMDATA 消息，用 LVIF_PARAM 查询行数据
LPARAM ListItemData(HWND list, int index) {
    LVITEMW item{};
    item.mask = LVIF_PARAM;
    item.iItem = index;
    if (!SendMessageW(list, LVM_GETITEMW, 0, (LPARAM)&item)) return -1;
    return item.lParam;
}

void FillRoundRect(HDC dc, const RECT &rc, int radius, HBRUSH fill, COLORREF border, int penWidth) {
    HBRUSH oldBrush = (HBRUSH)SelectObject(dc, fill);
    HPEN pen = CreatePen(PS_SOLID, penWidth, border);
    HPEN oldPen = (HPEN)SelectObject(dc, pen);
    RoundRect(dc, rc.left, rc.top, rc.right, rc.bottom, radius * 2, radius * 2);
    SelectObject(dc, oldPen);
    SelectObject(dc, oldBrush);
    DeleteObject(pen);
}

// ── 数据结构 ──────────────────────────────────────────────────────
struct Row {
    DWORD pid = 0;
    std::wstring exeName;
    std::wstring exePath;
    std::wstring title;
};

struct Picker;

// 子类化回调（定义见文件后半部分）
LRESULT CALLBACK ListSubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, UINT_PTR id,
                                  DWORD_PTR ref);
LRESULT CALLBACK EditSubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, UINT_PTR id,
                                  DWORD_PTR ref);
LRESULT CALLBACK ButtonSubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, UINT_PTR id,
                                    DWORD_PTR ref);
LRESULT CALLBACK ScrollbarWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

struct ButtonView {
    HWND hwnd = nullptr;
    bool hot = false;
};

struct Picker {
    HWND hwnd = nullptr;
    HWND owner = nullptr;
    HWND search = nullptr;
    HWND list = nullptr;
    HWND refresh = nullptr;
    HWND cancel = nullptr;
    HWND ok = nullptr;
    HWND scrollBar = nullptr;

    UINT dpi = 96;
    int rowHeight = 26;

    HFONT fontUi = nullptr;
    HFONT fontSmall = nullptr;
    HFONT fontHeader = nullptr;

    HBRUSH brBack = nullptr;
    HBRUSH brPanel = nullptr;
    HBRUSH brList = nullptr;
    HBRUSH brRow = nullptr;
    HBRUSH brRowAlt = nullptr;
    HBRUSH brRowHover = nullptr;
    HBRUSH brSelected = nullptr;
    HBRUSH brHeader = nullptr;
    HBRUSH brBorder = nullptr;
    HBRUSH brRowLine = nullptr;
    HBRUSH brAccent = nullptr;

    RECT rcSearch{};
    RECT rcRefresh{};
    RECT rcHint{};
    RECT rcList{};
    RECT rcScroll{};
    RECT rcCancel{};
    RECT rcOk{};
    RECT rcStatus{};
    int footerLineY = 0;

    bool searchFocused = false;
    std::wstring query;
    std::wstring queryLower;
    std::wstring statusText;

    std::vector<Row> all;
    std::vector<size_t> visible;
    std::unordered_map<std::wstring, HICON> icons;

    ButtonView btnRefresh;
    ButtonView btnCancel;
    ButtonView btnOk;

    bool listTracking = false;
    int hotRow = -1;
    bool scrollTracking = false;
    bool scrollHot = false;
    bool scrollDrag = false;
    int scrollDragOffset = 0;

    bool confirmed = false;
    Picked result;

    int S(int value) const { return MulDiv(value, (int)dpi, 96); }
    int RowHeight() const { return rowHeight; }

    ButtonView *ForButton(HWND h);
    DWORD SelectedPid() const;

    void CreateFonts();
    void DeleteFonts();
    void CreateBrushes();
    void DeleteBrushes();

    void ComputeRects();
    void CreateControls();
    void CreateList();
    void Layout();
    void LayoutColumns();

    void Reload();
    void ApplyFilter();
    void RebuildList(DWORD keepPid);
    void OnSearchChanged();
    void UpdateStatus();
    void UpdateOk();
    void FocusFirstItem();

    void InvalidateRow(int index);
    void Confirm();
    void Cancel();
    void OnDpiChanged();
    void Dispose();

    bool QueryScroll(SCROLLINFO &info) const;
    RECT ScrollThumbRect() const;
    void UpdateScrollbar();
    void PaintScrollbar();
    void ScrollbarDragTo(int y);
    void ScrollbarTrackClick(int y);

    HICON IconFor(const Row &row);
    void Paint();
    void DrawRow(const DRAWITEMSTRUCT *dis);
    void DrawButton(const DRAWITEMSTRUCT *dis);
};

ButtonView *Picker::ForButton(HWND h) {
    if (h == refresh) return &btnRefresh;
    if (h == cancel) return &btnCancel;
    if (h == ok) return &btnOk;
    return nullptr;
}

DWORD Picker::SelectedPid() const {
    if (!list) return 0;
    int index = (int)SendMessageW(list, LVM_GETNEXTITEM, (WPARAM)-1, LVNI_SELECTED);
    if (index < 0) return 0;
    LPARAM lp = ListItemData(list, index);
    if (lp < 0 || (size_t)lp >= all.size()) return 0;
    return all[(size_t)lp].pid;
}

// ── 字体与画笔 ────────────────────────────────────────────────────
void Picker::CreateFonts() {
    auto make = [this](int px, int weight) -> HFONT {
        return CreateFontW(-S(px), 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                           OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                           DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
    };
    fontUi = make(14, FW_NORMAL);
    fontSmall = make(12, FW_NORMAL);
    fontHeader = make(13, FW_SEMIBOLD);
}

void Picker::DeleteFonts() {
    if (fontUi) DeleteObject(fontUi);
    if (fontSmall) DeleteObject(fontSmall);
    if (fontHeader) DeleteObject(fontHeader);
    fontUi = fontSmall = fontHeader = nullptr;
}

void Picker::CreateBrushes() {
    brBack = CreateSolidBrush(kBack);
    brPanel = CreateSolidBrush(kPanel);
    brList = CreateSolidBrush(kListBg);
    brRow = CreateSolidBrush(kRow);
    brRowAlt = CreateSolidBrush(kRowAlt);
    brRowHover = CreateSolidBrush(kRowHover);
    brSelected = CreateSolidBrush(kRowSelected);
    brHeader = CreateSolidBrush(kHeader);
    brBorder = CreateSolidBrush(kBorder);
    brRowLine = CreateSolidBrush(kRowLine);
    brAccent = CreateSolidBrush(kAccent);
}

void Picker::DeleteBrushes() {
    HBRUSH *brushes[] = {&brBack, &brPanel, &brList, &brRow, &brRowAlt, &brRowHover,
                         &brSelected, &brHeader, &brBorder, &brRowLine, &brAccent};
    for (HBRUSH *b : brushes) {
        if (*b) DeleteObject(*b);
        *b = nullptr;
    }
}

// ── 布局 ──────────────────────────────────────────────────────────
void Picker::ComputeRects() {
    RECT client{};
    GetClientRect(hwnd, &client);
    const int width = client.right - client.left;
    const int height = client.bottom - client.top;

    const int margin = S(20);
    const int top = S(20);
    const int searchH = S(36);
    const int refreshW = S(88);
    const int gap = S(12);

    rcSearch = {margin, top, width - margin - refreshW - gap, top + searchH};
    rcRefresh = {width - margin - refreshW, top, width - margin, top + searchH};

    const int hintTop = top + searchH + S(10);
    rcHint = {margin, hintTop, width - margin, hintTop + S(18)};

    const int listTop = hintTop + S(18) + S(12);
    const int btnH = S(36);
    const int btnW = S(92);
    const int btnGap = S(10);
    const int btnTop = height - S(20) - btnH;
    footerLineY = btnTop - S(9);
    int listBottom = footerLineY - S(7);
    if (listBottom < listTop + S(80)) listBottom = listTop + S(80);

    rcList = {margin, listTop, width - margin, listBottom};
    int sbW = GetSystemMetricsForDpi(SM_CXVSCROLL, dpi);
    if (sbW < S(10)) sbW = S(10);
    // 覆盖系统滚动条时向右多盖 1px，遮住列表右侧的高亮边线
    rcScroll = {rcList.right - sbW, rcList.top, rcList.right + (std::max)(1, S(1)), rcList.bottom};
    rcOk = {width - margin - btnW, btnTop, width - margin, btnTop + btnH};
    rcCancel = {rcOk.left - btnGap - btnW, btnTop, rcOk.left - btnGap, btnTop + btnH};
    rcStatus = {margin, btnTop, rcCancel.left - S(12), btnTop + btnH};
}

void Picker::LayoutColumns() {
    if (!list) return;
    RECT client{};
    GetClientRect(list, &client);
    int width = client.right - client.left;
    int reserved = GetSystemMetricsForDpi(SM_CXVSCROLL, dpi);
    int available = (std::max)(S(200), width - reserved);

    int pidW = S(72);
    int nameW = (std::max)(S(200), available * 30 / 100);
    int pathW = (std::max)(S(170), available * 26 / 100);
    int titleW = available - nameW - pathW - pidW;

    // 窄窗口下优先保证窗口标题列可读，再压缩进程名与路径
    if (titleW < S(140)) {
        int deficit = S(140) - titleW;
        int take = (std::min)(deficit, (std::max)(0, nameW - S(130)));
        nameW -= take;
        deficit -= take;
        take = (std::min)(deficit, (std::max)(0, pathW - S(130)));
        pathW -= take;
        deficit -= take;
        titleW = available - nameW - pathW - pidW;
    }
    if (titleW < S(80)) titleW = S(80);
    if (pathW < S(80)) pathW = S(80);

    SendMessageW(list, LVM_SETCOLUMNWIDTH, 0, MAKELPARAM(nameW, 0));
    SendMessageW(list, LVM_SETCOLUMNWIDTH, 1, MAKELPARAM(titleW, 0));
    SendMessageW(list, LVM_SETCOLUMNWIDTH, 2, MAKELPARAM(pidW, 0));
    SendMessageW(list, LVM_SETCOLUMNWIDTH, 3, MAKELPARAM(pathW, 0));
}

void Picker::Layout() {
    if (search) {
        int w = (std::max)(S(10), (int)(rcSearch.right - rcSearch.left) - S(26));
        int h = S(26);
        int y = rcSearch.top + (S(36) - h) / 2;
        MoveWindow(search, rcSearch.left + S(14), y, w, h, FALSE);
    }
    if (refresh) {
        MoveWindow(refresh, rcRefresh.left, rcRefresh.top, rcRefresh.right - rcRefresh.left,
                   rcRefresh.bottom - rcRefresh.top, FALSE);
    }
    if (list) {
        MoveWindow(list, rcList.left, rcList.top, (std::max)(S(20), (int)(rcList.right - rcList.left)),
                   (std::max)(S(20), (int)(rcList.bottom - rcList.top)), FALSE);
        LayoutColumns();
    }
    if (scrollBar) {
        // 原生列表的纵向滚动条从列表顶部贯穿到底部（表头右侧也有一段）；覆盖层使用完整高度遮住它
        MoveWindow(scrollBar, rcScroll.left, rcScroll.top, rcScroll.right - rcScroll.left,
                   rcScroll.bottom - rcScroll.top, FALSE);
        SetWindowPos(scrollBar, HWND_TOP, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
    if (cancel) {
        MoveWindow(cancel, rcCancel.left, rcCancel.top, rcCancel.right - rcCancel.left,
                   rcCancel.bottom - rcCancel.top, FALSE);
    }
    if (ok) {
        MoveWindow(ok, rcOk.left, rcOk.top, rcOk.right - rcOk.left, rcOk.bottom - rcOk.top, FALSE);
    }
    UpdateScrollbar();
    InvalidateRect(hwnd, nullptr, FALSE);
}

// ── 控件创建 ──────────────────────────────────────────────────────
void Picker::CreateList() {
    HINSTANCE inst = (HINSTANCE)GetWindowLongPtrW(hwnd, GWLP_HINSTANCE);
    DWORD style = WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_CLIPSIBLINGS | LVS_REPORT |
                  LVS_OWNERDRAWFIXED | LVS_SINGLESEL | LVS_SHOWSELALWAYS | LVS_NOSORTHEADER;
    list = CreateWindowExW(0, WC_LISTVIEWW, L"", style, rcList.left, rcList.top,
                           (std::max)(S(20), (int)(rcList.right - rcList.left)),
                           (std::max)(S(20), (int)(rcList.bottom - rcList.top)), hwnd,
                           (HMENU)(INT_PTR)kIdList, inst, nullptr);
    if (!list) return;

    SendMessageW(list, WM_SETFONT, (WPARAM)fontUi, TRUE);
    SendMessageW(list, LVM_SETEXTENDEDLISTVIEWSTYLE, 0, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    SendMessageW(list, LVM_SETBKCOLOR, 0, (LPARAM)kListBg);
    SendMessageW(list, LVM_SETTEXTBKCOLOR, 0, (LPARAM)kListBg);
    SendMessageW(list, LVM_SETTEXTCOLOR, 0, (LPARAM)kText);
    // 原生控件的主题名随当前配色切换；这些调用发生在 SetTheme 之后（Show 内创建控件）
    ThemeSet(list, gLightTheme ? L"Explorer" : L"DarkMode_Explorer", nullptr);
    SetWindowSubclass(list, ListSubclassProc, 1, (DWORD_PTR)this);

    static const wchar_t *kHeaders[4] = {L"进程", L"窗口标题", L"PID", L"路径"};
    static const int kFormats[4] = {LVCFMT_LEFT, LVCFMT_LEFT, LVCFMT_RIGHT, LVCFMT_LEFT};
    for (int i = 0; i < 4; ++i) {
        LVCOLUMNW col{};
        col.mask = LVCF_TEXT | LVCF_FMT | LVCF_WIDTH;
        col.fmt = kFormats[i];
        col.cx = 100;
        col.pszText = const_cast<wchar_t *>(kHeaders[i]);
        SendMessageW(list, LVM_INSERTCOLUMNW, (WPARAM)i, (LPARAM)&col);
    }

    HWND header = (HWND)SendMessageW(list, LVM_GETHEADER, 0, 0);
    if (header) {
        SendMessageW(header, WM_SETFONT, (WPARAM)fontHeader, TRUE);
        ThemeSet(header, gLightTheme ? L"ItemsView" : L"DarkMode_ItemsView", nullptr);
    }
    LayoutColumns();
}

void Picker::CreateControls() {
    HINSTANCE inst = (HINSTANCE)GetWindowLongPtrW(hwnd, GWLP_HINSTANCE);

    search = CreateWindowExW(
        0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL | ES_LEFT,
        rcSearch.left + S(14), rcSearch.top + S(5),
        (std::max)(S(10), (int)(rcSearch.right - rcSearch.left) - S(26)), S(26), hwnd,
        (HMENU)(INT_PTR)kIdSearch, inst, nullptr);
    if (search) {
        SendMessageW(search, WM_SETFONT, (WPARAM)fontUi, TRUE);
        SetWindowSubclass(search, EditSubclassProc, 1, (DWORD_PTR)this);
        ThemeSet(search, gLightTheme ? L"Explorer" : L"DarkMode_Explorer", nullptr);
    }

    refresh = CreateWindowExW(0, L"BUTTON", L"刷新",
                              WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, rcRefresh.left,
                              rcRefresh.top, rcRefresh.right - rcRefresh.left,
                              rcRefresh.bottom - rcRefresh.top, hwnd, (HMENU)(INT_PTR)kIdRefresh,
                              inst, nullptr);
    if (refresh) {
        SendMessageW(refresh, WM_SETFONT, (WPARAM)fontUi, TRUE);
        SetWindowSubclass(refresh, ButtonSubclassProc, 1, (DWORD_PTR)this);
        btnRefresh.hwnd = refresh;
    }

    CreateList();

    scrollBar = CreateWindowExW(0, L"DnsPilot.PickerScroll", L"",
                                WS_CHILD | WS_CLIPSIBLINGS, rcScroll.left, rcScroll.top,
                                rcScroll.right - rcScroll.left, rcScroll.bottom - rcScroll.top, hwnd,
                                nullptr, inst, nullptr);
    if (scrollBar) SetWindowLongPtrW(scrollBar, GWLP_USERDATA, (LONG_PTR)this);

    cancel = CreateWindowExW(0, L"BUTTON", L"取消",
                             WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, rcCancel.left,
                             rcCancel.top, rcCancel.right - rcCancel.left,
                             rcCancel.bottom - rcCancel.top, hwnd, (HMENU)(INT_PTR)kIdCancel, inst,
                             nullptr);
    if (cancel) {
        SendMessageW(cancel, WM_SETFONT, (WPARAM)fontUi, TRUE);
        SetWindowSubclass(cancel, ButtonSubclassProc, 1, (DWORD_PTR)this);
        btnCancel.hwnd = cancel;
    }

    ok = CreateWindowExW(0, L"BUTTON", L"确定", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                         rcOk.left, rcOk.top, rcOk.right - rcOk.left, rcOk.bottom - rcOk.top, hwnd,
                         (HMENU)(INT_PTR)kIdOk, inst, nullptr);
    if (ok) {
        SendMessageW(ok, WM_SETFONT, (WPARAM)fontUi, TRUE);
        SetWindowSubclass(ok, ButtonSubclassProc, 1, (DWORD_PTR)this);
        btnOk.hwnd = ok;
        EnableWindow(ok, FALSE);
    }
}

// ── 数据 ──────────────────────────────────────────────────────────
struct WindowTitleMap {
    std::unordered_map<DWORD, std::wstring> byPid;
    HWND skip = nullptr;
};

BOOL CALLBACK EnumWindowProc(HWND hwnd, LPARAM lp) {
    WindowTitleMap *map = reinterpret_cast<WindowTitleMap *>(lp);
    if (hwnd == map->skip || !IsWindowVisible(hwnd)) return TRUE;
    int len = GetWindowTextLengthW(hwnd);
    if (len <= 0) return TRUE;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == 0) return TRUE;
    std::wstring title((size_t)len + 1, L'\0');
    int n = GetWindowTextW(hwnd, title.data(), (int)title.size());
    if (n <= 0) return TRUE;
    title.resize((size_t)n);
    std::wstring &cur = map->byPid[pid];
    if (title.size() > cur.size()) cur = std::move(title);
    return TRUE;
}

void Picker::ApplyFilter() {
    visible.clear();
    visible.reserve(all.size());
    for (size_t i = 0; i < all.size(); ++i) {
        const Row &row = all[i];
        if (queryLower.empty() || IContains(row.exeName, queryLower) ||
            IContains(row.title, queryLower) || IContains(row.exePath, queryLower) ||
            IContains(std::to_wstring((unsigned long)row.pid), queryLower)) {
            visible.push_back(i);
        }
    }
}

void Picker::RebuildList(DWORD keepPid) {
    if (!list) return;
    SendMessageW(list, WM_SETREDRAW, FALSE, 0);
    SendMessageW(list, LVM_DELETEALLITEMS, 0, 0);
    hotRow = -1;

    for (size_t i = 0; i < visible.size(); ++i) {
        const Row &row = all[visible[i]];
        LVITEMW item{};
        item.mask = LVIF_TEXT | LVIF_PARAM;
        item.iItem = (int)i;
        item.iSubItem = 0;
        item.pszText = const_cast<wchar_t *>(row.exeName.c_str());
        item.lParam = (LPARAM)visible[i];
        int index = (int)SendMessageW(list, LVM_INSERTITEMW, 0, (LPARAM)&item);
        if (index < 0) continue;

        LVITEMW sub{};
        sub.iSubItem = 1;
        sub.pszText = const_cast<wchar_t *>(row.title.c_str());
        SendMessageW(list, LVM_SETITEMTEXTW, (WPARAM)index, (LPARAM)&sub);

        std::wstring pid = std::to_wstring((unsigned long)row.pid);
        sub.iSubItem = 2;
        sub.pszText = pid.data();
        SendMessageW(list, LVM_SETITEMTEXTW, (WPARAM)index, (LPARAM)&sub);

        sub.iSubItem = 3;
        sub.pszText = const_cast<wchar_t *>(row.exePath.c_str());
        SendMessageW(list, LVM_SETITEMTEXTW, (WPARAM)index, (LPARAM)&sub);
    }

    if (keepPid != 0) {
        int count = (int)SendMessageW(list, LVM_GETITEMCOUNT, 0, 0);
        for (int i = 0; i < count; ++i) {
            LPARAM lp = ListItemData(list, i);
            if (lp < 0 || (size_t)lp >= all.size()) continue;
            if (all[(size_t)lp].pid != keepPid) continue;
            LVITEMW state{};
            state.state = LVIS_SELECTED | LVIS_FOCUSED;
            state.stateMask = LVIS_SELECTED | LVIS_FOCUSED;
            SendMessageW(list, LVM_SETITEMSTATE, (WPARAM)i, (LPARAM)&state);
            SendMessageW(list, LVM_ENSUREVISIBLE, (WPARAM)i, FALSE);
            break;
        }
    }

    SendMessageW(list, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(list, nullptr, TRUE);
    UpdateOk();
    UpdateStatus();
    UpdateScrollbar();
}

void Picker::Reload() {
    DWORD keepPid = SelectedPid();
    all.clear();

    WindowTitleMap titles;
    titles.skip = hwnd;
    EnumWindows(EnumWindowProc, (LPARAM)&titles);

    std::unordered_set<DWORD> seen;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W entry{};
        entry.dwSize = sizeof(entry);
        if (Process32FirstW(snap, &entry)) {
            do {
                DWORD pid = entry.th32ProcessID;
                if (!seen.insert(pid).second) continue;

                Row row;
                row.pid = pid;

                wchar_t path[32768];
                DWORD pathLen = (DWORD)(sizeof(path) / sizeof(path[0]));
                HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
                if (process) {
                    if (QueryFullProcessImageNameW(process, 0, path, &pathLen)) {
                        row.exePath.assign(path, pathLen);
                    }
                    CloseHandle(process);
                }

                if (!row.exePath.empty()) row.exeName = BaseName(row.exePath);
                else row.exeName = entry.szExeFile;

                auto it = titles.byPid.find(pid);
                if (it != titles.byPid.end()) row.title = it->second;

                // 0/4 是 System Idle Process / System；拿不到路径时没有可选项
                if ((pid == 0 || pid == 4) && row.exePath.empty()) continue;

                all.push_back(std::move(row));
            } while (Process32NextW(snap, &entry));
        }
        CloseHandle(snap);
    }

    // 有窗口的进程优先；随后按进程名不区分大小写；最后按 PID
    std::sort(all.begin(), all.end(), [](const Row &a, const Row &b) {
        bool aw = !a.title.empty();
        bool bw = !b.title.empty();
        if (aw != bw) return aw;
        int cmp = IcaseCompare(a.exeName, b.exeName);
        if (cmp != 0) return cmp < 0;
        return a.pid < b.pid;
    });

    ApplyFilter();
    RebuildList(keepPid);
}

void Picker::OnSearchChanged() {
    if (!search) return;
    int len = GetWindowTextLengthW(search);
    std::wstring text((size_t)len + 1, L'\0');
    int n = GetWindowTextW(search, text.data(), (int)text.size());
    text.resize(n > 0 ? (size_t)n : 0);

    DWORD keepPid = SelectedPid();
    query = text;
    queryLower = ToLower(text);
    ApplyFilter();
    RebuildList(keepPid);
}

void Picker::UpdateStatus() {
    if (!hwnd) return;
    size_t withWindow = 0;
    for (const Row &row : all) {
        if (!row.title.empty()) ++withWindow;
    }
    statusText = L"共 " + std::to_wstring(all.size()) + L" 个进程 · " +
                 std::to_wstring(withWindow) + L" 个有窗口";
    if (!query.empty()) {
        statusText += L" · 匹配 " + std::to_wstring(visible.size()) + L" 项";
    }
    InvalidateRect(hwnd, &rcStatus, FALSE);
}

void Picker::UpdateOk() {
    if (!ok) return;
    bool has = false;
    if (list) has = SendMessageW(list, LVM_GETNEXTITEM, (WPARAM)-1, LVNI_SELECTED) >= 0;
    BOOL enabled = IsWindowEnabled(ok);
    BOOL want = has ? TRUE : FALSE;
    if (enabled != want) {
        EnableWindow(ok, want);
        InvalidateRect(ok, nullptr, FALSE);
    }
}

void Picker::FocusFirstItem() {
    if (!list) return;
    SetFocus(list);
    if (SendMessageW(list, LVM_GETNEXTITEM, (WPARAM)-1, LVNI_SELECTED) < 0 &&
        SendMessageW(list, LVM_GETITEMCOUNT, 0, 0) > 0) {
        LVITEMW state{};
        state.state = LVIS_SELECTED | LVIS_FOCUSED;
        state.stateMask = LVIS_SELECTED | LVIS_FOCUSED;
        SendMessageW(list, LVM_SETITEMSTATE, 0, (LPARAM)&state);
        SendMessageW(list, LVM_ENSUREVISIBLE, 0, FALSE);
    }
}

void Picker::InvalidateRow(int index) {
    if (!list || index < 0) return;
    RECT rc{};
    if (SendMessageW(list, LVM_GETITEMRECT, (WPARAM)index, (LPARAM)&rc)) {
        InvalidateRect(list, &rc, FALSE);
    }
}

void Picker::Confirm() {
    if (!list) return;
    int index = (int)SendMessageW(list, LVM_GETNEXTITEM, (WPARAM)-1, LVNI_SELECTED);
    if (index < 0) return;
    LPARAM lp = ListItemData(list, index);
    if (lp < 0 || (size_t)lp >= all.size()) return;
    const Row &row = all[(size_t)lp];
    result.pid = row.pid;
    result.exeName = row.exeName;
    result.exePath = row.exePath;
    result.title = row.title;
    confirmed = true;
    DestroyWindow(hwnd);
}

void Picker::Cancel() {
    confirmed = false;
    DestroyWindow(hwnd);
}

void Picker::OnDpiChanged() {
    DWORD keepPid = SelectedPid();
    hotRow = -1;

    DeleteFonts();
    CreateFonts();
    if (search) SendMessageW(search, WM_SETFONT, (WPARAM)fontUi, TRUE);
    if (refresh) SendMessageW(refresh, WM_SETFONT, (WPARAM)fontUi, TRUE);
    if (cancel) SendMessageW(cancel, WM_SETFONT, (WPARAM)fontUi, TRUE);
    if (ok) SendMessageW(ok, WM_SETFONT, (WPARAM)fontUi, TRUE);

    // LVS_OWNERDRAWFIXED 的行高来自 WM_MEASUREITEM；重建列表以应用新 DPI 的行高
    if (list) {
        RemoveWindowSubclass(list, ListSubclassProc, 1);
        DestroyWindow(list);
        list = nullptr;
    }
    rowHeight = S(26);
    ComputeRects();
    CreateList();
    Layout();
    RebuildList(keepPid);
    InvalidateRect(hwnd, nullptr, FALSE);
}

void Picker::Dispose() {
    DeleteFonts();
    DeleteBrushes();
    for (auto &kv : icons) {
        if (kv.second) DestroyIcon(kv.second);
    }
    icons.clear();
}

// ── 自绘滚动条（覆盖列表原生浅色滚动条） ──────────────────────────
bool Picker::QueryScroll(SCROLLINFO &info) const {
    if (!list) return false;
    info = {};
    info.cbSize = sizeof(info);
    info.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    if (!::GetScrollInfo(list, SB_VERT, &info)) return false;
    if (info.nMax <= info.nMin || info.nPage == 0) return false;
    return (UINT)(info.nMax - info.nMin + 1) > info.nPage;
}

RECT Picker::ScrollThumbRect() const {
    RECT rc{};
    if (!scrollBar) return rc;
    GetClientRect(scrollBar, &rc);
    SCROLLINFO info{};
    if (!QueryScroll(info)) return rc;

    const int pad = S(3);
    int trackH = (rc.bottom - rc.top) - pad * 2;
    int range = info.nMax - info.nMin + 1;
    int thumbH = (int)((int64_t)trackH * info.nPage / range);
    int minH = S(26);
    if (thumbH < minH) thumbH = minH;
    if (thumbH > trackH) thumbH = trackH;
    if (thumbH < 1) thumbH = 1;

    int scrollable = range - (int)info.nPage;
    int travel = trackH - thumbH;
    int pos = info.nPos - info.nMin;
    int top = rc.top + pad + (scrollable > 0 ? (int)((int64_t)travel * pos / scrollable) : 0);

    int width = S(8);
    if (width < 4) width = 4;
    int maxW = (rc.right - rc.left) - S(2);
    if (width > maxW) width = maxW;
    int x = rc.left + ((rc.right - rc.left) - width) / 2;
    RECT thumb{x, top, x + width, top + thumbH};
    return thumb;
}

void Picker::UpdateScrollbar() {
    if (!scrollBar || !list) return;
    SCROLLINFO info{};
    bool want = QueryScroll(info);
    BOOL visible = IsWindowVisible(scrollBar);
    if (want && !visible) {
        ShowWindow(scrollBar, SW_SHOWNOACTIVATE);
        InvalidateRect(scrollBar, nullptr, FALSE);
    } else if (!want && visible) {
        ShowWindow(scrollBar, SW_HIDE);
    } else if (want) {
        InvalidateRect(scrollBar, nullptr, FALSE);
    }
}

void Picker::PaintScrollbar() {
    if (!scrollBar) return;
    PAINTSTRUCT ps{};
    HDC dc = BeginPaint(scrollBar, &ps);
    if (!dc) return;

    RECT rc{};
    GetClientRect(scrollBar, &rc);
    FillRect(dc, &rc, brBack);

    RECT edge{rc.left, rc.top, rc.left + (std::max)(1, S(1)), rc.bottom};
    FillRect(dc, &edge, brBorder);

    RECT thumb = ScrollThumbRect();
    if (thumb.right > thumb.left && thumb.bottom > thumb.top) {
        bool active = scrollHot || scrollDrag;
        COLORREF thumbColor = active ? kScrollThumbActive : kScrollThumb;
        HBRUSH brush = CreateSolidBrush(thumbColor);
        FillRoundRect(dc, thumb, S(4), brush, thumbColor, 1);
        DeleteObject(brush);
    }

    EndPaint(scrollBar, &ps);
}

void Picker::ScrollbarDragTo(int y) {
    if (!list || !scrollBar) return;
    SCROLLINFO info{};
    if (!QueryScroll(info)) return;

    RECT rc{};
    GetClientRect(scrollBar, &rc);
    const int pad = S(3);
    RECT thumb = ScrollThumbRect();
    int thumbH = thumb.bottom - thumb.top;
    int trackH = (rc.bottom - rc.top) - pad * 2;
    int travel = trackH - thumbH;
    if (travel <= 0) return;

    int top = y - scrollDragOffset;
    int minTop = rc.top + pad;
    int maxTop = rc.bottom - pad - thumbH;
    if (top < minTop) top = minTop;
    if (top > maxTop) top = maxTop;

    int range = info.nMax - info.nMin + 1;
    int scrollable = range - (int)info.nPage;
    if (scrollable <= 0) return;

    int target = info.nMin + MulDiv(top - minTop, scrollable, travel);
    int maxIndex = info.nMax - (int)info.nPage + 1;
    if (maxIndex < info.nMin) maxIndex = info.nMin;
    if (target < info.nMin) target = info.nMin;
    if (target > maxIndex) target = maxIndex;

    int current = (int)SendMessageW(list, LVM_GETTOPINDEX, 0, 0);
    if (target != current) {
        SendMessageW(list, LVM_SCROLL, 0, (target - current) * rowHeight);
    }
    UpdateScrollbar();
}

void Picker::ScrollbarTrackClick(int y) {
    if (!list || !scrollBar) return;
    RECT thumb = ScrollThumbRect();
    SendMessageW(list, WM_VSCROLL, y < thumb.top ? SB_PAGEUP : SB_PAGEDOWN, 0);
    UpdateScrollbar();
}

// ── 绘制 ──────────────────────────────────────────────────────────
HICON Picker::IconFor(const Row &row) {
    if (row.exePath.empty()) return nullptr;
    std::wstring key = ToLower(row.exePath);
    auto it = icons.find(key);
    if (it != icons.end()) return it->second;

    SHFILEINFOW info{};
    HICON icon = nullptr;
    if (SHGetFileInfoW(row.exePath.c_str(), 0, &info, sizeof(info), SHGFI_ICON | SHGFI_SMALLICON)) {
        icon = info.hIcon;
    }
    icons.emplace(std::move(key), icon);
    return icon;
}

void Picker::Paint() {
    PAINTSTRUCT ps{};
    HDC dc = BeginPaint(hwnd, &ps);
    if (!dc) return;

    RECT client{};
    GetClientRect(hwnd, &client);
    FillRect(dc, &client, brBack);

    // 搜索框底板（EDIT 子控件覆盖内部，圆角与聚焦描边由父窗口绘制）
    FillRoundRect(dc, rcSearch, S(10), brPanel, searchFocused ? kAccent : kBorder, S(1));

    SetBkMode(dc, TRANSPARENT);
    if (fontSmall) SelectObject(dc, fontSmall);
    SetTextColor(dc, kTextMuted);
    RECT hint = rcHint;
    DrawTextW(dc, L"按进程名、窗口标题或路径筛选 · 双击或回车确定", -1, &hint,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);

    RECT line{S(20), footerLineY, client.right - S(20), footerLineY + (std::max)(1, S(1))};
    FillRect(dc, &line, brBorder);

    SetTextColor(dc, kTextDim);
    RECT status = rcStatus;
    DrawTextW(dc, statusText.c_str(), (int)statusText.size(), &status,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);

    EndPaint(hwnd, &ps);
}

void Picker::DrawRow(const DRAWITEMSTRUCT *dis) {
    if (!dis || !list) return;
    int index = (int)dis->itemID;
    LPARAM lp = ListItemData(list, index);
    if (lp < 0 || (size_t)lp >= all.size()) return;
    const Row &row = all[(size_t)lp];

    HDC dc = dis->hDC;
    RECT rc = dis->rcItem;
    const bool selected = (dis->itemState & ODS_SELECTED) != 0;
    const bool hot = (index == hotRow) && !selected;

    HBRUSH fill = selected ? brSelected : (hot ? brRowHover : ((index & 1) ? brRowAlt : brRow));
    FillRect(dc, &rc, fill);

    if (selected) {
        RECT bar = rc;
        bar.right = bar.left + (std::max)(2, S(3));
        FillRect(dc, &bar, brAccent);
    }

    RECT rowLine{rc.left + S(10), rc.bottom - (std::max)(1, S(1)), rc.right, rc.bottom};
    FillRect(dc, &rowLine, brRowLine);

    HICON icon = IconFor(row);
    int iconSize = S(16);
    int iconX = rc.left + S(10);
    int iconY = rc.top + ((rc.bottom - rc.top) - iconSize) / 2;

    int saved = SaveDC(dc);
    SetBkMode(dc, TRANSPARENT);
    if (fontUi) SelectObject(dc, fontUi);

    int x = rc.left;
    for (int col = 0; col < 4; ++col) {
        int colW = (int)SendMessageW(list, LVM_GETCOLUMNWIDTH, (WPARAM)col, 0);
        RECT cell{x, rc.top, x + colW, rc.bottom - (std::max)(1, S(1))};
        x += colW;

        int cellSaved = SaveDC(dc);
        IntersectClipRect(dc, cell.left, cell.top, cell.right, cell.bottom);

        std::wstring text;
        COLORREF color = kTextDim;
        UINT fmt = DT_LEFT;
        int textLeft = cell.left + S(12);

        switch (col) {
            case 0: {
                if (icon) {
                    DrawIconEx(dc, iconX, iconY, icon, iconSize, iconSize, 0, nullptr, DI_NORMAL);
                } else {
                    // 无路径/无图标时保留统一的图标缩进
                    RECT ph{iconX, iconY, iconX + iconSize, iconY + iconSize};
                    HBRUSH placeholder = CreateSolidBrush(kIconPlaceholder);
                    FillRect(dc, &ph, placeholder);
                    DeleteObject(placeholder);
                }
                textLeft = iconX + iconSize + S(8);
                text = row.exeName.empty() ? L"(未知)" : row.exeName;
                color = row.exeName.empty() ? kTextMuted : kText;
                break;
            }
            case 1:
                if (row.title.empty()) {
                    text = L"—";
                    color = kTextMuted;
                } else {
                    text = row.title;
                    color = selected ? kText : kTextDim;
                }
                break;
            case 2:
                text = std::to_wstring((unsigned long)row.pid);
                color = selected ? kTextDim : kTextMuted;
                fmt = DT_RIGHT;
                break;
            case 3:
                if (row.exePath.empty()) {
                    text = L"—";
                    color = kTextMuted;
                } else {
                    text = row.exePath;
                    color = selected ? kTextDim : kTextMuted;
                }
                break;
            default:
                break;
        }

        RECT textRc = cell;
        textRc.left = textLeft;
        textRc.right -= S(10);
        if (textRc.right < textRc.left) textRc.right = textRc.left;

        SetTextColor(dc, selected ? (col == 0 ? kText : color) : color);
        DrawTextW(dc, text.c_str(), (int)text.size(), &textRc,
                  fmt | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
        if (cellSaved) RestoreDC(dc, cellSaved);
    }

    if (saved) RestoreDC(dc, saved);
}

void Picker::DrawButton(const DRAWITEMSTRUCT *dis) {
    if (!dis) return;
    HDC dc = dis->hDC;
    RECT rc = dis->rcItem;
    const bool isOk = dis->CtlID == kIdOk;
    const bool disabled = (dis->itemState & ODS_DISABLED) != 0;
    const bool pressed = (dis->itemState & ODS_SELECTED) != 0;
    const bool focused = (dis->itemState & ODS_FOCUS) != 0;
    ButtonView *view = ForButton(dis->hwndItem);
    const bool hot = view && view->hot && !disabled;

    COLORREF fill = kButtonFill;
    COLORREF border = kBorder;
    COLORREF text = kText;

    if (isOk) {
        if (disabled) {
            fill = kDisabledFill;
            border = kDisabledBorder;
            text = kDisabledText;
        } else {
            fill = pressed ? kAccentPressed : (hot ? kAccentHover : kAccent);
            border = fill;
            text = kAccentText;
        }
    } else {
        // 次级按钮沿用调色板，浅色主题下为白底深字（原固定深蓝会导致黑字黑底）
        fill = pressed ? kButtonFillPressed : (hot ? kButtonFillHover : kButtonFill);
        border = hot ? kButtonBorderHover : kBorder;
        text = disabled ? kDisabledText : kText;
    }

    HBRUSH brush = CreateSolidBrush(fill);
    FillRoundRect(dc, rc, S(8), brush, border, (std::max)(1, S(1)));
    DeleteObject(brush);

    const wchar_t *label = isOk ? L"确定" : (dis->CtlID == kIdCancel ? L"取消" : L"刷新");
    SetBkMode(dc, TRANSPARENT);
    if (fontUi) SelectObject(dc, fontUi);
    SetTextColor(dc, text);
    RECT textRc = rc;
    DrawTextW(dc, label, -1, &textRc, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

    if (focused && !disabled) {
        RECT fr = rc;
        InflateRect(&fr, -S(4), -S(4));
        DrawFocusRect(dc, &fr);
    }
}

// ── 子类化与消息处理 ──────────────────────────────────────────────
LRESULT CALLBACK HeaderCustomDraw(Picker *p, LPNMCUSTOMDRAW cd) {
    if (!p || !cd) return CDRF_DODEFAULT;
    if (cd->dwDrawStage == CDDS_PREPAINT) {
        RECT rc = cd->rc;
        FillRect(cd->hdc, &rc, p->brHeader);
        return CDRF_NOTIFYITEMDRAW;
    }
    if (cd->dwDrawStage == CDDS_ITEMPREPAINT) {
        HDC dc = cd->hdc;
        RECT rc = cd->rc;
        FillRect(dc, &rc, p->brHeader);

        RECT bottom = rc;
        bottom.top = bottom.bottom - (std::max)(1, p->S(1));
        FillRect(dc, &bottom, p->brBorder);
        RECT right = rc;
        right.left = right.right - (std::max)(1, p->S(1));
        FillRect(dc, &right, p->brBorder);

        wchar_t text[64] = {};
        HDITEMW item{};
        item.mask = HDI_TEXT;
        item.pszText = text;
        item.cchTextMax = 64;
        SendMessageW(cd->hdr.hwndFrom, HDM_GETITEMW, (WPARAM)cd->dwItemSpec, (LPARAM)&item);

        SetBkMode(dc, TRANSPARENT);
        if (p->fontHeader) SelectObject(dc, p->fontHeader);
        SetTextColor(dc, kTextDim);
        RECT tr = rc;
        tr.left += p->S(12);
        tr.right -= p->S(10);
        UINT fmt = DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX;
        if (cd->dwItemSpec == 2) fmt = DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX;
        DrawTextW(dc, text, -1, &tr, fmt);
        return CDRF_SKIPDEFAULT;
    }
    return CDRF_DODEFAULT;
}

LRESULT CALLBACK ListSubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
                                  UINT_PTR id, DWORD_PTR ref) {
    Picker *p = reinterpret_cast<Picker *>(ref);
    switch (msg) {
        case WM_NOTIFY: {
            NMHDR *nh = reinterpret_cast<NMHDR *>(lParam);
            if (nh && nh->code == NM_CUSTOMDRAW) {
                HWND header = (HWND)SendMessageW(hwnd, LVM_GETHEADER, 0, 0);
                if (nh->hwndFrom == header) {
                    return HeaderCustomDraw(p, reinterpret_cast<LPNMCUSTOMDRAW>(lParam));
                }
            }
            break;
        }
        case WM_ERASEBKGND: {
            if (p && p->brList) {
                RECT rc{};
                GetClientRect(hwnd, &rc);
                FillRect((HDC)wParam, &rc, p->brList);
                return 1;
            }
            break;
        }
        case WM_MOUSEMOVE: {
            if (!p) break;
            LVHITTESTINFO hit{};
            GetCursorPos(&hit.pt);
            ScreenToClient(hwnd, &hit.pt);
            int index = (int)SendMessageW(hwnd, LVM_HITTEST, 0, (LPARAM)&hit);
            if (!(hit.flags & LVHT_ONITEM)) index = -1;
            if (index != p->hotRow) {
                int old = p->hotRow;
                p->hotRow = index;
                p->InvalidateRow(old);
                p->InvalidateRow(index);
            }
            if (!p->listTracking) {
                TRACKMOUSEEVENT tme{};
                tme.cbSize = sizeof(tme);
                tme.dwFlags = TME_LEAVE;
                tme.hwndTrack = hwnd;
                TrackMouseEvent(&tme);
                p->listTracking = true;
            }
            break;
        }
        case WM_MOUSELEAVE: {
            if (!p) break;
            p->listTracking = false;
            int old = p->hotRow;
            p->hotRow = -1;
            p->InvalidateRow(old);
            break;
        }
        case WM_MOUSEWHEEL: {
            if (p) {
                int old = p->hotRow;
                p->hotRow = -1;
                p->InvalidateRow(old);
            }
            break;
        }
        case WM_PAINT: {
            LRESULT result = DefSubclassProc(hwnd, msg, wParam, lParam);
            if (p && SendMessageW(hwnd, LVM_GETITEMCOUNT, 0, 0) == 0) {
                HDC dc = GetDC(hwnd);
                if (dc) {
                    RECT rc{};
                    GetClientRect(hwnd, &rc);
                    SetBkMode(dc, TRANSPARENT);
                    if (p->fontUi) SelectObject(dc, p->fontUi);
                    SetTextColor(dc, kTextMuted);
                    const wchar_t *empty = p->query.empty() ? L"未发现进程" : L"没有匹配的进程";
                    DrawTextW(dc, empty, -1, &rc,
                              DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
                    ReleaseDC(hwnd, dc);
                }
            }
            return result;
        }
        case WM_NCDESTROY:
            RemoveWindowSubclass(hwnd, ListSubclassProc, id);
            break;
        default:
            break;
    }
    LRESULT result = DefSubclassProc(hwnd, msg, wParam, lParam);
    if (p && (msg == WM_VSCROLL || msg == WM_MOUSEWHEEL || msg == WM_KEYDOWN)) {
        p->UpdateScrollbar();
    }
    return result;
}

LRESULT CALLBACK EditSubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
                                  UINT_PTR id, DWORD_PTR ref) {
    Picker *p = reinterpret_cast<Picker *>(ref);
    if (msg == WM_KEYDOWN && p && wParam == VK_DOWN) {
        p->FocusFirstItem();
        return 0;
    }
    if (msg == WM_PAINT) {
        LRESULT result = DefSubclassProc(hwnd, msg, wParam, lParam);
        // 未启用 comctl32 v6 时 EM_SETCUEBANNER 无效，这里自绘占位提示
        if (p && GetWindowTextLengthW(hwnd) == 0) {
            HDC dc = GetDC(hwnd);
            if (dc) {
                RECT rc{};
                GetClientRect(hwnd, &rc);
                SetBkMode(dc, TRANSPARENT);
                if (p->fontUi) SelectObject(dc, p->fontUi);
                SetTextColor(dc, kTextMuted);
                RECT tr = rc;
                tr.left += p->S(2);
                tr.right -= p->S(2);
                DrawTextW(dc, L"搜索进程或窗口标题", -1, &tr,
                          DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
                ReleaseDC(hwnd, dc);
            }
        }
        return result;
    }
    if (msg == WM_SETFOCUS || msg == WM_KILLFOCUS) {
        InvalidateRect(hwnd, nullptr, FALSE);
    }
    if (msg == WM_NCDESTROY) RemoveWindowSubclass(hwnd, EditSubclassProc, id);
    return DefSubclassProc(hwnd, msg, wParam, lParam);
}

LRESULT CALLBACK ButtonSubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
                                    UINT_PTR id, DWORD_PTR ref) {
    Picker *p = reinterpret_cast<Picker *>(ref);
    ButtonView *view = p ? p->ForButton(hwnd) : nullptr;
    switch (msg) {
        case WM_MOUSEMOVE:
            if (view && !view->hot) {
                view->hot = true;
                InvalidateRect(hwnd, nullptr, FALSE);
                TRACKMOUSEEVENT tme{};
                tme.cbSize = sizeof(tme);
                tme.dwFlags = TME_LEAVE;
                tme.hwndTrack = hwnd;
                TrackMouseEvent(&tme);
            }
            break;
        case WM_MOUSELEAVE:
            if (view && view->hot) {
                view->hot = false;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            break;
        case WM_NCDESTROY:
            RemoveWindowSubclass(hwnd, ButtonSubclassProc, id);
            break;
        default:
            break;
    }
    return DefSubclassProc(hwnd, msg, wParam, lParam);
}

Picker *PickerFromWindow(HWND hwnd) {
    return reinterpret_cast<Picker *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
}

LRESULT CALLBACK ScrollbarWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_NCCREATE) {
        CREATESTRUCTW *cs = reinterpret_cast<CREATESTRUCTW *>(lParam);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
    }
    Picker *p = PickerFromWindow(hwnd);

    switch (msg) {
        case WM_ERASEBKGND:
            return 1;

        case WM_PAINT:
            if (p) {
                p->PaintScrollbar();
            } else {
                PAINTSTRUCT ps{};
                BeginPaint(hwnd, &ps);
                EndPaint(hwnd, &ps);
            }
            return 0;

        case WM_LBUTTONDOWN:
            if (p && p->list) {
                int y = (int)(short)HIWORD(lParam);
                POINT pt{(int)(short)LOWORD(lParam), y};
                RECT thumb = p->ScrollThumbRect();
                if (PtInRect(&thumb, pt)) {
                    p->scrollDrag = true;
                    p->scrollDragOffset = y - thumb.top;
                    SetFocus(p->list);
                    SetCapture(hwnd);
                    InvalidateRect(hwnd, nullptr, FALSE);
                } else {
                    SetFocus(p->list);
                    p->ScrollbarTrackClick(y);
                }
                return 0;
            }
            break;

        case WM_MOUSEMOVE:
            if (p) {
                int y = (int)(short)HIWORD(lParam);
                if (p->scrollDrag) {
                    p->ScrollbarDragTo(y);
                } else {
                    POINT pt{(int)(short)LOWORD(lParam), y};
                    RECT thumb = p->ScrollThumbRect();
                    bool hot = PtInRect(&thumb, pt) != 0;
                    if (hot != p->scrollHot) {
                        p->scrollHot = hot;
                        InvalidateRect(hwnd, nullptr, FALSE);
                    }
                    if (!p->scrollTracking) {
                        TRACKMOUSEEVENT tme{};
                        tme.cbSize = sizeof(tme);
                        tme.dwFlags = TME_LEAVE;
                        tme.hwndTrack = hwnd;
                        TrackMouseEvent(&tme);
                        p->scrollTracking = true;
                    }
                }
                return 0;
            }
            break;

        case WM_LBUTTONUP:
            if (p) {
                if (p->scrollDrag) {
                    p->scrollDrag = false;
                    ReleaseCapture();
                    p->UpdateScrollbar();
                }
                return 0;
            }
            break;

        case WM_MOUSELEAVE:
            if (p) {
                p->scrollTracking = false;
                if (p->scrollHot) {
                    p->scrollHot = false;
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
                return 0;
            }
            break;

        case WM_MOUSEWHEEL:
            if (p && p->list) {
                SendMessageW(p->list, WM_MOUSEWHEEL, wParam, lParam);
                p->UpdateScrollbar();
                return 0;
            }
            break;

        case WM_SETCURSOR:
            if (p) {
                POINT pt{};
                GetCursorPos(&pt);
                ScreenToClient(hwnd, &pt);
                RECT thumb = p->ScrollThumbRect();
                if (p->scrollDrag || PtInRect(&thumb, pt)) {
                    SetCursor(LoadCursorW(nullptr, (LPCWSTR)IDC_ARROW));
                    return TRUE;
                }
            }
            break;

        case WM_NCDESTROY:
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            break;

        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

bool FilterKeyMessage(Picker &p, const MSG &msg) {
    if (msg.message != WM_KEYDOWN) return false;
    if (msg.wParam == VK_ESCAPE) {
        p.Cancel();
        return true;
    }
    if (msg.wParam == VK_RETURN) {
        if (msg.hwnd == p.search || msg.hwnd == p.list || msg.hwnd == p.hwnd) {
            p.Confirm();
            return true;
        }
        if (msg.hwnd == p.refresh) {
            p.Reload();
            return true;
        }
        if (msg.hwnd == p.cancel) {
            p.Cancel();
            return true;
        }
        if (msg.hwnd == p.ok) {
            p.Confirm();
            return true;
        }
    }
    if (msg.wParam == VK_DOWN && msg.hwnd == p.search) {
        p.FocusFirstItem();
        return true;
    }
    return false;
}

LRESULT CALLBACK PickerWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_NCCREATE) {
        CREATESTRUCTW *cs = reinterpret_cast<CREATESTRUCTW *>(lParam);
        Picker *p = reinterpret_cast<Picker *>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)p);
        if (p) p->hwnd = hwnd;
    }

    Picker *p = PickerFromWindow(hwnd);

    switch (msg) {
        case WM_CREATE:
            if (!p) return -1;
            p->rowHeight = p->S(26);
            p->CreateFonts();
            p->CreateBrushes();
            p->ComputeRects();
            p->CreateControls();
            p->Layout();
            return 0;

        case WM_SIZE:
            if (p) {
                p->ComputeRects();
                p->Layout();
            }
            return 0;

        case WM_GETMINMAXINFO:
            if (p) {
                MINMAXINFO *mmi = reinterpret_cast<MINMAXINFO *>(lParam);
                mmi->ptMinTrackSize.x = p->S(680);
                mmi->ptMinTrackSize.y = p->S(480);
                return 0;
            }
            break;

        case WM_DPICHANGED:
            if (p) {
                p->dpi = HIWORD(wParam) ? HIWORD(wParam) : 96;
                RECT *suggest = reinterpret_cast<RECT *>(lParam);
                SetWindowPos(hwnd, nullptr, suggest->left, suggest->top,
                             suggest->right - suggest->left, suggest->bottom - suggest->top,
                             SWP_NOZORDER | SWP_NOACTIVATE);
                p->OnDpiChanged();
                return 0;
            }
            break;

        case WM_ERASEBKGND:
            return 1;

        case WM_PAINT:
            if (p) p->Paint();
            return 0;

        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORSTATIC: {
            if (p && p->brPanel) {
                HDC dc = (HDC)wParam;
                SetTextColor(dc, kText);
                SetBkColor(dc, kPanel);
                return (LRESULT)p->brPanel;
            }
            break;
        }

        case WM_CTLCOLORBTN:
            if (p && p->brBack) {
                SetBkColor((HDC)wParam, kBack);
                return (LRESULT)p->brBack;
            }
            break;

        case WM_COMMAND: {
            if (!p) break;
            int id = LOWORD(wParam);
            int code = HIWORD(wParam);
            if (id == kIdSearch) {
                if (code == EN_CHANGE) {
                    p->OnSearchChanged();
                } else if (code == EN_SETFOCUS) {
                    p->searchFocused = true;
                    InvalidateRect(hwnd, &p->rcSearch, FALSE);
                } else if (code == EN_KILLFOCUS) {
                    p->searchFocused = false;
                    InvalidateRect(hwnd, &p->rcSearch, FALSE);
                }
            } else if (id == kIdRefresh && code == BN_CLICKED) {
                p->Reload();
            } else if (id == kIdCancel && code == BN_CLICKED) {
                p->Cancel();
            } else if (id == kIdOk && code == BN_CLICKED) {
                p->Confirm();
            } else if (id == IDOK) {
                p->Confirm();
            } else if (id == IDCANCEL) {
                p->Cancel();
            }
            return 0;
        }

        case WM_NOTIFY: {
            if (!p) break;
            NMHDR *nh = reinterpret_cast<NMHDR *>(lParam);
            if (!nh) break;
            if (nh->idFrom == kIdList) {
                if (nh->code == LVN_ITEMCHANGED) {
                    // 选择变化可能触发键盘自动滚动；只重绘滚动条，避免在列表通知中再次查询滚动信息
                    p->UpdateOk();
                    if (p->scrollBar) InvalidateRect(p->scrollBar, nullptr, FALSE);
                } else if (nh->code == NM_DBLCLK) {
                    LPNMLISTVIEW nv = reinterpret_cast<LPNMLISTVIEW>(lParam);
                    if (nv->iItem >= 0) {
                        LVITEMW state{};
                        state.state = LVIS_SELECTED | LVIS_FOCUSED;
                        state.stateMask = LVIS_SELECTED | LVIS_FOCUSED;
                        SendMessageW(p->list, LVM_SETITEMSTATE, (WPARAM)nv->iItem, (LPARAM)&state);
                        p->Confirm();
                    }
                }
            }
            return 0;
        }

        case WM_MEASUREITEM: {
            MEASUREITEMSTRUCT *mi = reinterpret_cast<MEASUREITEMSTRUCT *>(lParam);
            if (p && mi && mi->CtlType == ODT_LISTVIEW && mi->CtlID == kIdList) {
                mi->itemHeight = (UINT)p->RowHeight();
                return TRUE;
            }
            break;
        }

        case WM_DRAWITEM: {
            DRAWITEMSTRUCT *dis = reinterpret_cast<DRAWITEMSTRUCT *>(lParam);
            if (!p || !dis) break;
            if (dis->CtlType == ODT_LISTVIEW && dis->CtlID == kIdList) {
                p->DrawRow(dis);
                return TRUE;
            }
            if (dis->CtlType == ODT_BUTTON) {
                p->DrawButton(dis);
                return TRUE;
            }
            break;
        }

        case WM_KEYDOWN:
            if (p) {
                if (wParam == VK_ESCAPE) {
                    p->Cancel();
                    return 0;
                }
                if (wParam == VK_RETURN) {
                    p->Confirm();
                    return 0;
                }
            }
            break;

        case WM_LBUTTONDOWN:
            if (p) {
                POINT pt{(int)(short)LOWORD(lParam), (int)(short)HIWORD(lParam)};
                if (PtInRect(&p->rcSearch, pt) && p->search) {
                    SetFocus(p->search);
                    return 0;
                }
            }
            break;

        case WM_CLOSE:
            if (p) {
                p->Cancel();
                return 0;
            }
            break;

        case WM_DESTROY:
            return 0;

        case WM_NCDESTROY:
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            break;

        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

bool RegisterPickerClass(HINSTANCE inst) {
    static bool registered = false;
    if (registered) return true;

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = PickerWndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, (LPCWSTR)IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = L"DnsPilot.ProcessPicker";

    ATOM atom = RegisterClassExW(&wc);
    if (!atom && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;

    WNDCLASSEXW sc{};
    sc.cbSize = sizeof(sc);
    sc.style = CS_HREDRAW | CS_VREDRAW;
    sc.lpfnWndProc = ScrollbarWndProc;
    sc.hInstance = inst;
    sc.hCursor = LoadCursorW(nullptr, (LPCWSTR)IDC_ARROW);
    sc.hbrBackground = nullptr;
    sc.lpszClassName = L"DnsPilot.PickerScroll";
    ATOM satom = RegisterClassExW(&sc);
    if (!satom && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;

    registered = true;
    return true;
}

}  // namespace

void SetTheme(bool light) {
    gLightTheme = light;
    // 只更新配色变量，不引用任何 HWND：本函数可能在窗口创建之前被调用。
    // 画刷在 Show() -> WM_CREATE -> CreateBrushes() 时按当前配色重建，并在 Dispose() 删除。
    if (gLightTheme) {
        kBack = RGB(245, 247, 250);                // #F5F7FA
        kPanel = RGB(255, 255, 255);               // #FFFFFF
        kListBg = RGB(255, 255, 255);              // #FFFFFF
        kRow = RGB(255, 255, 255);                 // #FFFFFF
        kRowAlt = RGB(247, 250, 252);              // #F7FAFC
        kRowHover = RGB(241, 245, 249);            // #F1F5F9
        kRowSelected = RGB(234, 246, 250);         // #EAF6FA
        kHeader = RGB(247, 249, 252);              // #F7F9FC
        kBorder = RGB(227, 232, 239);              // #E3E8EF
        kRowLine = RGB(237, 241, 246);             // #EDF1F6
        kText = RGB(27, 36, 51);                   // #1B2433
        kTextDim = RGB(91, 107, 128);              // #5B6B80
        kTextMuted = RGB(133, 147, 165);           // #8593A5
        kAccent = RGB(11, 127, 150);               // #0B7F96，白字对比度约 4.7:1
        kAccentHover = RGB(14, 147, 172);          // #0E93AC
        kAccentPressed = RGB(9, 107, 128);         // #096B80
        kAccentText = RGB(255, 255, 255);          // 强调按钮上为白字
        kDisabledFill = RGB(241, 245, 249);        // #F1F5F9
        kDisabledBorder = RGB(227, 232, 239);      // #E3E8EF
        kDisabledText = RGB(163, 175, 191);        // #A3AFBF
        kScrollThumb = RGB(199, 210, 224);         // #C7D2E0
        kScrollThumbActive = RGB(148, 163, 184);   // #94A3B8
        kButtonFill = RGB(255, 255, 255);          // 与面板同色
        kButtonFillHover = RGB(241, 245, 249);     // #F1F5F9
        kButtonFillPressed = RGB(232, 238, 245);   // #E8EEF5
        kButtonBorderHover = RGB(203, 213, 225);   // #CBD5E1
        kIconPlaceholder = RGB(232, 237, 243);     // #E8EDF3
    } else {
        kBack = RGB(7, 11, 21);
        kPanel = RGB(14, 21, 35);
        kListBg = RGB(11, 17, 29);
        kRow = RGB(12, 19, 31);
        kRowAlt = RGB(14, 22, 36);
        kRowHover = RGB(19, 31, 48);
        kRowSelected = RGB(16, 45, 60);
        kHeader = RGB(18, 27, 44);
        kBorder = RGB(30, 41, 61);
        kRowLine = RGB(18, 26, 40);
        kText = RGB(226, 232, 240);
        kTextDim = RGB(148, 163, 184);
        kTextMuted = RGB(100, 116, 139);
        kAccent = RGB(34, 211, 238);
        kAccentHover = RGB(103, 232, 249);
        kAccentPressed = RGB(14, 165, 233);
        kAccentText = RGB(4, 15, 24);
        kDisabledFill = RGB(18, 26, 40);
        kDisabledBorder = RGB(30, 40, 58);
        kDisabledText = RGB(92, 105, 125);
        kScrollThumb = RGB(55, 70, 94);
        kScrollThumbActive = RGB(82, 101, 129);
        kButtonFill = RGB(18, 27, 43);
        kButtonFillHover = RGB(24, 35, 53);
        kButtonFillPressed = RGB(14, 22, 36);
        kButtonBorderHover = RGB(49, 66, 94);
        kIconPlaceholder = RGB(22, 31, 48);
    }
}

bool Show(HWND owner, Picked &out) {
    // 尽可能让本进程按显示器 DPI 工作；进程已经设置过时调用失败，可安全忽略
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    INITCOMMONCONTROLSEX icc{};
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icc);

    HINSTANCE inst = (HINSTANCE)GetModuleHandleW(nullptr);
    if (!RegisterPickerClass(inst)) return false;

    Picker p;
    p.owner = owner;

    UINT dpi = owner ? GetDpiForWindow(owner) : GetDpiForSystem();
    if (!dpi) dpi = 96;
    p.dpi = dpi;

    int w = p.S(860);
    int h = p.S(620);

    // 让默认尺寸在较小工作区也能完整显示
    HMONITOR monitor = MonitorFromWindow(owner ? owner : GetForegroundWindow(),
                                         MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    RECT work{};
    if (GetMonitorInfoW(monitor, &mi)) {
        work = mi.rcWork;
    } else {
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    }
    int maxW = work.right - work.left - p.S(20);
    int maxH = work.bottom - work.top - p.S(20);
    if (maxW > p.S(360) && w > maxW) w = maxW;
    if (maxH > p.S(280) && h > maxH) h = maxH;

    int x = 0;
    int y = 0;
    RECT anchor{};
    if (owner && GetWindowRect(owner, &anchor)) {
        x = anchor.left + (anchor.right - anchor.left - w) / 2;
        y = anchor.top + (anchor.bottom - anchor.top - h) / 2;
    } else {
        x = work.left + (work.right - work.left - w) / 2;
        y = work.top + (work.bottom - work.top - h) / 2;
    }
    if (x + w > work.right) x = work.right - w;
    if (x < work.left) x = work.left;
    if (y + h > work.bottom) y = work.bottom - h;
    if (y < work.top) y = work.top;

    DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME | WS_CLIPCHILDREN;
    HWND hwnd = CreateWindowExW(0, L"DnsPilot.ProcessPicker", L"选择进程", style, x, y, w, h, owner,
                                nullptr, inst, &p);
    if (!hwnd) return false;

    // 标题栏跟随主题：浅色关闭沉浸式深色，边框改用浅色 kBorder（#E3E8EF）；
    // 现有代码只设置 dark 与 border 两项，不新增可能失败的颜色属性
    BOOL dark = gLightTheme ? FALSE : TRUE;
    if (FAILED(DwmSetWindowAttribute(hwnd, kDwmUseImmersiveDarkMode, &dark, sizeof(dark)))) {
        DwmSetWindowAttribute(hwnd, kDwmUseImmersiveDarkModeOld, &dark, sizeof(dark));
    }
    COLORREF borderColor = kBorder;
    DwmSetWindowAttribute(hwnd, kDwmBorderColor, &borderColor, sizeof(borderColor));

    if (owner) EnableWindow(owner, FALSE);

    p.Reload();

    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);
    SetForegroundWindow(hwnd);
    SetFocus(p.search);

    bool quit = false;
    MSG msg{};
    while (IsWindow(hwnd)) {
        BOOL got = GetMessageW(&msg, nullptr, 0, 0);
        if (got == 0) {
            quit = true;
            break;
        }
        if (got == -1) break;
        if (FilterKeyMessage(p, msg)) continue;
        if (IsDialogMessageW(hwnd, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (IsWindow(hwnd)) DestroyWindow(hwnd);
    if (owner && IsWindow(owner)) {
        EnableWindow(owner, TRUE);
        SetForegroundWindow(owner);
    }

    p.Dispose();

    if (quit) PostQuitMessage((int)msg.wParam);
    if (p.confirmed) {
        out = p.result;
        return true;
    }
    return false;
}

}  // namespace procpick
