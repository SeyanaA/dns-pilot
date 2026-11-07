// 最近 60 秒请求量柱状图：GDI+ 绘制渐变柱，GDI 绘制文字（避免每帧创建 GDI+ 字体）
#include "ui_chart.h"

#include <windows.h>

#ifndef WM_DPICHANGED_AFTERPARENT
#define WM_DPICHANGED_AFTERPARENT 0x02E3
#endif

#include <gdiplus.h>

#include <algorithm>
#include <mutex>
#include <string>
#include <vector>

using namespace Gdiplus;

namespace {

const wchar_t *kClassName = L"DNSPilotChart";

// 当前是否为浅色主题；由 SetTheme 维护，绘制时配色变量已随之更新
bool gLightTheme = false;

// 图表配色：默认深色，SetTheme 切换主题时整体重设
Color kBg(255, 14, 23, 40);       // #0e1728，与卡片底色一致
Color kGrid(255, 28, 42, 66);     // 网格线
Color kCyan(255, 56, 225, 255);
Color kViolet(255, 139, 123, 255);
Color kMaxText(255, 176, 192, 214);
Color kMuted(255, 139, 155, 180);
Color kEmpty(255, 92, 107, 131);

struct ChartState {
    std::mutex mu;  // 保护 SetData 与绘制线程之间的数据读写
    std::vector<uint32_t> data;
    std::wstring rightLabel;
    UINT dpi = 96;
    HFONT fontLabel = nullptr;
    HFONT fontEmpty = nullptr;
    bool gdipOk = false;
    bool gdipRef = false;
};

// CreateWindowEx 期间传递状态，用于区分 WM_CREATE 是否已接管所有权
struct CreateCtx {
    ChartState *state = nullptr;
    bool consumed = false;
};

// GDI+ 按控件实例引用计数延迟初始化，最后一个控件销毁时关闭
std::mutex gGdipMu;
ULONG_PTR gGdipToken = 0;
int gGdipRefs = 0;

bool gdipAddRef() {
    std::lock_guard<std::mutex> lk(gGdipMu);
    if (gGdipRefs == 0) {
        GdiplusStartupInput input;
        if (GdiplusStartup(&gGdipToken, &input, nullptr) != Ok) {
            gGdipToken = 0;
            return false;
        }
    }
    gGdipRefs++;
    return true;
}

void gdipRelease() {
    std::lock_guard<std::mutex> lk(gGdipMu);
    if (gGdipRefs > 0) gGdipRefs--;
    if (gGdipRefs == 0 && gGdipToken) {
        GdiplusShutdown(gGdipToken);
        gGdipToken = 0;
    }
}

using GetDpiForWindowFn = UINT(WINAPI *)(HWND);

UINT dpiOf(HWND hwnd) {
    static GetDpiForWindowFn fn = reinterpret_cast<GetDpiForWindowFn>(
        reinterpret_cast<void *>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow")));
    if (fn) {
        UINT d = fn(hwnd);
        if (d) return d;
    }
    UINT d = 96;
    HDC dc = GetDC(hwnd);
    if (dc) {
        int v = GetDeviceCaps(dc, LOGPIXELSX);
        if (v > 0) d = (UINT)v;
        ReleaseDC(hwnd, dc);
    }
    return d;
}

int CALLBACK fontEnumProc(const LOGFONTW *, const TEXTMETRICW *, DWORD, LPARAM lp) {
    *reinterpret_cast<bool *>(lp) = true;
    return 0;
}

bool fontExists(const wchar_t *face) {
    HDC dc = GetDC(nullptr);
    if (!dc) return false;
    LOGFONTW lf{};
    lf.lfCharSet = DEFAULT_CHARSET;
    lstrcpynW(lf.lfFaceName, face, LF_FACESIZE);
    bool found = false;
    EnumFontFamiliesExW(dc, &lf, fontEnumProc, reinterpret_cast<LPARAM>(&found), 0);
    ReleaseDC(nullptr, dc);
    return found;
}

const wchar_t *uiFace() {
    static const wchar_t *face = fontExists(L"Microsoft YaHei UI") ? L"Microsoft YaHei UI" : L"Segoe UI";
    return face;
}

HFONT makeFont(UINT dpi, int logicalPx, int weight) {
    // 深色背景上用灰度抗锯齿，避免 ClearType 彩色边缘
    return CreateFontW(-MulDiv(logicalPx, (int)dpi, 96), 0, 0, 0, weight, FALSE, FALSE, FALSE,
                       DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                       DEFAULT_PITCH | FF_DONTCARE, uiFace());
}

void freeFonts(ChartState *st) {
    if (st->fontLabel) DeleteObject(st->fontLabel);
    if (st->fontEmpty) DeleteObject(st->fontEmpty);
    st->fontLabel = nullptr;
    st->fontEmpty = nullptr;
}

void ensureFonts(ChartState *st, UINT dpi) {
    if (st->fontLabel && st->fontEmpty && st->dpi == dpi) return;
    freeFonts(st);
    st->dpi = dpi;
    st->fontLabel = makeFont(dpi, 12, FW_SEMIBOLD);
    st->fontEmpty = makeFont(dpi, 13, FW_NORMAL);
}

Color mixColor(const Color &a, const Color &b, float t) {
    auto mix = [t](BYTE x, BYTE y) { return (BYTE)((float)x + ((float)y - (float)x) * t + 0.5f); };
    return Color(255, mix(a.GetR(), b.GetR()), mix(a.GetG(), b.GetG()), mix(a.GetB(), b.GetB()));
}

Color shade(const Color &c, float f) {
    auto dim = [f](BYTE v) { return (BYTE)((float)v * f + 0.5f); };
    return Color(255, dim(c.GetR()), dim(c.GetG()), dim(c.GetB()));
}

void drawText(HDC dc, const std::wstring &text, RECT rc, HFONT font, const Color &color, UINT fmt) {
    if (!font || text.empty()) return;
    HGDIOBJ old = SelectObject(dc, font);
    int oldMode = SetBkMode(dc, TRANSPARENT);
    COLORREF oldColor = SetTextColor(dc, RGB(color.GetR(), color.GetG(), color.GetB()));
    DrawTextW(dc, text.c_str(), (int)text.size(), &rc, fmt | DT_NOPREFIX);
    SetTextColor(dc, oldColor);
    SetBkMode(dc, oldMode);
    SelectObject(dc, old);
}

// 绘制整个控件；dc 为双缓冲内存 DC
void drawAll(HDC dc, const RECT &rc, ChartState *st, UINT dpi) {
    int w = rc.right - rc.left;
    int h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) return;

    std::vector<uint32_t> data;
    std::wstring rightLabel;
    if (st) {
        std::lock_guard<std::mutex> lk(st->mu);
        data = st->data;
        rightLabel = st->rightLabel;
    }

    auto S = [dpi](int v) { return MulDiv(v, (int)dpi, 96); };

    int padX = S(12), padTop = S(9), padBottom = S(8), labelH = S(18);
    RECT plot{padX, padTop + labelH, w - padX, h - padBottom};
    int plotW = plot.right - plot.left;
    int plotH = plot.bottom - plot.top;
    if (plotW <= 0 || plotH <= 0) return;

    uint32_t maxV = 0;
    for (uint32_t v : data)
        if (v > maxV) maxV = v;

    // 背景用 GDI 精确铺满客户区：GDI+ 的 FillRectangle 在 HDC 上有 0.5px 栅格偏移，
    // 首行/首列只会覆盖一半，在内存位图上混出一条灰边（浅色主题下非常明显）。
    HBRUSH bgBrush = CreateSolidBrush(RGB(kBg.GetR(), kBg.GetG(), kBg.GetB()));
    FillRect(dc, &rc, bgBrush);
    DeleteObject(bgBrush);

    bool gdip = st && st->gdipOk;
    if (gdip) {
        Graphics g(dc);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        // 关闭 GDI+ 的半像素偏移：网格线与柱子的边缘落在整像素上，1px 线不再糊成 2px
        g.SetPixelOffsetMode(PixelOffsetModeNone);

        Pen grid(kGrid, 1.0f);
        for (int i = 1; i <= 3; i++) {
            int y = plot.bottom - (plotH * i) / 4;
            g.DrawLine(&grid, (REAL)plot.left, (REAL)y, (REAL)plot.right, (REAL)y);
        }

        int n = (int)data.size();
        if (n > 0 && maxV > 0) {
            float slot = (float)plotW / (float)n;
            float gap = slot > 4.0f ? slot * 0.28f : (slot > 2.0f ? 1.0f : 0.0f);
            float barW = slot - gap;
            if (barW < 0.6f) barW = slot;
            for (int i = 0; i < n; i++) {
                float t = n > 1 ? (float)i / (float)(n - 1) : 0.0f;
                Color base = mixColor(kCyan, kViolet, t);
                float x = (float)plot.left + slot * (float)i + gap * 0.5f;
                float bh = (float)plotH * (float)data[i] / (float)maxV;
                if (data[i] > 0 && bh < 2.0f) bh = 2.0f;
                if (bh > (float)plotH) bh = (float)plotH;
                if (data[i] == 0) {
                    // 空白秒保留一个很淡的基线，方便看出时间窗口是连续的
                    SolidBrush stub(Color(46, base.GetR(), base.GetG(), base.GetB()));
                    g.FillRectangle(&stub, x, (REAL)(plot.bottom - 2), barW, 2.0f);
                    continue;
                }
                RectF bar(x, (REAL)plot.bottom - bh, barW, bh);
                Color top(255, base.GetR(), base.GetG(), base.GetB());
                Color bottom = shade(base, 0.52f);
                LinearGradientBrush brush(bar, top, bottom, LinearGradientModeVertical);
                g.FillRectangle(&brush, bar);
            }
        }
    }

    if (!st) return;
    ensureFonts(st, dpi);

    std::wstring maxLabel = std::to_wstring(maxV) + L" req/s";
    RECT maxRc{padX, padTop, w / 2, padTop + labelH};
    drawText(dc, maxLabel, maxRc, st->fontLabel, kMaxText, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    RECT rightRc{w / 2, padTop, w - padX, padTop + labelH};
    drawText(dc, rightLabel, rightRc, st->fontLabel, kMuted, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);

    if (data.empty() || maxV == 0) {
        RECT emptyRc{plot.left, plot.top, plot.right, plot.bottom};
        drawText(dc, L"暂无请求", emptyRc, st->fontEmpty, kEmpty, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
}

void paint(HWND hwnd) {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(hwnd, &ps);
    RECT rc{};
    GetClientRect(hwnd, &rc);
    int w = rc.right - rc.left;
    int h = rc.bottom - rc.top;
    ChartState *st = reinterpret_cast<ChartState *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    UINT dpi = dpiOf(hwnd);
    if (w > 0 && h > 0) {
        HDC mem = CreateCompatibleDC(dc);
        HBITMAP bmp = mem ? CreateCompatibleBitmap(dc, w, h) : nullptr;
        if (mem && bmp) {
            HGDIOBJ old = SelectObject(mem, bmp);
            drawAll(mem, rc, st, dpi);
            BitBlt(dc, 0, 0, w, h, mem, 0, 0, SRCCOPY);
            SelectObject(mem, old);
            DeleteObject(bmp);
        } else {
            drawAll(dc, rc, st, dpi);
        }
        if (mem) DeleteDC(mem);
    }
    EndPaint(hwnd, &ps);
}

LRESULT CALLBACK ChartProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE: {
            const CREATESTRUCTW *cs = reinterpret_cast<const CREATESTRUCTW *>(lp);
            CreateCtx *ctx = cs ? reinterpret_cast<CreateCtx *>(cs->lpCreateParams) : nullptr;
            ChartState *st = ctx ? ctx->state : nullptr;
            if (!st) st = new ChartState();
            if (ctx) ctx->consumed = true;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(st));
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT:
            paint(hwnd);
            return 0;
        case WM_SIZE:
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case WM_DPICHANGED_AFTERPARENT:
            if (auto *st = reinterpret_cast<ChartState *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA)))
                freeFonts(st);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case WM_NCDESTROY: {
            ChartState *st = reinterpret_cast<ChartState *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
            bool release = false;
            if (st) {
                release = st->gdipRef;
                freeFonts(st);
                SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
                delete st;
            }
            if (release) gdipRelease();
            break;
        }
        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace

namespace uichart {

void SetTheme(bool light) {
    gLightTheme = light;
    // 只更新配色变量，不依赖任何 HWND：本函数可能在图表窗口创建之前被调用
    if (gLightTheme) {
        kBg = Color(255, 255, 255, 255);      // 与白色卡片底色一致
        kGrid = Color(255, 227, 232, 239);    // #E3E8EF
        kCyan = Color(255, 56, 189, 248);     // #38BDF8
        kViolet = Color(255, 129, 140, 248);  // #818CF8
        kMaxText = Color(255, 91, 107, 128);  // #5B6B80
        kMuted = Color(255, 91, 107, 128);    // #5B6B80
        kEmpty = Color(255, 133, 147, 165);   // #8593A5
    } else {
        kBg = Color(255, 14, 23, 40);       // #0e1728，与卡片底色一致
        kGrid = Color(255, 28, 42, 66);     // 网格线
        kCyan = Color(255, 56, 225, 255);
        kViolet = Color(255, 139, 123, 255);
        kMaxText = Color(255, 176, 192, 214);
        kMuted = Color(255, 139, 155, 180);
        kEmpty = Color(255, 92, 107, 131);
    }
}

void Register(HINSTANCE inst) {
    static std::mutex mu;
    static bool done = false;
    std::lock_guard<std::mutex> lk(mu);
    if (done) return;
    HINSTANCE module = inst ? inst : GetModuleHandleW(nullptr);
    WNDCLASSEXW existing{};
    existing.cbSize = sizeof(existing);
    if (GetClassInfoExW(module, kClassName, &existing)) {
        done = true;
        return;
    }
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = ChartProc;
    wc.hInstance = module;
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));  // IDC_ARROW
    wc.lpszClassName = kClassName;
    if (RegisterClassExW(&wc)) done = true;
}

HWND Create(HWND parent, int id) {
    if (!parent) return nullptr;
    HINSTANCE module = reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(parent, GWLP_HINSTANCE));
    if (!module) module = GetModuleHandleW(nullptr);
    Register(module);
    CreateCtx ctx;
    ctx.state = new ChartState();
    HWND hwnd = CreateWindowExW(0, kClassName, L"", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS, 0, 0, 0, 0,
                                parent, reinterpret_cast<HMENU>((INT_PTR)id), module, &ctx);
    if (!hwnd) {
        if (!ctx.consumed) delete ctx.state;
        return nullptr;
    }
    ctx.state->gdipOk = gdipAddRef();
    ctx.state->gdipRef = ctx.state->gdipOk;
    return hwnd;
}

void SetData(HWND chart, const std::vector<uint32_t> &data, const std::wstring &rightLabel) {
    if (!chart || !IsWindow(chart)) return;
    ChartState *st = reinterpret_cast<ChartState *>(GetWindowLongPtrW(chart, GWLP_USERDATA));
    if (!st) return;
    {
        std::lock_guard<std::mutex> lk(st->mu);
        st->data = data;
        st->rightLabel = rightLabel;
    }
    InvalidateRect(chart, nullptr, FALSE);
}

}  // namespace uichart
