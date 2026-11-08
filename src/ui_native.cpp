// 原生 Win32 界面：侧边栏、总览、规则、DNS 配置、浏览器绑定、系统接管、设置
#include "ui_native.h"

#include <commctrl.h>
#include <commdlg.h>
#include <dwmapi.h>
#include <gdiplus.h>
#include <windows.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cwchar>
#include <functional>
#include <string>
#include <vector>

#include "browsers.h"
#include "process_picker.h"
#include "systemdns.h"
#include "ui_chart.h"
#include "util.h"

using namespace Gdiplus;

namespace {

// ---------------------------------------------------------------- 主题调色板
// 这里的颜色是唯一来源：深色值为原有配色，浅色值由 applyTheme() 整体覆盖。
// 变量必须是可赋值的（不能是 const），否则无法在运行时切换主题。
Color kBg(255, 7, 11, 21);
Color kPanel(255, 11, 18, 32);
Color kCard(255, 14, 23, 40);
Color kCard2(255, 17, 29, 51);
Color kLine(255, 28, 42, 66);
Color kLine2(255, 44, 62, 92);
Color kText(255, 230, 237, 247);
Color kMuted(255, 139, 155, 180);
Color kDim(255, 92, 107, 131);
Color kCyan(255, 56, 225, 255);
Color kViolet(255, 139, 123, 255);
Color kGreen(255, 52, 211, 153);
Color kAmber(255, 251, 191, 36);
Color kRed(255, 251, 113, 133);

// 表面色：原先是散落在各处的硬编码 Color(255,...)，现在集中到这里
Color kTitleBg(255, 9, 15, 27);
Color kStripe(255, 12, 20, 34);
Color kInputBg(255, 10, 16, 29);
Color kInputBg2(255, 12, 19, 33);
Color kSelectBg(255, 22, 40, 64);
Color kBrowseBg(255, 18, 31, 54);
Color kBrowseHot(255, 26, 45, 72);
Color kBtnBg(255, 17, 29, 51);
Color kBtnPrimary(255, 20, 40, 62);
Color kBtnPrimaryForm(255, 23, 45, 66);
Color kBtnPrimaryBorder(255, 50, 160, 190);
Color kBtnPrimaryBorderForm(255, 56, 180, 210);
Color kNavActiveBg(255, 20, 36, 58);
Color kNavIdleBg(255, 16, 27, 46);
Color kGlassBg(255, 15, 25, 43);
Color kInfoBg(255, 17, 28, 46);
Color kInfoBorder(255, 52, 70, 100);
Color kSysRowBg(255, 20, 30, 46);
Color kLineSoft(255, 22, 32, 50);
Color kTitleHoverBg(255, 30, 44, 68);
Color kCloseHoverBg(255, 170, 45, 55);
Color kTitleGlyph(255, 200, 212, 230);
Color kDomainText(255, 219, 234, 254);
Color kToggleOnBg(255, 27, 74, 62);
Color kToggleOffBg(255, 30, 40, 58);
Color kToggleKnobOff(255, 150, 163, 184);
Color kBadgeGoodBg(255, 16, 44, 36);
Color kBadgeWarnBg(255, 48, 40, 16);
Color kBadgeBadBg(255, 48, 22, 28);
Color kBadgeInfoBg(255, 16, 40, 55);
Color kBadgeNeutralBg(255, 22, 30, 44);
Color kBadgeProxyFg(255, 200, 192, 255);
Color kBadgeProxyBg(255, 34, 29, 55);
Color kBadgeDnsFg(255, 155, 232, 255);
Color kTakeoverOnBg(255, 13, 34, 32);
Color kTakeoverOnBorder(255, 40, 120, 96);
Color kTakeoverOffBg(255, 33, 28, 16);
Color kTakeoverOffBorder(255, 90, 78, 30);
Color kGlowA(18, 56, 225, 255);
Color kGlowB(0, 7, 11, 21);
Color kLogoText(255, 255, 255, 255);

bool gLightTheme = false;

const wchar_t *kNavNames[] = {L"总览", L"应用规则", L"域名规则", L"DNS 服务器",
                              L"浏览器绑定", L"系统接管", L"绑定查询", L"设置"};
const wchar_t *kPageTitles[] = {L"总览", L"应用规则", L"域名规则", L"DNS 服务器",
                                L"浏览器绑定", L"系统接管", L"绑定查询", L"设置"};
const wchar_t *kPageSubs[] = {
    L"实时解析流、规则命中与缓存效果",
    L"让指定进程单独使用某个 DNS，可直接从运行中的进程选择",
    L"让指定网站及其子域单独使用某个 DNS",
    L"管理 DNS 服务器配置，主用优先、失败回退",
    L"修改浏览器配置文件夹中的安全 DNS（DoH），按浏览器配置单独绑定",
    L"把系统 DNS 指向本机解析器，让域名规则全局生效",
    L"查看某个程序或域名当前生效的 DNS 绑定",
    L"缓存、代理、端口与退出行为",
};

const int kSidebarW = 216;
const int kHeaderH = 72;
const int kTitleH = 52;
const int kPageCount = 8;

enum {
    A_NONE = 0,
    A_APP_TOGGLE,
    A_APP_EDIT,
    A_APP_DEL,
    A_APP_ADD,
    A_DOM_TOGGLE,
    A_DOM_EDIT,
    A_DOM_DEL,
    A_DOM_ADD,
    A_PROF_EDIT,
    A_PROF_DEL,
    A_PROF_ADD,
    A_BROWSER_BIND,
    A_BROWSER_RESET,
    A_BROWSER_REFRESH,
    A_TAKEOVER,
    A_RESTORE,
    A_ELEVATE,
    A_SETTINGS,
    A_CLEAR,
    A_QUIT,
    A_LOOKUP_EXE,
    A_LOOKUP_DOMAIN,
    A_EXPORT_CONFIG,
    A_EXPORT_PROFILE,
    A_IMPORT_CONFIG,
    A_OPEN_PANEL,
};

enum { ID_TIMER = 1, ID_CHART = 100 };

struct Action {
    RECT r{};
    int kind = A_NONE;
    int index = -1;
};

struct UIState {
    HWND hwnd = nullptr;
    HWND chart = nullptr;
    HINSTANCE inst = nullptr;
    ui::Context *ctx = nullptr;
    int page = 0;
    int dpi = 96;
    int navHover = -1;
    int titleHot = 0;  // 1=最小化 2=最大化/还原 3=关闭 4=主题
    RECT btnMin{}, btnMax{}, btnClose{}, btnTheme{};
    int scroll[kPageCount] = {0};
    int maxScroll[kPageCount] = {0};
    uint64_t startMs = 0;
    uint64_t lastAdapter = 0;
    UiStats stats;
    Config cfg;
    std::vector<BrowserProfileInfo> browsers;
    std::vector<AdapterInfo> adapters;
    std::vector<RECT> navRects;
    std::vector<Action> actions;
    std::wstring toast;
    uint64_t toastUntil = 0;
    // 图表子窗口位置缓存：内容每秒刷新时不必重复 SetWindowPos/ShowWindow
    RECT chartRect{};
    bool chartPlaced = false;
    // 绑定查询页的输入（只读查询，不写配置）
    std::wstring lookupExe;
    std::wstring lookupDomain;
    // 关闭确认：系统注销/关机时不再弹窗；避免重复打开确认框
    bool sessionEnding = false;
    bool closePromptOpen = false;

    FontFamily *family = nullptr;
    Font *fH1 = nullptr;
    Font *fH2 = nullptr;
    Font *fBold = nullptr;
    Font *fBody = nullptr;
    Font *fSmall = nullptr;
    Font *fTiny = nullptr;
    Font *fKpi = nullptr;
    Font *fMono = nullptr;
    HBRUSH editBrush = nullptr;
    HBRUSH formBrush = nullptr;
    HFONT editFont = nullptr;
};

UIState uiState;

int S(int v) { return MulDiv(v, uiState.dpi, 96); }

// ---------------------------------------------------------------- 主题切换

// 标题栏由 DWM 绘制的部分（表单对话框的系统标题栏、主窗口外框）跟随主题
void applyDwmTheme(HWND hwnd) {
    if (!hwnd) return;
    BOOL dark = gLightTheme ? FALSE : TRUE;
    if (FAILED(DwmSetWindowAttribute(hwnd, 20, &dark, sizeof(dark))))
        DwmSetWindowAttribute(hwnd, 19, &dark, sizeof(dark));
    COLORREF cap = gLightTheme ? RGB(255, 255, 255) : RGB(11, 18, 32);
    DwmSetWindowAttribute(hwnd, 35, &cap, sizeof(cap));
    COLORREF txt = gLightTheme ? RGB(27, 36, 51) : RGB(230, 237, 247);
    DwmSetWindowAttribute(hwnd, 36, &txt, sizeof(txt));
}

// uxtheme 的进程级明暗模式：影响 MessageBox、滚动条等系统控件
void setAppThemeMode(bool light) {
    if (HMODULE ux = LoadLibraryW(L"uxtheme.dll")) {
        using SetModeFn = int(WINAPI *)(int);
        auto setMode = (SetModeFn)GetProcAddress(ux, MAKEINTRESOURCEA(135));
        // 0=Default 1=AllowDark 2=ForceDark 3=ForceLight
        if (setMode) setMode(light ? 3 : 2);
        using FlushFn = void(WINAPI *)();
        auto flush = (FlushFn)GetProcAddress(ux, MAKEINTRESOURCEA(136));
        if (flush) flush();
    }
}

// 切换整套调色板。必须在绘制前调用；会重建颜色相关的 GDI 画刷并刷新已存在的窗口。
void applyTheme(bool light) {
    gLightTheme = light;
    if (light) {
        kBg = Color(255, 245, 247, 250);
        kPanel = Color(255, 255, 255, 255);
        kCard = Color(255, 255, 255, 255);
        kCard2 = Color(255, 247, 249, 252);
        kLine = Color(255, 227, 232, 239);
        kLine2 = Color(255, 203, 213, 225);
        kText = Color(255, 27, 36, 51);
        kMuted = Color(255, 91, 107, 128);
        kDim = Color(255, 107, 122, 145);
        kCyan = Color(255, 11, 127, 150);
        kViolet = Color(255, 109, 91, 208);
        kGreen = Color(255, 15, 122, 87);
        kAmber = Color(255, 166, 102, 15);
        kRed = Color(255, 194, 65, 79);

        kTitleBg = Color(255, 255, 255, 255);
        kStripe = Color(255, 247, 250, 252);
        kInputBg = Color(255, 255, 255, 255);
        kInputBg2 = Color(255, 255, 255, 255);
        kSelectBg = Color(255, 234, 246, 250);
        kBrowseBg = Color(255, 255, 255, 255);
        kBrowseHot = Color(255, 241, 245, 249);
        kBtnBg = Color(255, 255, 255, 255);
        kBtnPrimary = Color(255, 230, 246, 250);
        kBtnPrimaryForm = Color(255, 230, 246, 250);
        kBtnPrimaryBorder = Color(255, 11, 127, 150);
        kBtnPrimaryBorderForm = Color(255, 11, 127, 150);
        kNavActiveBg = Color(255, 234, 246, 250);
        kNavIdleBg = Color(255, 247, 249, 252);
        kGlassBg = Color(255, 255, 255, 255);
        kInfoBg = Color(255, 247, 249, 252);
        kInfoBorder = Color(255, 227, 232, 239);
        kSysRowBg = Color(255, 247, 249, 252);
        kLineSoft = Color(255, 237, 241, 246);
        kTitleHoverBg = Color(255, 241, 245, 249);
        kCloseHoverBg = Color(255, 196, 43, 28);
        kTitleGlyph = Color(255, 91, 107, 128);
        kDomainText = Color(255, 27, 36, 51);
        kToggleOnBg = Color(255, 223, 242, 234);
        kToggleOffBg = Color(255, 227, 232, 239);
        kToggleKnobOff = Color(255, 148, 163, 184);
        kBadgeGoodBg = Color(255, 231, 246, 239);
        kBadgeWarnBg = Color(255, 253, 243, 226);
        kBadgeBadBg = Color(255, 253, 232, 235);
        kBadgeInfoBg = Color(255, 230, 246, 250);
        kBadgeNeutralBg = Color(255, 241, 245, 249);
        kBadgeProxyFg = Color(255, 109, 91, 208);
        kBadgeProxyBg = Color(255, 239, 236, 250);
        kBadgeDnsFg = Color(255, 11, 127, 150);
        kTakeoverOnBg = Color(255, 231, 246, 239);
        kTakeoverOnBorder = Color(255, 15, 122, 87);
        kTakeoverOffBg = Color(255, 253, 243, 226);
        kTakeoverOffBorder = Color(255, 166, 102, 15);
        kGlowA = Color(22, 11, 127, 150);
        kGlowB = Color(0, 245, 247, 250);
    } else {
        kBg = Color(255, 7, 11, 21);
        kPanel = Color(255, 11, 18, 32);
        kCard = Color(255, 14, 23, 40);
        kCard2 = Color(255, 17, 29, 51);
        kLine = Color(255, 28, 42, 66);
        kLine2 = Color(255, 44, 62, 92);
        kText = Color(255, 230, 237, 247);
        kMuted = Color(255, 139, 155, 180);
        kDim = Color(255, 92, 107, 131);
        kCyan = Color(255, 56, 225, 255);
        kViolet = Color(255, 139, 123, 255);
        kGreen = Color(255, 52, 211, 153);
        kAmber = Color(255, 251, 191, 36);
        kRed = Color(255, 251, 113, 133);

        kTitleBg = Color(255, 9, 15, 27);
        kStripe = Color(255, 12, 20, 34);
        kInputBg = Color(255, 10, 16, 29);
        kInputBg2 = Color(255, 12, 19, 33);
        kSelectBg = Color(255, 22, 40, 64);
        kBrowseBg = Color(255, 18, 31, 54);
        kBrowseHot = Color(255, 26, 45, 72);
        kBtnBg = Color(255, 17, 29, 51);
        kBtnPrimary = Color(255, 20, 40, 62);
        kBtnPrimaryForm = Color(255, 23, 45, 66);
        kBtnPrimaryBorder = Color(255, 50, 160, 190);
        kBtnPrimaryBorderForm = Color(255, 56, 180, 210);
        kNavActiveBg = Color(255, 20, 36, 58);
        kNavIdleBg = Color(255, 16, 27, 46);
        kGlassBg = Color(255, 15, 25, 43);
        kInfoBg = Color(255, 17, 28, 46);
        kInfoBorder = Color(255, 52, 70, 100);
        kSysRowBg = Color(255, 20, 30, 46);
        kLineSoft = Color(255, 22, 32, 50);
        kTitleHoverBg = Color(255, 30, 44, 68);
        kCloseHoverBg = Color(255, 170, 45, 55);
        kTitleGlyph = Color(255, 200, 212, 230);
        kDomainText = Color(255, 219, 234, 254);
        kToggleOnBg = Color(255, 27, 74, 62);
        kToggleOffBg = Color(255, 30, 40, 58);
        kToggleKnobOff = Color(255, 150, 163, 184);
        kBadgeGoodBg = Color(255, 16, 44, 36);
        kBadgeWarnBg = Color(255, 48, 40, 16);
        kBadgeBadBg = Color(255, 48, 22, 28);
        kBadgeInfoBg = Color(255, 16, 40, 55);
        kBadgeNeutralBg = Color(255, 22, 30, 44);
        kBadgeProxyFg = Color(255, 200, 192, 255);
        kBadgeProxyBg = Color(255, 34, 29, 55);
        kBadgeDnsFg = Color(255, 155, 232, 255);
        kTakeoverOnBg = Color(255, 13, 34, 32);
        kTakeoverOnBorder = Color(255, 40, 120, 96);
        kTakeoverOffBg = Color(255, 33, 28, 16);
        kTakeoverOffBorder = Color(255, 90, 78, 30);
        kGlowA = Color(18, 56, 225, 255);
        kGlowB = Color(0, 7, 11, 21);
    }

    // 画刷按颜色创建，主题变化后必须重建，否则表单输入框仍是旧底色
    if (uiState.editBrush) DeleteObject(uiState.editBrush);
    uiState.editBrush = CreateSolidBrush(RGB(kInputBg2.GetR(), kInputBg2.GetG(), kInputBg2.GetB()));
    if (uiState.formBrush) DeleteObject(uiState.formBrush);
    uiState.formBrush = CreateSolidBrush(RGB(kBg.GetR(), kBg.GetG(), kBg.GetB()));

    uichart::SetTheme(light);
    procpick::SetTheme(light);
    setAppThemeMode(light);
    if (uiState.hwnd) {
        applyDwmTheme(uiState.hwnd);
        InvalidateRect(uiState.hwnd, nullptr, FALSE);
    }
    if (uiState.chart) InvalidateRect(uiState.chart, nullptr, FALSE);
}

// ---------------------------------------------------------------- 绘制基础

void roundRect(Graphics &g, RectF r, float rad, Color fill, Color border = Color(0, 0, 0, 0),
               float bw = 1.0f) {
    float d = rad * 2;
    GraphicsPath p;
    p.AddArc(r.X, r.Y, d, d, 180, 90);
    p.AddArc(r.X + r.Width - d, r.Y, d, d, 270, 90);
    p.AddArc(r.X + r.Width - d, r.Y + r.Height - d, d, d, 0, 90);
    p.AddArc(r.X, r.Y + r.Height - d, d, d, 90, 90);
    p.CloseFigure();
    if (fill.GetA()) {
        SolidBrush b(fill);
        g.FillPath(&b, &p);
    }
    if (border.GetA()) {
        Pen pen(border, bw);
        g.DrawPath(&pen, &p);
    }
}

void str(Graphics &g, const std::wstring &s, Font *f, Color c, RectF r,
         StringAlignment ha = StringAlignmentNear, bool ellipsis = true) {
    if (s.empty()) return;
    StringFormat fmt;
    fmt.SetAlignment(ha);
    fmt.SetLineAlignment(StringAlignmentCenter);
    if (ellipsis) fmt.SetTrimming(StringTrimmingEllipsisCharacter);
    SolidBrush b(c);
    g.DrawString(s.c_str(), (INT)s.size(), f, r, &fmt, &b);
}

void badge(Graphics &g, RECT rc, const std::wstring &text, Color fg, Color fill) {
    RectF r((REAL)rc.left, (REAL)rc.top, (REAL)(rc.right - rc.left), (REAL)(rc.bottom - rc.top));
    roundRect(g, r, (REAL)S(5), fill, fg);
    str(g, text, uiState.fTiny, fg, r, StringAlignmentCenter);
}

void switchBox(Graphics &g, RECT rc, bool on) {
    RectF r((REAL)rc.left, (REAL)rc.top, (REAL)(rc.right - rc.left), (REAL)(rc.bottom - rc.top));
    roundRect(g, r, r.Height / 2, on ? kToggleOnBg : kToggleOffBg,
              on ? Color(150, 52, 211, 153) : kLine2);
    float d = r.Height - S(4);
    RectF knob(on ? r.X + r.Width - d - S(2) : r.X + S(2), r.Y + S(2), d, d);
    SolidBrush b(on ? kGreen : kToggleKnobOff);
    g.FillEllipse(&b, knob);
}

void addAct(RECT r, int kind, int index = -1) { uiState.actions.push_back({r, kind, index}); }

void clearActions() { uiState.actions.clear(); }

std::wstring widestr(const std::string &s) { return util::w(s); }
std::string narrow(const std::wstring &s) { return util::u8(s); }

std::wstring trimW(const std::wstring &s) {
    size_t a = s.find_first_not_of(L" \t\r\n");
    if (a == std::wstring::npos) return L"";
    size_t b = s.find_last_not_of(L" \t\r\n");
    return s.substr(a, b - a + 1);
}

std::wstring joinServers(const std::vector<std::string> &servers) {
    std::wstring out;
    for (size_t i = 0; i < servers.size(); i++) {
        if (i) out += L", ";
        out += util::w(servers[i]);
    }
    return out;
}

std::string profileNameOf(const Config &c, const std::string &id) {
    for (auto &p : c.profiles)
        if (p.id == id) return p.name;
    return id;
}

std::string profileProviderOf(const Config &c, const std::string &id) {
    for (auto &p : c.profiles)
        if (p.id == id) return p.provider;
    return "";
}

bool isAutoProfile(const Config &c, const std::string &id) { return profileProviderOf(c, id) == "auto"; }

const Profile *findProfile(const Config &c, const std::string &id) {
    for (auto &p : c.profiles)
        if (p.id == id) return &p;
    return nullptr;
}

// 把“域名 / 网址”输入归一成主机名：去掉 scheme、userinfo、端口、路径、查询串和末尾点
std::wstring normalizeHostInput(const std::wstring &input) {
    std::wstring s = trimW(input);
    size_t scheme = s.find(L"://");
    if (scheme != std::wstring::npos) s = s.substr(scheme + 3);
    size_t cut = s.find_first_of(L"/?#");
    if (cut != std::wstring::npos) s = s.substr(0, cut);
    size_t at = s.rfind(L'@');
    if (at != std::wstring::npos) s = s.substr(at + 1);
    if (!s.empty() && s[0] == L'[') {
        size_t close = s.find(L']');
        if (close != std::wstring::npos) s = s.substr(1, close - 1);
    } else {
        size_t colon = s.rfind(L':');
        if (colon != std::wstring::npos && s.find(L':') == colon) s = s.substr(0, colon);
    }
    while (!s.empty() && s.back() == L'.') s.pop_back();
    return trimW(s);
}

struct BindingView {
    std::wstring rule;
    std::wstring profile;
    std::wstring servers;
    std::wstring strategy;
    bool fallback = true;
};

// 只读查询：用 ConfigStore 的匹配逻辑得到规则与配置，不写配置、不发查询
BindingView describeBinding(const std::string &domain, const std::string &exe) {
    BindingView v;
    ProfileChoice pc = uiState.ctx->cfg->chooseProfile(domain, exe);
    if (pc.ruleType == "domain")
        v.rule = L"域名规则 · " + util::w(pc.ruleMatch);
    else if (pc.ruleType == "app")
        v.rule = L"应用规则 · " + util::w(pc.ruleMatch);
    else
        v.rule = L"默认配置（未命中规则）";
    v.fallback = pc.ruleType == "none";
    const Profile *p = findProfile(uiState.cfg, pc.profileId);
    if (p) {
        v.profile = util::w(p->name) + (p->provider == "system" ? L" · 自动探测" : L"");
        v.servers = joinServers(p->servers);
        if (v.servers.empty() && p->provider == "system") v.servers = L"未探测到网卡 DNS";
        v.strategy = p->strategy == "only" ? L"仅使用第一项" : L"主用优先，失败后回退";
    } else {
        v.profile = util::w(pc.profileId);
        v.servers = L"—";
        v.strategy = L"—";
    }
    return v;
}

void toast(const std::wstring &text) {
    uiState.toast = text;
    uiState.toastUntil = util::nowMs() + 3500;
    InvalidateRect(uiState.hwnd, nullptr, FALSE);
}

void msgErr(HWND owner, const std::wstring &text) {
    MessageBoxW(owner, text.c_str(), L"DNS Pilot", MB_OK | MB_ICONERROR);
}

void cleanupAutoProfiles(Config &c) {
    auto used = [&](const std::string &id) {
        for (auto &r : c.appRules)
            if (r.profile == id) return true;
        for (auto &r : c.domainRules)
            if (r.profile == id) return true;
        return false;
    };
    c.profiles.erase(std::remove_if(c.profiles.begin(), c.profiles.end(),
                                    [&](const Profile &p) {
                                        return p.provider == "auto" && !used(p.id);
                                    }),
                     c.profiles.end());
}

std::wstring dohForProvider(const std::string &provider) {
    if (provider == "cloudflare") return L"https://cloudflare-dns.com/dns-query";
    if (provider == "aliyun") return L"https://dns.alidns.com/dns-query";
    if (provider == "google") return L"https://dns.google/dns-query";
    if (provider == "dnspod") return L"https://doh.pub/dns-query";
    if (provider == "quad9") return L"https://dns.quad9.net/dns-query";
    return L"";
}

bool saveRule(bool app, int index, const std::wstring &matchW, const std::string &profileId,
              const std::vector<std::string> &servers, const std::wstring &noteW, std::string &err) {
    std::string match = narrow(matchW);
    std::string note = narrow(noteW);
    std::string pid = profileId;
    return uiState.ctx->cfg->update(
        [&](Config &c) {
            if (!servers.empty()) {
                Profile p;
                p.id = "auto-" + util::randHex(4);
                p.name = (match.empty() ? std::string("规则") : match) + " · 专用";
                p.provider = "auto";
                p.strategy = "failover";
                p.note = "规则专用配置";
                p.servers = servers;
                c.profiles.push_back(p);
                pid = p.id;
            }
            auto &rules = app ? c.appRules : c.domainRules;
            bool enabled = true;
            if (index >= 0 && index < (int)rules.size()) {
                enabled = rules[index].enabled;
                rules[index].match = match;
                rules[index].profile = pid;
                rules[index].note = note;
                rules[index].enabled = enabled;
            } else {
                Rule r;
                r.match = match;
                r.profile = pid;
                r.note = note;
                r.enabled = true;
                rules.push_back(r);
            }
            cleanupAutoProfiles(c);
            return true;
        },
        err);
}

// ---------------------------------------------------------------- 通用表单对话框

enum FormType { FT_TEXT, FT_NUMBER, FT_BROWSE, FT_TOGGLE, FT_LIST };

struct FormItem {
    FormType type = FT_TEXT;
    std::wstring label;
    std::wstring value;
    bool on = false;
    std::vector<std::wstring> options;
    int selected = 0;
};

struct FormState {
    HWND hwnd = nullptr;
    HWND owner = nullptr;
    std::wstring title;
    std::vector<FormItem> items;
    std::vector<HWND> edits;
    std::vector<RECT> labelRects;
    std::vector<RECT> fieldRects;
    std::vector<RECT> toggleRects;
    std::vector<RECT> browseRects;
    std::vector<RECT> listRects;
    std::function<void(FormItem &, HWND)> onBrowse;
    std::function<void(std::vector<FormItem> &)> onChange;
    std::function<bool(std::vector<FormItem> &, std::wstring &)> validate;
    bool ok = false;
    int hot = -1;
    int width = 720;
    int height = 0;
    int scrollV = 0;
    int scrollMax = 0;
    int contentH = 0;
    RECT btnCancel{}, btnSave{};
};

int formItemHeight(const FormItem &it) {
    switch (it.type) {
        case FT_TOGGLE: return S(42);
        case FT_LIST: return S(24) + (int)std::min<size_t>(it.options.size(), 8) * S(26);
        default: return S(64);
    }
}

void syncEditsFromItems(FormState &st) {
    for (size_t i = 0; i < st.items.size() && i < st.edits.size(); i++) {
        if (st.edits[i]) SetWindowTextW(st.edits[i], st.items[i].value.c_str());
    }
}

void syncItemsFromEdits(FormState &st) {
    for (size_t i = 0; i < st.items.size() && i < st.edits.size(); i++) {
        if (!st.edits[i]) continue;
        LRESULT n = SendMessageW(st.edits[i], WM_GETTEXTLENGTH, 0, 0);
        std::wstring text((size_t)n + 1, L'\0');
        SendMessageW(st.edits[i], WM_GETTEXT, (WPARAM)(n + 1), (LPARAM)text.data());
        text.resize((size_t)n);
        st.items[i].value = text;
    }
}

void layoutForm(FormState &st) {
    RECT rc;
    GetClientRect(st.hwnd, &rc);
    int w = rc.right - rc.left;
    int x = S(28);
    int y = S(72) - st.scrollV;
    int cw = w - S(56);
    st.labelRects.assign(st.items.size(), RECT{});
    st.fieldRects.assign(st.items.size(), RECT{});
    st.toggleRects.assign(st.items.size(), RECT{});
    st.browseRects.assign(st.items.size(), RECT{});
    st.listRects.assign(st.items.size(), RECT{});
    int baseY = y;
    for (size_t i = 0; i < st.items.size(); i++) {
        auto &it = st.items[i];
        int h = formItemHeight(it);
        if (it.type == FT_TOGGLE) {
            st.labelRects[i] = {x, y, x + cw - S(60), y + S(24)};
            st.toggleRects[i] = {x + cw - S(46), y + S(2), x + cw, y + S(24)};
        } else if (it.type == FT_LIST) {
            st.labelRects[i] = {x, y, x + cw, y + S(20)};
            st.listRects[i] = {x, y + S(22), x + cw, y + h};
        } else if (it.type == FT_BROWSE) {
            st.labelRects[i] = {x, y, x + cw, y + S(20)};
            st.fieldRects[i] = {x, y + S(22), x + cw - S(104), y + S(52)};
            st.browseRects[i] = {x + cw - S(96), y + S(22), x + cw, y + S(52)};
        } else {
            st.labelRects[i] = {x, y, x + cw, y + S(20)};
            st.fieldRects[i] = {x, y + S(22), x + cw, y + S(52)};
        }
        y += h + S(8);
    }
    st.contentH = (y + st.scrollV) + S(10);
    int listBottom = rc.bottom - S(74);
    int avail = listBottom - S(66);
    st.scrollMax = std::max(0, st.contentH - S(66) - avail);
    if (st.scrollV > st.scrollMax) st.scrollV = st.scrollMax;
    if (st.scrollV < 0) st.scrollV = 0;
    st.btnCancel = {w - S(196), rc.bottom - S(56), w - S(108), rc.bottom - S(18)};
    st.btnSave = {w - S(100), rc.bottom - S(56), w - S(28), rc.bottom - S(18)};
    (void)baseY;
}

void moveEdits(FormState &st) {
    for (size_t i = 0; i < st.items.size() && i < st.edits.size(); i++) {
        if (!st.edits[i]) continue;
        RECT r = st.fieldRects[i];
        SetWindowPos(st.edits[i], nullptr, r.left, r.top, r.right - r.left, r.bottom - r.top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
    }
}

LRESULT CALLBACK FormProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto *st = (FormState *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    switch (msg) {
        case WM_NCCREATE: {
            auto *cs = (CREATESTRUCTW *)lp;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
            return TRUE;
        }
        case WM_CREATE: {
            auto *s = (FormState *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
            s->hwnd = hwnd;
            s->edits.assign(s->items.size(), nullptr);
            RECT crc;
            GetClientRect(hwnd, &crc);
            s->height = crc.bottom;
            layoutForm(*s);
            for (size_t i = 0; i < s->items.size(); i++) {
                auto &it = s->items[i];
                if (it.type == FT_TEXT || it.type == FT_NUMBER) {
                    RECT r = s->fieldRects[i];
                    HWND e = CreateWindowExW(0, L"EDIT", it.value.c_str(),
                                             WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                                             r.left, r.top, r.right - r.left, r.bottom - r.top, hwnd,
                                             (HMENU)(INT_PTR)(1000 + i), GetModuleHandleW(nullptr), nullptr);
                    SendMessageW(e, WM_SETFONT, (WPARAM)uiState.editFont, TRUE);
                    s->edits[i] = e;
                }
            }
            SetFocus(s->edits.empty() ? hwnd : s->edits[0]);
            return 0;
        }
        case WM_CTLCOLOREDIT: {
            HDC dc = (HDC)wp;
            SetTextColor(dc, RGB(kText.GetR(), kText.GetG(), kText.GetB()));
            SetBkColor(dc, RGB(kInputBg2.GetR(), kInputBg2.GetG(), kInputBg2.GetB()));
            return (LRESULT)uiState.editBrush;
        }
        case WM_ERASEBKGND: {
            // 类背景画刷为空，这里用主题色擦除，避免浅色主题下先闪一帧黑色
            RECT erc;
            GetClientRect(hwnd, &erc);
            FillRect((HDC)wp, &erc, uiState.formBrush);
            return 1;
        }
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            RECT rc;
            GetClientRect(hwnd, &rc);
            Bitmap bmp(rc.right, rc.bottom);
            Graphics gr(&bmp);
            gr.SetSmoothingMode(SmoothingModeAntiAlias);
            gr.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);
            SolidBrush bg(kBg);
            gr.FillRectangle(&bg, 0, 0, (INT)rc.right, (INT)rc.bottom);
            str(gr, st->title, uiState.fH2, kText, RectF((REAL)S(28), (REAL)S(24), (REAL)(rc.right - S(56)), (REAL)S(32)));
            // 裁剪高度必须减去起始偏移，否则内容会画到底部按钮下面
            gr.SetClip(RectF(0, (REAL)S(64), (REAL)rc.right, (REAL)(rc.bottom - S(64) - S(66))));
            for (size_t i = 0; i < st->items.size(); i++) {
                auto &it = st->items[i];
                RECT lr = st->labelRects[i];
                str(gr, it.label, uiState.fSmall, kMuted,
                    RectF((REAL)lr.left, (REAL)lr.top, (REAL)(lr.right - lr.left), (REAL)(lr.bottom - lr.top)));
                if (it.type == FT_TOGGLE) {
                    switchBox(gr, st->toggleRects[i], it.on);
                } else if (it.type == FT_LIST) {
                    RECT lr2 = st->listRects[i];
                    roundRect(gr, RectF((REAL)lr2.left, (REAL)lr2.top, (REAL)(lr2.right - lr2.left),
                                        (REAL)(lr2.bottom - lr2.top)),
                              (REAL)S(8), kInputBg, kLine2);
                    int rowH = S(26);
                    for (size_t k = 0; k < it.options.size() && k < 8; k++) {
                        RECT rr{lr2.left + S(1), lr2.top + S(1) + (int)k * rowH, lr2.right - S(1),
                                lr2.top + S(1) + (int)(k + 1) * rowH};
                        bool sel = (int)k == it.selected;
                        if (sel) {
                            SolidBrush sb(kSelectBg);
                            gr.FillRectangle(&sb, (INT)rr.left, (INT)rr.top, (INT)(rr.right - rr.left), (INT)(rr.bottom - rr.top));
                            SolidBrush bar(kCyan);
                            gr.FillRectangle(&bar, (INT)rr.left, (INT)(rr.top + S(4)), S(2), rowH - S(8));
                        }
                        str(gr, it.options[k], uiState.fBody, sel ? kText : kMuted,
                            RectF((REAL)(rr.left + S(10)), (REAL)rr.top, (REAL)(rr.right - rr.left - S(14)),
                                  (REAL)(rr.bottom - rr.top)));
                    }
                } else if (it.type == FT_BROWSE) {
                    RECT fr = st->fieldRects[i];
                    roundRect(gr, RectF((REAL)fr.left, (REAL)fr.top, (REAL)(fr.right - fr.left),
                                        (REAL)(fr.bottom - fr.top)),
                              (REAL)S(8), kInputBg, kLine2);
                    str(gr, it.value.empty() ? L"（尚未选择）" : it.value, uiState.fMono,
                        it.value.empty() ? kDim : kText,
                        RectF((REAL)(fr.left + S(10)), (REAL)fr.top, (REAL)(fr.right - fr.left - S(16)),
                              (REAL)(fr.bottom - fr.top)));
                    RECT br = st->browseRects[i];
                    bool hot = (st->hot == (int)i);
                    roundRect(gr, RectF((REAL)br.left, (REAL)br.top, (REAL)(br.right - br.left),
                                        (REAL)(br.bottom - br.top)),
                              (REAL)S(8), hot ? kBrowseHot : kBrowseBg, kLine2);
                    str(gr, L"选择…", uiState.fSmall, kCyan,
                        RectF((REAL)br.left, (REAL)br.top, (REAL)(br.right - br.left), (REAL)(br.bottom - br.top)),
                        StringAlignmentCenter);
                } else {
                    RECT fr = st->fieldRects[i];
                    roundRect(gr, RectF((REAL)fr.left, (REAL)fr.top, (REAL)(fr.right - fr.left),
                                        (REAL)(fr.bottom - fr.top)),
                              (REAL)S(8), kInputBg, kLine2);
                }
            }
            // 按钮
            gr.ResetClip();
            auto drawBtn = [&](RECT r, const wchar_t *text, bool primary) {
                roundRect(gr, RectF((REAL)r.left, (REAL)r.top, (REAL)(r.right - r.left), (REAL)(r.bottom - r.top)),
                          (REAL)S(7), primary ? kBtnPrimaryForm : kBtnBg,
                          primary ? kBtnPrimaryBorderForm : kLine2);
                str(gr, text, uiState.fBody, primary ? kCyan : kText,
                    RectF((REAL)r.left, (REAL)r.top, (REAL)(r.right - r.left), (REAL)(r.bottom - r.top)),
                    StringAlignmentCenter);
            };
            drawBtn(st->btnCancel, L"取消", false);
            drawBtn(st->btnSave, L"保存", true);
            Graphics screen(hdc);
            screen.DrawImage(&bmp, 0, 0);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_MOUSEMOVE: {
            int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
            int hot = -1;
            for (size_t i = 0; i < st->items.size(); i++)
                if (st->items[i].type == FT_BROWSE && PtInRect(&st->browseRects[i], POINT{x, y})) hot = (int)i;
            if (hot != st->hot) {
                st->hot = hot;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }
        case WM_SETCURSOR:
            if (LOWORD(lp) == HTCLIENT) {
                POINT p;
                GetCursorPos(&p);
                ScreenToClient(hwnd, &p);
                for (auto &br : st->browseRects)
                    if (PtInRect(&br, p)) {
                        SetCursor(LoadCursorW(nullptr, MAKEINTRESOURCEW(32512)));
                        return TRUE;
                    }
            }
            break;
        case WM_LBUTTONDOWN: {
            int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
            POINT p{x, y};
            if (PtInRect(&st->btnCancel, p)) {
                DestroyWindow(hwnd);
                return 0;
            }
            if (PtInRect(&st->btnSave, p)) {
                syncItemsFromEdits(*st);
                std::wstring err;
                if (st->validate && !st->validate(st->items, err)) {
                    MessageBoxW(hwnd, err.c_str(), L"无法保存", MB_OK | MB_ICONWARNING);
                    return 0;
                }
                st->ok = true;
                DestroyWindow(hwnd);
                return 0;
            }
            for (size_t i = 0; i < st->items.size(); i++) {
                auto &it = st->items[i];
                if (it.type == FT_TOGGLE && PtInRect(&st->toggleRects[i], p)) {
                    it.on = !it.on;
                    if (st->onChange) {
                        st->onChange(st->items);
                        syncEditsFromItems(*st);
                    }
                    InvalidateRect(hwnd, nullptr, FALSE);
                    return 0;
                }
                if (it.type == FT_BROWSE && PtInRect(&st->browseRects[i], p)) {
                    if (st->onBrowse) {
                        st->onBrowse(it, hwnd);
                        InvalidateRect(hwnd, nullptr, FALSE);
                    }
                    return 0;
                }
                if (it.type == FT_LIST) {
                    RECT lr = st->listRects[i];
                    if (PtInRect(&lr, p)) {
                        int rowH = S(26);
                        int idx = (y - lr.top - S(1)) / rowH;
                        if (idx >= 0 && idx < (int)it.options.size() && idx < 8) {
                            it.selected = idx;
                            if (st->onChange) {
                                st->onChange(st->items);
                                syncEditsFromItems(*st);
                            }
                            InvalidateRect(hwnd, nullptr, FALSE);
                        }
                        return 0;
                    }
                }
            }
            return 0;
        }
        case WM_MOUSEWHEEL: {
            int delta = GET_WHEEL_DELTA_WPARAM(wp);
            int step = delta > 0 ? -S(40) : S(40);
            st->scrollV += step;
            if (st->scrollV < 0) st->scrollV = 0;
            if (st->scrollV > st->scrollMax) st->scrollV = st->scrollMax;
            layoutForm(*st);
            moveEdits(*st);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case WM_KEYDOWN:
            if (wp == VK_ESCAPE) {
                DestroyWindow(hwnd);
                return 0;
            }
            break;
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

bool ShowForm(HWND owner, const std::wstring &title, std::vector<FormItem> items,
              std::function<void(FormItem &, HWND)> onBrowse,
              std::function<void(std::vector<FormItem> &)> onChange,
              std::function<bool(std::vector<FormItem> &, std::wstring &)> validate,
              std::vector<FormItem> &out) {
    static bool clsDone = false;
    if (!clsDone) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = FormProc;
        wc.hInstance = uiState.inst;
        wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
        wc.hIcon = util::appIcon();
        wc.hIconSm = wc.hIcon;
        wc.hbrBackground = nullptr;  // 背景由 WM_ERASEBKGND 按主题填充
        wc.lpszClassName = L"DNSPilotForm";
        RegisterClassExW(&wc);
        clsDone = true;
    }
    FormState st;
    st.owner = owner;
    st.title = title;
    st.items = std::move(items);
    st.onBrowse = std::move(onBrowse);
    st.onChange = std::move(onChange);
    st.validate = std::move(validate);
    int h = S(80) + S(10);
    for (auto &it : st.items) h += formItemHeight(it) + S(10);
    h += S(74);
    if (h > S(900)) h = S(900);
    st.height = h;
    RECT orc;
    GetWindowRect(owner, &orc);
    int w = S(st.width);
    int x = orc.left + ((orc.right - orc.left) - w) / 2;
    int y = orc.top + ((orc.bottom - orc.top) - h) / 2;
    if (y < 20) y = 20;
    HWND dlg = CreateWindowExW(0, L"DNSPilotForm", title.c_str(),
                               WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN, x, y, w, h, owner,
                               nullptr, uiState.inst, &st);
    if (!dlg) return false;
    SetWindowTextW(dlg, st.title.c_str());
    SendMessageW(dlg, WM_SETICON, ICON_SMALL,
                 (LPARAM)util::appIcon());
    SendMessageW(dlg, WM_SETICON, ICON_BIG,
                 (LPARAM)util::appIcon());
    // 系统标题栏按当前主题着色（浅色为白底深字）
    applyDwmTheme(dlg);
    ShowWindow(dlg, SW_SHOW);
    UpdateWindow(dlg);
    EnableWindow(owner, FALSE);
    SetForegroundWindow(dlg);
    MSG msg;
    while (IsWindow(dlg) && GetMessageW(&msg, nullptr, 0, 0)) {
        if (!IsDialogMessageW(dlg, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    EnableWindow(owner, TRUE);
    SetForegroundWindow(owner);
    out = st.items;
    return st.ok;
}

// ---------------------------------------------------------------- 业务操作

bool parseServers(const std::wstring &text, std::vector<std::string> &out, std::wstring &err) {
    out.clear();
    std::string s = narrow(text);
    auto parts = util::splitAny(s, ",; \t");
    for (auto &p : parts) {
        std::string host;
        uint16_t port = 0;
        if (!util::parseServer(p, host, port)) {
            err = L"DNS 服务器地址无效：" + util::w(p);
            return false;
        }
        out.push_back(p);
    }
    if (out.empty()) {
        err = L"至少需要一个 DNS 服务器";
        return false;
    }
    return true;
}

void refreshData() {
    uiState.cfg = uiState.ctx->cfg->snapshot();
    uiState.stats = uiState.ctx->stats->view(60);
}

// 标题栏主题按钮：切换后立即生效并写入配置（ConfigStore 是唯一写入点）
void toggleTheme() {
    bool light = !gLightTheme;
    std::string err;
    if (!uiState.ctx->cfg->update(
            [&](Config &c) {
                c.ui.theme = light ? "light" : "dark";
                return true;
            },
            err)) {
        msgErr(uiState.hwnd, util::w(err));
        return;
    }
    applyTheme(light);
    refreshData();
    toast(light ? L"已切换到浅色主题" : L"已切换到深色主题");
}

void openRuleDialog(bool app, int index) {
    Config cfg = uiState.ctx->cfg->snapshot();
    std::wstring match, note, servers;
    int selected = 0;
    bool custom = false;
    if (index >= 0 && index < (int)(app ? cfg.appRules.size() : cfg.domainRules.size())) {
        const Rule &r = app ? cfg.appRules[index] : cfg.domainRules[index];
        match = util::w(r.match);
        note = util::w(r.note);
        if (isAutoProfile(cfg, r.profile)) {
            custom = true;
            for (auto &p : cfg.profiles)
                if (p.id == r.profile) servers = joinServers(p.servers);
        } else {
            for (size_t i = 0; i < cfg.profiles.size(); i++)
                if (cfg.profiles[i].id == r.profile) selected = (int)i;
        }
    }
    std::vector<FormItem> items;
    FormItem f1;
    f1.type = app ? FT_BROWSE : FT_TEXT;
    f1.label = app ? L"进程（exe 文件名）"
                   : L"域名（可直接粘贴网址，自动取主机名；匹配该域名及其所有子域）";
    f1.value = match;
    items.push_back(f1);

    FormItem f2;
    f2.type = FT_LIST;
    f2.label = L"DNS 配置";
    for (auto &p : cfg.profiles) f2.options.push_back(util::w(p.name));
    f2.selected = selected;
    items.push_back(f2);

    FormItem f3;
    f3.type = FT_TOGGLE;
    f3.label = L"使用自定义 DNS 服务器（为该规则单独绑定）";
    f3.on = custom;
    items.push_back(f3);

    FormItem f4;
    f4.type = FT_TEXT;
    f4.label = L"自定义服务器（逗号分隔，可带端口）";
    f4.value = servers;
    items.push_back(f4);

    FormItem f5;
    f5.type = FT_TEXT;
    f5.label = L"备注";
    f5.value = note;
    items.push_back(f5);

    std::vector<FormItem> out;
    bool ok = ShowForm(
        uiState.hwnd, app ? (index >= 0 ? L"编辑应用规则" : L"添加应用规则")
                    : (index >= 0 ? L"编辑域名规则" : L"添加域名规则"),
        items,
        [](FormItem &it, HWND dlg) {
            procpick::Picked p;
            if (procpick::Show(dlg, p) && !p.exeName.empty()) it.value = p.exeName;
        },
        nullptr,
        [cfg, app](std::vector<FormItem> &v, std::wstring &err) {
            if (!app) {
                // 域名规则允许直接粘贴网址，统一归一化成主机名后再保存
                std::wstring host = util::w(util::normalizeDomainRule(narrow(v[0].value)));
                if (host.empty()) {
                    err = L"域名格式无效，请填写 netflix.com 这类域名，或直接粘贴网址";
                    return false;
                }
                v[0].value = host;
            } else if (util::trim(narrow(v[0].value)).empty()) {
                err = L"请填写进程名或域名";
                return false;
            }
            if (v[2].on) {
                std::vector<std::string> servers;
                if (!parseServers(v[3].value, servers, err)) return false;
            } else {
                int i = v[1].selected;
                if (i < 0 || i >= (int)cfg.profiles.size()) {
                    err = L"请选择一个 DNS 配置";
                    return false;
                }
            }
            return true;
        },
        out);
    if (!ok) return;

    // 域名规则再归一化一次，避免绕过表单校验的调用写入 URL 形式
    std::wstring matchW = out[0].value;
    if (!app) matchW = util::w(util::normalizeDomainRule(narrow(matchW)));
    std::string matchA = util::trim(narrow(matchW));
    if (matchA.empty()) return;
    std::vector<std::string> customServers;
    std::string profileId;
    if (out[2].on) {
        std::wstring err;
        if (!parseServers(out[3].value, customServers, err)) {
            msgErr(uiState.hwnd, err);
            return;
        }
    } else {
        int sel = out[1].selected;
        if (sel >= 0 && sel < (int)cfg.profiles.size()) profileId = cfg.profiles[sel].id;
    }
    std::string err;
    if (!saveRule(app, index, matchW, profileId, customServers, out[4].value, err)) {
        msgErr(uiState.hwnd, util::w(err));
        return;
    }
    refreshData();
    toast(app ? L"应用规则已保存" : L"域名规则已保存");
    InvalidateRect(uiState.hwnd, nullptr, FALSE);
}

void openProfileDialog(int index) {
    Config cfg = uiState.ctx->cfg->snapshot();
    std::vector<FormItem> items;
    FormItem f1;
    f1.type = FT_TEXT;
    f1.label = L"配置名称";
    FormItem f2;
    f2.type = FT_TEXT;
    f2.label = L"DNS 服务器（逗号分隔，可带端口）";
    FormItem f3;
    f3.type = FT_LIST;
    f3.label = L"策略";
    f3.options = {L"主用优先，失败后回退", L"仅使用第一项"};
    FormItem f4;
    f4.type = FT_TEXT;
    f4.label = L"备注";
    std::string id;
    if (index >= 0 && index < (int)cfg.profiles.size()) {
        const Profile &p = cfg.profiles[index];
        if (p.id == "system") return;
        id = p.id;
        f1.value = util::w(p.name);
        f2.value = joinServers(p.servers);
        f3.selected = p.strategy == "only" ? 1 : 0;
        f4.value = util::w(p.note);
    }
    items = {f1, f2, f3, f4};
    std::vector<FormItem> out;
    bool ok = ShowForm(
        uiState.hwnd, index >= 0 ? L"编辑 DNS 配置" : L"添加 DNS 配置", items, nullptr, nullptr,
        [](std::vector<FormItem> &v, std::wstring &err) {
            if (util::trim(narrow(v[0].value)).empty()) {
                err = L"配置名称不能为空";
                return false;
            }
            std::vector<std::string> servers;
            return parseServers(v[1].value, servers, err);
        },
        out);
    if (!ok) return;
    std::vector<std::string> servers;
    std::wstring werr;
    if (!parseServers(out[1].value, servers, werr)) {
        msgErr(uiState.hwnd, werr);
        return;
    }
    std::string name = util::trim(narrow(out[0].value));
    std::string note = narrow(out[3].value);
    std::string strategy = out[2].selected == 1 ? "only" : "failover";
    std::string err;
    bool saved = uiState.ctx->cfg->update(
        [&](Config &c) {
            if (!id.empty()) {
                for (auto &p : c.profiles) {
                    if (p.id == id) {
                        p.name = name;
                        p.servers = servers;
                        p.strategy = strategy;
                        p.note = note;
                        return true;
                    }
                }
                return false;
            }
            Profile p;
            p.id = "custom-" + util::randHex(3);
            p.name = name;
            p.provider = "custom";
            p.strategy = strategy;
            p.note = note;
            p.servers = servers;
            c.profiles.push_back(p);
            return true;
        },
        err);
    if (!saved) {
        msgErr(uiState.hwnd, util::w(err));
        return;
    }
    refreshData();
    toast(L"DNS 配置已保存");
    InvalidateRect(uiState.hwnd, nullptr, FALSE);
}

void openSettingsDialog() {
    Config cfg = uiState.ctx->cfg->snapshot();
    std::vector<FormItem> items;
    FormItem f1;
    f1.type = FT_LIST;
    f1.label = L"默认 DNS 配置（未命中规则时使用）";
    for (auto &p : cfg.profiles) f1.options.push_back(util::w(p.name));
    for (size_t i = 0; i < cfg.profiles.size(); i++)
        if (cfg.profiles[i].id == cfg.defaultProfile) f1.selected = (int)i;
    FormItem f2;
    f2.type = FT_TOGGLE;
    f2.label = L"启用 DNS 缓存";
    f2.on = cfg.resolver.cacheEnabled;
    FormItem f3;
    f3.type = FT_NUMBER;
    f3.label = L"最小 TTL（秒）";
    f3.value = std::to_wstring(cfg.resolver.minTtl);
    FormItem f4;
    f4.type = FT_NUMBER;
    f4.label = L"最大 TTL（秒）";
    f4.value = std::to_wstring(cfg.resolver.maxTtl);
    FormItem f5;
    f5.type = FT_NUMBER;
    f5.label = L"上游超时（毫秒）";
    f5.value = std::to_wstring(cfg.resolver.queryTimeoutMs);
    FormItem f6;
    f6.type = FT_TOGGLE;
    f6.label = L"启用应用代理（SOCKS5 / HTTP CONNECT）";
    f6.on = cfg.proxy.enabled;
    FormItem f7;
    f7.type = FT_NUMBER;
    f7.label = L"代理端口（重启生效）";
    f7.value = std::to_wstring(cfg.proxy.port);
    FormItem f8;
    f8.type = FT_NUMBER;
    f8.label = L"解析端口（重启生效，系统接管需要 53）";
    f8.value = std::to_wstring(cfg.resolver.port);
    FormItem f9;
    f9.type = FT_NUMBER;
    f9.label = L"面板端口（重启生效）";
    f9.value = std::to_wstring(cfg.resolver.httpPort);
    FormItem f10;
    f10.type = FT_TOGGLE;
    f10.label = L"退出时自动恢复系统 DNS";
    f10.on = cfg.ui.restoreOnExit;
    FormItem f11;
    f11.type = FT_TOGGLE;
    f11.label = L"启动时打开桌面窗口（重启生效）";
    f11.on = cfg.ui.nativeWindow;
    FormItem f12;
    f12.type = FT_LIST;
    f12.label = L"界面主题（立即生效）";
    f12.options = {L"浅色（白底）", L"深色"};
    f12.selected = cfg.ui.theme == "dark" ? 1 : 0;
    items = {f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12};
    std::vector<FormItem> out;
    bool ok = ShowForm(
        uiState.hwnd, L"设置", items, nullptr, nullptr,
        [](std::vector<FormItem> &v, std::wstring &err) {
            auto num = [&](int i, int lo, int hi, const wchar_t *name, int &dst) {
                wchar_t *end = nullptr;
                long x = wcstol(v[i].value.c_str(), &end, 10);
                if (end == v[i].value.c_str() || x < lo || x > hi) {
                    err = std::wstring(name) + L" 超出范围";
                    return false;
                }
                dst = (int)x;
                return true;
            };
            int a, b, c, d, e, f;
            if (!num(2, 1, 86400, L"最小 TTL", a)) return false;
            if (!num(3, 1, 86400, L"最大 TTL", b)) return false;
            if (b < a) {
                err = L"最大 TTL 不能小于最小 TTL";
                return false;
            }
            if (!num(4, 200, 10000, L"上游超时", c)) return false;
            if (!num(6, 1, 65535, L"代理端口", d)) return false;
            if (!num(7, 1, 65535, L"解析端口", e)) return false;
            if (!num(8, 1, 65535, L"面板端口", f)) return false;
            return true;
        },
        out);
    if (!ok) return;

    bool proxyWas = cfg.proxy.enabled;
    bool proxyNow = out[5].on;
    int newProxyPort = _wtoi(out[6].value.c_str());
    int newPort = _wtoi(out[7].value.c_str());
    int newHttp = _wtoi(out[8].value.c_str());
    int newMin = _wtoi(out[2].value.c_str());
    int newMax = _wtoi(out[3].value.c_str());
    int newTimeout = _wtoi(out[4].value.c_str());
    // 主题只需要 light/dark 两个值，索引 0 为浅色、1 为深色
    std::string newTheme = out[11].selected == 1 ? "dark" : "light";
    bool restart = newProxyPort != cfg.proxy.port || newPort != cfg.resolver.port ||
                   newHttp != cfg.resolver.httpPort;
    std::string err;
    bool saved = uiState.ctx->cfg->update(
        [&](Config &c) {
            int sel = out[0].selected;
            if (sel >= 0 && sel < (int)c.profiles.size()) c.defaultProfile = c.profiles[sel].id;
            c.resolver.cacheEnabled = out[1].on;
            c.resolver.minTtl = newMin;
            c.resolver.maxTtl = newMax;
            c.resolver.queryTimeoutMs = newTimeout;
            c.resolver.port = newPort;
            c.resolver.httpPort = newHttp;
            c.proxy.port = newProxyPort;
            c.proxy.enabled = proxyNow;
            c.ui.restoreOnExit = out[9].on;
            c.ui.nativeWindow = out[10].on;
            c.ui.theme = newTheme;
            return true;
        },
        err);
    if (!saved) {
        msgErr(uiState.hwnd, util::w(err));
        return;
    }
    uiState.ctx->engine->applyRuntimeSettings();
    // 主题与其它设置不同：保存后立即切换，不需要重启
    if (newTheme != cfg.ui.theme) applyTheme(newTheme != "dark");
    if (proxyWas != proxyNow) {
        std::string perr;
        if (proxyNow) {
            if (!uiState.ctx->proxy->start(perr)) {
                uiState.ctx->cfg->update([&](Config &c) { c.proxy.enabled = false; return true; }, perr);
                msgErr(uiState.hwnd, L"代理启动失败：" + util::w(perr));
            }
        } else {
            uiState.ctx->proxy->stop();
        }
    }
    refreshData();
    toast(restart ? L"设置已保存，端口修改重启后生效" : L"设置已保存");
    InvalidateRect(uiState.hwnd, nullptr, FALSE);
}

void openBrowserDialog(int index) {
    if (index < 0 || index >= (int)uiState.browsers.size()) return;
    const BrowserProfileInfo b = uiState.browsers[index];
    if (b.running) {
        msgErr(uiState.hwnd, L"该浏览器正在运行。请完全退出浏览器后再绑定，否则设置会被覆盖。");
        return;
    }
    Config cfg = uiState.ctx->cfg->snapshot();
    std::vector<FormItem> items;
    FormItem f1;
    f1.type = FT_LIST;
    f1.label = L"选择 DNS 配置（选择“系统默认”表示关闭浏览器安全 DNS）";
    for (auto &p : cfg.profiles) f1.options.push_back(util::w(p.name));
    FormItem f2;
    f2.type = FT_TEXT;
    f2.label = L"DoH 地址（浏览器只支持 DoH，选择内置配置会自动填入）";
    items = {f1, f2};
    // 预先选中能映射到当前 DoH 的配置
    for (size_t i = 0; i < cfg.profiles.size(); i++) {
        std::wstring u = dohForProvider(cfg.profiles[i].provider);
        if (!b.dohUrl.empty() && u == util::w(b.dohUrl)) f1.selected = (int)i;
        if (cfg.profiles[i].provider == "system" && b.mode == "off") f1.selected = (int)i;
    }
    if (f1.selected >= 0 && f1.selected < (int)cfg.profiles.size())
        f2.value = dohForProvider(cfg.profiles[f1.selected].provider);
    else
        f2.value = util::w(b.dohUrl);

    std::vector<FormItem> out;
    bool ok = ShowForm(
        uiState.hwnd, L"绑定浏览器 DNS · " + util::w(b.browserName) + L" / " + util::w(b.name), items,
        nullptr,
        [&cfg](std::vector<FormItem> &v) {
            int sel = v[0].selected;
            if (sel < 0 || sel >= (int)cfg.profiles.size()) return;
            v[1].value = dohForProvider(cfg.profiles[sel].provider);
        },
        [&cfg](std::vector<FormItem> &v, std::wstring &err) {
            int sel = v[0].selected;
            if (sel < 0 || sel >= (int)cfg.profiles.size()) {
                err = L"请选择一个 DNS 配置";
                return false;
            }
            std::string provider = cfg.profiles[sel].provider;
            if (provider == "system") return true;
            std::wstring url = trimW(v[1].value);
            if (url.rfind(L"https://", 0) != 0) {
                err = L"DoH 地址必须以 https:// 开头";
                return false;
            }
            return true;
        },
        out);
    if (!ok) return;
    int sel = out[0].selected;
    if (sel < 0 || sel >= (int)cfg.profiles.size()) return;
    bool off = cfg.profiles[sel].provider == "system";
    std::string url = off ? std::string() : narrow(trimW(out[1].value));
    std::string msg, backup;
    if (!browsers::apply(b.browser, b.folder, url, off, msg, backup)) {
        msgErr(uiState.hwnd, util::w(msg));
        return;
    }
    toast(off ? L"已恢复浏览器默认 DNS" : L"浏览器安全 DNS 已写入配置");
    uiState.browsers = browsers::scan();
    InvalidateRect(uiState.hwnd, nullptr, FALSE);
}

// 绑定查询：输入域名或网址，只记录到界面状态，不修改配置
void openDomainLookupDialog() {    std::vector<FormItem> items;
    FormItem f1;
    f1.type = FT_TEXT;
    f1.label = L"域名或网址（例如 netflix.com、https://www.netflix.com/title/1）";
    f1.value = uiState.lookupDomain;
    items = {f1};
    std::vector<FormItem> out;
    bool ok = ShowForm(
        uiState.hwnd, L"查看域名绑定", items, nullptr, nullptr,
        [](std::vector<FormItem> &v, std::wstring &err) {
            std::wstring host = normalizeHostInput(v[0].value);
            if (host.empty()) {
                err = L"请输入域名，例如 netflix.com";
                return false;
            }
            v[0].value = host;
            return true;
        },
        out);
    if (!ok) return;
    uiState.lookupDomain = normalizeHostInput(out[0].value);
    InvalidateRect(uiState.hwnd, nullptr, FALSE);
}

// ---------------------------------------------------------------- 配置导出 / 导入

std::wstring pickSaveFile(HWND owner, const std::wstring &suggested,
                          const wchar_t *title = L"导出 DNS 配置与绑定") {
    wchar_t buf[MAX_PATH] = {};
    lstrcpynW(buf, suggested.c_str(), MAX_PATH);
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner;
    ofn.lpstrFilter = L"DNS Pilot 配置 (*.json)\0*.json\0所有文件 (*.*)\0*.*\0";
    ofn.lpstrFile = buf;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrDefExt = L"json";
    ofn.lpstrTitle = title;
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetSaveFileNameW(&ofn)) return L"";
    return buf;
}

std::wstring pickOpenFile(HWND owner) {
    wchar_t buf[MAX_PATH] = {};
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner;
    ofn.lpstrFilter = L"DNS Pilot 配置 (*.json)\0*.json\0所有文件 (*.*)\0*.*\0";
    ofn.lpstrFile = buf;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrDefExt = L"json";
    ofn.lpstrTitle = L"导入 DNS 配置与绑定";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&ofn)) return L"";
    return buf;
}

void exportConfigFile() {
    SYSTEMTIME st{};
    GetLocalTime(&st);
    wchar_t name[64];
    swprintf(name, 64, L"dnspilot-backup-%04d%02d%02d.json", st.wYear, st.wMonth, st.wDay);
    std::wstring path = pickSaveFile(uiState.hwnd, name);
    if (path.empty()) return;

    Config c = uiState.ctx->cfg->snapshot();
    js::Value bundle = configExportBundle(c, uiState.ctx->version);
    std::string text = bundle.dump(2) + "\n";
    if (!util::fileWrite(narrow(path), text)) {
        msgErr(uiState.hwnd, L"无法写入文件：\n" + path);
        return;
    }
    toast(L"已全量导出 " + std::to_wstring(c.profiles.size()) + L" 个 DNS 配置、" +
          std::to_wstring(c.appRules.size() + c.domainRules.size()) + L" 条规则");
}

// ---------------------------------------------------------------- 导出单个配置
// 列出可导出的 DNS 配置（排除 system 与规则专用 auto），选中后只导出该配置，不含规则。

struct ExportPickState {
    HWND hwnd = nullptr;
    HWND list = nullptr;
    RECT btnOk{};
    RECT btnCancel{};
    int hot = 0;
    int result = -1;
    std::vector<std::wstring> names;
};

enum { ID_EXPORT_LIST = 2001 };

LRESULT CALLBACK ExportPickProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto *st = (ExportPickState *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    switch (msg) {
        case WM_NCCREATE: {
            auto *cs = (CREATESTRUCTW *)lp;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
            return TRUE;
        }
        case WM_CREATE: {
            auto *s = (ExportPickState *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
            s->hwnd = hwnd;
            RECT rc;
            GetClientRect(hwnd, &rc);
            int w = rc.right, h = rc.bottom;
            s->btnCancel = {w - S(196), h - S(56), w - S(108), h - S(18)};
            s->btnOk = {w - S(100), h - S(56), w - S(28), h - S(18)};
            s->list = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", L"",
                                      WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_TABSTOP | LBS_NOTIFY |
                                          LBS_NOINTEGRALHEIGHT | LBS_WANTKEYBOARDINPUT,
                                      S(24), S(88), w - S(48), h - S(88) - S(74), hwnd,
                                      (HMENU)(INT_PTR)ID_EXPORT_LIST, uiState.inst, nullptr);
            SendMessageW(s->list, WM_SETFONT, (WPARAM)uiState.editFont, TRUE);
            for (auto &n : s->names) SendMessageW(s->list, LB_ADDSTRING, 0, (LPARAM)n.c_str());
            if (!s->names.empty()) SendMessageW(s->list, LB_SETCURSEL, 0, 0);
            SetFocus(s->list);
            return 0;
        }
        case WM_CTLCOLORLISTBOX: {
            HDC dc = (HDC)wp;
            SetTextColor(dc, RGB(kText.GetR(), kText.GetG(), kText.GetB()));
            SetBkColor(dc, RGB(kInputBg2.GetR(), kInputBg2.GetG(), kInputBg2.GetB()));
            return (LRESULT)uiState.editBrush;
        }
        case WM_ERASEBKGND: {
            RECT erc;
            GetClientRect(hwnd, &erc);
            FillRect((HDC)wp, &erc, uiState.formBrush);
            return 1;
        }
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            RECT rc;
            GetClientRect(hwnd, &rc);
            Bitmap bmp(rc.right, rc.bottom);
            Graphics g(&bmp);
            g.SetSmoothingMode(SmoothingModeAntiAlias);
            g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);
            SolidBrush bg(kBg);
            g.FillRectangle(&bg, 0, 0, (INT)rc.right, (INT)rc.bottom);
            str(g, L"导出单个配置", uiState.fH2, kText,
                RectF((REAL)S(24), (REAL)S(18), (REAL)(rc.right - S(48)), (REAL)S(30)));
            str(g, L"选择一个 DNS 配置导出；只包含该配置，不含应用/域名规则。", uiState.fSmall, kMuted,
                RectF((REAL)S(24), (REAL)S(54), (REAL)(rc.right - S(48)), (REAL)S(20)));
            auto drawBtn = [&](RECT r, const wchar_t *text, bool primary, bool hot) {
                roundRect(g, RectF((REAL)r.left, (REAL)r.top, (REAL)(r.right - r.left),
                                   (REAL)(r.bottom - r.top)),
                          (REAL)S(7), primary ? kBtnPrimaryForm : (hot ? kBrowseHot : kBtnBg),
                          primary ? kBtnPrimaryBorderForm : kLine2);
                str(g, text, uiState.fSmall, primary ? kCyan : kText,
                    RectF((REAL)r.left, (REAL)r.top, (REAL)(r.right - r.left), (REAL)(r.bottom - r.top)),
                    StringAlignmentCenter);
            };
            drawBtn(st->btnCancel, L"取消", false, st->hot == 1);
            drawBtn(st->btnOk, L"导出…", true, st->hot == 2);
            Graphics screen(hdc);
            screen.DrawImage(&bmp, 0, 0);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_MOUSEMOVE: {
            POINT p{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            int hot = 0;
            if (PtInRect(&st->btnCancel, p)) hot = 1;
            else if (PtInRect(&st->btnOk, p)) hot = 2;
            if (hot != st->hot) {
                st->hot = hot;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }
        case WM_SETCURSOR:
            if (LOWORD(lp) == HTCLIENT) {
                POINT p;
                GetCursorPos(&p);
                ScreenToClient(hwnd, &p);
                if (PtInRect(&st->btnCancel, p) || PtInRect(&st->btnOk, p)) {
                    SetCursor(LoadCursorW(nullptr, MAKEINTRESOURCEW(32649)));
                    return TRUE;
                }
            }
            break;
        case WM_LBUTTONDOWN: {
            POINT p{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            if (PtInRect(&st->btnCancel, p)) {
                DestroyWindow(hwnd);
                return 0;
            }
            if (PtInRect(&st->btnOk, p)) {
                LRESULT sel = SendMessageW(st->list, LB_GETCURSEL, 0, 0);
                if (sel != LB_ERR) st->result = (int)sel;
                DestroyWindow(hwnd);
                return 0;
            }
            return 0;
        }
        case WM_VKEYTOITEM:
            if (LOWORD(wp) == VK_RETURN) {
                LRESULT sel = SendMessageW(st->list, LB_GETCURSEL, 0, 0);
                if (sel != LB_ERR) st->result = (int)sel;
                DestroyWindow(hwnd);
                return -2;
            }
            if (LOWORD(wp) == VK_ESCAPE) {
                DestroyWindow(hwnd);
                return -2;
            }
            break;
        case WM_COMMAND:
            if (LOWORD(wp) == ID_EXPORT_LIST && HIWORD(wp) == LBN_DBLCLK) {
                LRESULT sel = SendMessageW(st->list, LB_GETCURSEL, 0, 0);
                if (sel != LB_ERR) st->result = (int)sel;
                DestroyWindow(hwnd);
                return 0;
            }
            break;
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int showExportProfilePicker(HWND owner, const std::vector<const Profile *> &profiles) {
    static bool clsDone = false;
    if (!clsDone) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = ExportPickProc;
        wc.hInstance = uiState.inst;
        wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
        wc.hIcon = util::appIcon();
        wc.hIconSm = wc.hIcon;
        wc.hbrBackground = nullptr;
        wc.lpszClassName = L"DNSPilotExportPick";
        RegisterClassExW(&wc);
        clsDone = true;
    }
    ExportPickState st;
    for (auto *p : profiles) {
        std::wstring row = util::w(p->name);
        if (!p->servers.empty()) row += L"  ·  " + util::w(p->servers[0]);
        st.names.push_back(row);
    }
    int w = S(600);
    int h = S(470);
    RECT orc;
    GetWindowRect(owner, &orc);
    int x = orc.left + ((orc.right - orc.left) - w) / 2;
    int y = orc.top + ((orc.bottom - orc.top) - h) / 2;
    if (y < 20) y = 20;
    HWND dlg = CreateWindowExW(0, L"DNSPilotExportPick", L"导出单个配置",
                               WS_POPUP | WS_CAPTION | WS_SYSMENU, x, y, w, h, owner, nullptr,
                               uiState.inst, &st);
    if (!dlg) return -1;
    SendMessageW(dlg, WM_SETICON, ICON_SMALL, (LPARAM)util::appIcon());
    SendMessageW(dlg, WM_SETICON, ICON_BIG, (LPARAM)util::appIcon());
    applyDwmTheme(dlg);
    ShowWindow(dlg, SW_SHOW);
    UpdateWindow(dlg);
    EnableWindow(owner, FALSE);
    SetForegroundWindow(dlg);
    MSG msg;
    while (IsWindow(dlg) && GetMessageW(&msg, nullptr, 0, 0)) {
        if (!IsDialogMessageW(dlg, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    EnableWindow(owner, TRUE);
    SetForegroundWindow(owner);
    return st.result;
}

void exportSingleProfile() {
    Config c = uiState.ctx->cfg->snapshot();
    std::vector<const Profile *> cands;
    for (auto &p : c.profiles) {
        if (p.provider == "system" || p.provider == "auto") continue;
        cands.push_back(&p);
    }
    if (cands.empty()) {
        msgErr(uiState.hwnd, L"没有可单独导出的 DNS 配置。\n系统默认和规则专用配置不能单独导出。");
        return;
    }
    int idx = showExportProfilePicker(uiState.hwnd, cands);
    if (idx < 0 || idx >= (int)cands.size()) return;
    const Profile &p = *cands[idx];
    js::Value bundle = configExportBundle(c, uiState.ctx->version, p.id);
    if (!bundle.isObj()) {
        msgErr(uiState.hwnd, L"导出失败：配置不存在");
        return;
    }

    std::wstring id = util::w(p.id);
    for (auto &ch : id) {
        bool ok = (ch >= L'a' && ch <= L'z') || (ch >= L'A' && ch <= L'Z') ||
                  (ch >= L'0' && ch <= L'9') || ch == L'-' || ch == L'_' || ch == L'.';
        if (!ok) ch = L'-';
    }
    if (id.empty()) id = L"profile";
    SYSTEMTIME st{};
    GetLocalTime(&st);
    wchar_t name[160];
    swprintf(name, 160, L"dnspilot-%s-%04d%02d%02d.json", id.c_str(), st.wYear, st.wMonth, st.wDay);
    std::wstring path = pickSaveFile(uiState.hwnd, name, L"导出单个 DNS 配置");
    if (path.empty()) return;

    std::string text = bundle.dump(2) + "\n";
    if (!util::fileWrite(narrow(path), text)) {
        msgErr(uiState.hwnd, L"无法写入文件：\n" + path);
        return;
    }
    toast(L"已导出配置：" + util::w(p.name));
}

bool importConfigPath(const std::wstring &path, HWND owner) {
    if (path.empty()) return false;

    std::string text;
    if (!util::fileRead(narrow(path), text)) {
        msgErr(owner, L"无法读取文件：\n" + path);
        return false;
    }
    js::Value bundle;
    std::string perr;
    if (!js::Value::parse(text, bundle, perr)) {
        msgErr(owner, L"JSON 解析失败：" + util::w(perr));
        return false;
    }

    TransferStats stats;
    std::string ierr;
    std::string err;
    bool accepted = false;
    bool saved = uiState.ctx->cfg->update(
        [&](Config &c) {
            accepted = configImportBundle(bundle, c, stats, ierr);
            if (!accepted) return false;
            cleanupAutoProfiles(c);  // 规则被跳过时清掉无引用的规则专用配置
            return true;
        },
        err);
    if (!accepted) {
        msgErr(owner, L"导入失败：" + util::w(ierr));
        return false;
    }
    if (!saved) {
        msgErr(owner, L"导入失败：" + util::w(err));
        return false;
    }

    refreshData();
    InvalidateRect(uiState.hwnd, nullptr, FALSE);
    MessageBoxW(owner,
                (L"导入完成。\n\n新增 DNS 配置：" + std::to_wstring(stats.profilesAdded) +
                 L" 个（复用已有 " + std::to_wstring(stats.profilesReused) + L" 个）\n" +
                 L"新增规则：" + std::to_wstring(stats.rulesAdded) + L" 条\n" +
                 L"跳过：" + std::to_wstring(stats.rulesSkipped) + L" 条（重复或引用了无效配置）")
                    .c_str(),
                L"DNS Pilot", MB_OK | MB_ICONINFORMATION);
    return true;
}

// ---------------------------------------------------------------- 导入配置窗口
// 点击「导入配置…」打开：提示把导出的 JSON 拖入窗口，或点「选择文件…」。
// 拖放只在这个窗口内生效，主界面不直接接收拖放。

struct ImportDlgState {
    HWND hwnd = nullptr;
    RECT dropZone{};
    RECT btnPick{};
    RECT btnCancel{};
    int hot = 0;  // 1=选择文件，2=取消
};

#ifndef WM_COPYGLOBALDATA
#define WM_COPYGLOBALDATA 0x0049
#endif

void enableFileDrop(HWND hwnd) {
    DragAcceptFiles(hwnd, TRUE);
    // 提权运行时放行来自资源管理器（普通权限）的拖放消息
    ChangeWindowMessageFilterEx(hwnd, WM_DROPFILES, MSGFLT_ALLOW, nullptr);
    ChangeWindowMessageFilterEx(hwnd, WM_COPYGLOBALDATA, MSGFLT_ALLOW, nullptr);
    ChangeWindowMessageFilterEx(hwnd, WM_COPYDATA, MSGFLT_ALLOW, nullptr);
}

std::wstring takeDropPath(HDROP drop, UINT &count) {
    count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
    if (count != 1) {
        DragFinish(drop);
        return {};
    }
    UINT len = DragQueryFileW(drop, 0, nullptr, 0);
    std::wstring path((size_t)len + 1, L'\0');
    UINT copied = DragQueryFileW(drop, 0, &path[0], len + 1);
    DragFinish(drop);
    path.resize(copied);
    return path;
}

void importDlgPickFile(ImportDlgState &st) {
    std::wstring path = pickOpenFile(st.hwnd);
    if (path.empty()) return;
    if (importConfigPath(path, st.hwnd)) DestroyWindow(st.hwnd);
}

LRESULT CALLBACK ImportDlgProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto *st = (ImportDlgState *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    switch (msg) {
        case WM_NCCREATE: {
            auto *cs = (CREATESTRUCTW *)lp;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
            return TRUE;
        }
        case WM_CREATE: {
            auto *s = (ImportDlgState *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
            s->hwnd = hwnd;
            RECT rc;
            GetClientRect(hwnd, &rc);
            int w = rc.right, h = rc.bottom;
            s->dropZone = {S(28), S(140), w - S(28), S(244)};
            s->btnCancel = {w - S(116), h - S(56), w - S(28), h - S(18)};
            s->btnPick = {w - S(250), h - S(56), w - S(128), h - S(18)};
            enableFileDrop(hwnd);
            SetFocus(hwnd);
            return 0;
        }
        case WM_ERASEBKGND: {
            RECT erc;
            GetClientRect(hwnd, &erc);
            FillRect((HDC)wp, &erc, uiState.formBrush);
            return 1;
        }
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            RECT rc;
            GetClientRect(hwnd, &rc);
            Bitmap bmp(rc.right, rc.bottom);
            Graphics g(&bmp);
            g.SetSmoothingMode(SmoothingModeAntiAlias);
            g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);
            SolidBrush bg(kBg);
            g.FillRectangle(&bg, 0, 0, (INT)rc.right, (INT)rc.bottom);

            str(g, L"导入 DNS 配置", uiState.fH2, kText,
                RectF((REAL)S(28), (REAL)S(20), (REAL)(rc.right - S(56)), (REAL)S(30)));
            str(g, L"把 DNS Pilot 导出的 JSON 配置文件拖入下方区域，", uiState.fBody, kMuted,
                RectF((REAL)S(28), (REAL)S(62), (REAL)(rc.right - S(56)), (REAL)S(22)));
            str(g, L"或点击「选择文件…」从磁盘选择。一次只能导入一个文件。", uiState.fBody, kMuted,
                RectF((REAL)S(28), (REAL)S(86), (REAL)(rc.right - S(56)), (REAL)S(22)));
            str(g, L"导入为合并操作：不会删除现有配置，重复项自动跳过。", uiState.fTiny, kDim,
                RectF((REAL)S(28), (REAL)S(112), (REAL)(rc.right - S(56)), (REAL)S(18)));

            RECT dz = st->dropZone;
            roundRect(g, RectF((REAL)dz.left, (REAL)dz.top, (REAL)(dz.right - dz.left),
                               (REAL)(dz.bottom - dz.top)),
                      (REAL)S(10), kSelectBg);
            Pen dash(Color(150, kCyan.GetR(), kCyan.GetG(), kCyan.GetB()), (REAL)S(2));
            dash.SetDashStyle(DashStyleDash);
            g.DrawRectangle(&dash, (INT)(dz.left + S(1)), (INT)(dz.top + S(1)),
                            (INT)(dz.right - dz.left - S(2)), (INT)(dz.bottom - dz.top - S(2)));
            str(g, L"拖放配置文件到这里", uiState.fBody, kText,
                RectF((REAL)dz.left, (REAL)(dz.top + S(24)), (REAL)(dz.right - dz.left), (REAL)S(26)),
                StringAlignmentCenter);
            str(g, L"支持 DNS Pilot 导出的 .json 文件", uiState.fTiny, kDim,
                RectF((REAL)dz.left, (REAL)(dz.top + S(54)), (REAL)(dz.right - dz.left), (REAL)S(20)),
                StringAlignmentCenter);

            auto drawBtn = [&](RECT r, const wchar_t *text, bool primary, bool hot) {
                roundRect(g, RectF((REAL)r.left, (REAL)r.top, (REAL)(r.right - r.left),
                                   (REAL)(r.bottom - r.top)),
                          (REAL)S(7), primary ? kBtnPrimaryForm : (hot ? kBrowseHot : kBtnBg),
                          primary ? kBtnPrimaryBorderForm : kLine2);
                str(g, text, uiState.fSmall, primary ? kCyan : kText,
                    RectF((REAL)r.left, (REAL)r.top, (REAL)(r.right - r.left), (REAL)(r.bottom - r.top)),
                    StringAlignmentCenter);
            };
            drawBtn(st->btnPick, L"选择文件…", true, st->hot == 1);
            drawBtn(st->btnCancel, L"取消", false, st->hot == 2);
            str(g, L"按 Esc 取消", uiState.fTiny, kDim,
                RectF((REAL)S(28), (REAL)(rc.bottom - S(46)), (REAL)S(160), (REAL)S(18)));

            Graphics screen(hdc);
            screen.DrawImage(&bmp, 0, 0);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_MOUSEMOVE: {
            POINT p{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            int hot = 0;
            if (PtInRect(&st->btnPick, p)) hot = 1;
            else if (PtInRect(&st->btnCancel, p)) hot = 2;
            if (hot != st->hot) {
                st->hot = hot;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }
        case WM_SETCURSOR:
            if (LOWORD(lp) == HTCLIENT) {
                POINT p;
                GetCursorPos(&p);
                ScreenToClient(hwnd, &p);
                if (PtInRect(&st->btnPick, p) || PtInRect(&st->btnCancel, p)) {
                    SetCursor(LoadCursorW(nullptr, MAKEINTRESOURCEW(32649)));  // IDC_HAND
                    return TRUE;
                }
            }
            break;
        case WM_LBUTTONDOWN: {
            POINT p{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            if (PtInRect(&st->btnCancel, p)) {
                DestroyWindow(hwnd);
                return 0;
            }
            if (PtInRect(&st->btnPick, p)) {
                importDlgPickFile(*st);
                return 0;
            }
            return 0;
        }
        case WM_DROPFILES: {
            UINT count = 0;
            std::wstring path = takeDropPath((HDROP)wp, count);
            if (count > 1) {
                msgErr(hwnd, L"一次只能导入一个配置文件。");
                return 0;
            }
            if (!path.empty() && importConfigPath(path, hwnd)) DestroyWindow(hwnd);
            return 0;
        }
        case WM_KEYDOWN:
            if (wp == VK_ESCAPE) {
                DestroyWindow(hwnd);
                return 0;
            }
            break;
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void showImportDialog(HWND owner) {
    static bool clsDone = false;
    if (!clsDone) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = ImportDlgProc;
        wc.hInstance = uiState.inst;
        wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
        wc.hIcon = util::appIcon();
        wc.hIconSm = wc.hIcon;
        wc.hbrBackground = nullptr;  // 背景由 WM_ERASEBKGND 按主题填充
        wc.lpszClassName = L"DNSPilotImport";
        RegisterClassExW(&wc);
        clsDone = true;
    }
    ImportDlgState st;
    int w = S(560);
    int h = S(380);
    RECT orc;
    GetWindowRect(owner, &orc);
    int x = orc.left + ((orc.right - orc.left) - w) / 2;
    int y = orc.top + ((orc.bottom - orc.top) - h) / 2;
    if (y < 20) y = 20;
    HWND dlg = CreateWindowExW(0, L"DNSPilotImport", L"导入 DNS 配置",
                               WS_POPUP | WS_CAPTION | WS_SYSMENU, x, y, w, h, owner, nullptr,
                               uiState.inst, &st);
    if (!dlg) return;
    SendMessageW(dlg, WM_SETICON, ICON_SMALL, (LPARAM)util::appIcon());
    SendMessageW(dlg, WM_SETICON, ICON_BIG, (LPARAM)util::appIcon());
    applyDwmTheme(dlg);
    ShowWindow(dlg, SW_SHOW);
    UpdateWindow(dlg);
    EnableWindow(owner, FALSE);
    SetForegroundWindow(dlg);
    MSG msg;
    while (IsWindow(dlg) && GetMessageW(&msg, nullptr, 0, 0)) {
        if (!IsDialogMessageW(dlg, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    EnableWindow(owner, TRUE);
    SetForegroundWindow(owner);
}

void importConfigFile() {
    showImportDialog(uiState.hwnd);
}

// ---------------------------------------------------------------- 关闭确认
// 点标题栏 X / Alt+F4 时询问：最小化到任务栏继续运行，还是直接退出程序。

struct CloseDlgState {
    HWND hwnd = nullptr;
    RECT btnMin{};
    RECT btnExit{};
    RECT btnCancel{};
    int hot = 0;     // 1=最小化 2=退出 3=取消
    int result = 0;  // 0=取消 1=最小化 2=退出
};

LRESULT CALLBACK CloseDlgProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto *st = (CloseDlgState *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    switch (msg) {
        case WM_NCCREATE: {
            auto *cs = (CREATESTRUCTW *)lp;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
            return TRUE;
        }
        case WM_CREATE: {
            auto *s = (CloseDlgState *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
            s->hwnd = hwnd;
            RECT rc;
            GetClientRect(hwnd, &rc);
            int w = rc.right, h = rc.bottom;
            s->btnCancel = {w - S(110), h - S(56), w - S(28), h - S(18)};
            s->btnExit = {w - S(260), h - S(56), w - S(120), h - S(18)};
            s->btnMin = {w - S(450), h - S(56), w - S(270), h - S(18)};
            SetFocus(hwnd);
            return 0;
        }
        case WM_ERASEBKGND: {
            RECT erc;
            GetClientRect(hwnd, &erc);
            FillRect((HDC)wp, &erc, uiState.formBrush);
            return 1;
        }
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            RECT rc;
            GetClientRect(hwnd, &rc);
            Bitmap bmp(rc.right, rc.bottom);
            Graphics g(&bmp);
            g.SetSmoothingMode(SmoothingModeAntiAlias);
            g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);
            SolidBrush bg(kBg);
            g.FillRectangle(&bg, 0, 0, (INT)rc.right, (INT)rc.bottom);
            str(g, L"关闭 DNS Pilot", uiState.fH2, kText,
                RectF((REAL)S(28), (REAL)S(22), (REAL)(rc.right - S(56)), (REAL)S(30)));
            str(g, L"要最小化到任务栏继续运行，还是直接退出程序？", uiState.fBody, kMuted,
                RectF((REAL)S(28), (REAL)S(70), (REAL)(rc.right - S(56)), (REAL)S(22)));
            str(g, L"直接退出会按设置恢复系统 DNS；最小化不会中断接管。", uiState.fTiny, kDim,
                RectF((REAL)S(28), (REAL)S(98), (REAL)(rc.right - S(56)), (REAL)S(18)));

            auto drawBtn = [&](RECT r, const wchar_t *text, bool primary, bool danger, bool hot) {
                Color fill = primary ? kBtnPrimaryForm : (hot ? kBrowseHot : kBtnBg);
                Color border = primary ? kBtnPrimaryBorderForm : kLine2;
                roundRect(g, RectF((REAL)r.left, (REAL)r.top, (REAL)(r.right - r.left),
                                   (REAL)(r.bottom - r.top)),
                          (REAL)S(7), fill, border);
                str(g, text, uiState.fSmall, primary ? kCyan : (danger ? kRed : kText),
                    RectF((REAL)r.left, (REAL)r.top, (REAL)(r.right - r.left), (REAL)(r.bottom - r.top)),
                    StringAlignmentCenter);
            };
            drawBtn(st->btnMin, L"最小化到任务栏", true, false, st->hot == 1);
            drawBtn(st->btnExit, L"直接退出程序", false, true, st->hot == 2);
            drawBtn(st->btnCancel, L"取消", false, false, st->hot == 3);

            Graphics screen(hdc);
            screen.DrawImage(&bmp, 0, 0);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_MOUSEMOVE: {
            POINT p{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            int hot = 0;
            if (PtInRect(&st->btnMin, p)) hot = 1;
            else if (PtInRect(&st->btnExit, p)) hot = 2;
            else if (PtInRect(&st->btnCancel, p)) hot = 3;
            if (hot != st->hot) {
                st->hot = hot;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }
        case WM_SETCURSOR:
            if (LOWORD(lp) == HTCLIENT) {
                POINT p;
                GetCursorPos(&p);
                ScreenToClient(hwnd, &p);
                if (PtInRect(&st->btnMin, p) || PtInRect(&st->btnExit, p) || PtInRect(&st->btnCancel, p)) {
                    SetCursor(LoadCursorW(nullptr, MAKEINTRESOURCEW(32649)));
                    return TRUE;
                }
            }
            break;
        case WM_LBUTTONDOWN: {
            POINT p{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            if (PtInRect(&st->btnMin, p)) st->result = 1;
            else if (PtInRect(&st->btnExit, p)) st->result = 2;
            else if (PtInRect(&st->btnCancel, p)) st->result = 0;
            else return 0;
            DestroyWindow(hwnd);
            return 0;
        }
        case WM_KEYDOWN:
            if (wp == VK_ESCAPE) {
                st->result = 0;
                DestroyWindow(hwnd);
                return 0;
            }
            if (wp == VK_RETURN) {
                st->result = 1;
                DestroyWindow(hwnd);
                return 0;
            }
            break;
        case WM_CLOSE:
            st->result = 0;
            DestroyWindow(hwnd);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int showClosePrompt(HWND owner) {
    static bool clsDone = false;
    if (!clsDone) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = CloseDlgProc;
        wc.hInstance = uiState.inst;
        wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
        wc.hIcon = util::appIcon();
        wc.hIconSm = wc.hIcon;
        wc.hbrBackground = nullptr;
        wc.lpszClassName = L"DNSPilotClose";
        RegisterClassExW(&wc);
        clsDone = true;
    }
    if (IsIconic(owner)) ShowWindow(owner, SW_RESTORE);
    CloseDlgState st;
    int w = S(560);
    int h = S(300);
    RECT orc;
    GetWindowRect(owner, &orc);
    int x = orc.left + ((orc.right - orc.left) - w) / 2;
    int y = orc.top + ((orc.bottom - orc.top) - h) / 2;
    if (y < 20) y = 20;
    HWND dlg = CreateWindowExW(0, L"DNSPilotClose", L"关闭 DNS Pilot",
                               WS_POPUP | WS_CAPTION | WS_SYSMENU, x, y, w, h, owner, nullptr,
                               uiState.inst, &st);
    if (!dlg) return 2;  // 窗口创建失败时按“退出”处理
    SendMessageW(dlg, WM_SETICON, ICON_SMALL, (LPARAM)util::appIcon());
    SendMessageW(dlg, WM_SETICON, ICON_BIG, (LPARAM)util::appIcon());
    applyDwmTheme(dlg);
    ShowWindow(dlg, SW_SHOW);
    UpdateWindow(dlg);
    EnableWindow(owner, FALSE);
    SetForegroundWindow(dlg);
    MSG msg;
    while (IsWindow(dlg) && GetMessageW(&msg, nullptr, 0, 0)) {
        if (!IsDialogMessageW(dlg, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    EnableWindow(owner, TRUE);
    SetForegroundWindow(owner);
    return st.result;
}

void requestClose(HWND hwnd) {
    if (uiState.sessionEnding) {
        DestroyWindow(hwnd);
        return;
    }
    if (uiState.closePromptOpen) return;
    bool wasMin = IsIconic(hwnd) != 0;
    uiState.closePromptOpen = true;
    int r = showClosePrompt(hwnd);
    uiState.closePromptOpen = false;
    if (r == 1 || (r == 0 && wasMin)) ShowWindow(hwnd, SW_MINIMIZE);
    else if (r == 2) DestroyWindow(hwnd);
}

void applyTakeover(bool restore) {
    if (!util::isAdmin()) {
        msgErr(uiState.hwnd, L"接管系统 DNS 需要管理员权限。\n请点击“以管理员身份重启”，在 UAC 确认后重试。");
        return;
    }
    if (!restore) {
        if (uiState.ctx->cfg->takeover().active) {
            msgErr(uiState.hwnd, L"系统 DNS 已接管。请先点「恢复原 DNS」，再重新接管；"
                                 L"否则会把 127.0.0.1 当成原配置写入备份。");
            return;
        }
        if (uiState.cfg.resolver.port != 53) {
            msgErr(uiState.hwnd, L"系统接管要求解析端口为 53，当前为 " + std::to_wstring(uiState.cfg.resolver.port) +
                               L"。\n请在“设置”中修改端口并重启。");
            return;
        }
        std::string terr;
        if (!uiState.ctx->engine->selfTest(terr)) {
            msgErr(uiState.hwnd, L"自检失败，已拒绝接管：\n" + util::w(terr));
            return;
        }
        std::vector<TakeoverBackup> backup;
        TakeoverResult res;
        if (!SystemDns::apply(uiState.ctx->engine->ipv6Ready(), backup, res)) {
            msgErr(uiState.hwnd, L"接管失败：\n" + util::w(res.message));
            return;
        }
        uiState.ctx->cfg->setTakeover(true, backup);
        toast(L"系统 DNS 已接管");
    } else {
        TakeoverCfg tk = uiState.ctx->cfg->takeover();
        if (tk.backup.empty()) {
            msgErr(uiState.hwnd, L"没有可恢复的 DNS 备份");
            return;
        }
        // 备份失效（仅含 127.0.0.1/::1）时的 IPv4 兜底：使用 system 配置里探测到的系统 DNS
        std::vector<std::string> fallback;
        for (auto &p : uiState.ctx->cfg->snapshot().profiles)
            if (p.id == "system")
                for (auto &s : p.servers)
                    if (!util::isLoopbackHost(s)) fallback.push_back(s);
        TakeoverResult res;
        if (!SystemDns::restore(tk.backup, res, fallback)) {
            std::wstring detail = util::w(res.message);
            for (auto &e : res.errors) detail += L"\n- " + util::w(e);
            msgErr(uiState.hwnd, L"恢复失败：\n" + detail +
                                     (res.errors.empty() ? L"" : L"\n备份已保留，可重试。"));
            return;
        }
        uiState.ctx->cfg->setTakeover(false, {});
        toast(L"系统 DNS 已恢复");
    }
    uiState.adapters = SystemDns::adapters();
    refreshData();
    InvalidateRect(uiState.hwnd, nullptr, FALSE);
}

void elevate() {
    if (util::isAdmin()) {
        msgErr(uiState.hwnd, L"当前已经是管理员权限");
        return;
    }
    if (util::selfElevate(uiState.ctx->relaunchArgs)) {
        util::quitFlag() = true;
        DestroyWindow(uiState.hwnd);
    } else {
        msgErr(uiState.hwnd, L"提权请求被取消或失败");
    }
}

// ---------------------------------------------------------------- 页面绘制

void paintTitleBar(Graphics &g, const RECT &rc) {
    SolidBrush bar(kTitleBg);
    g.FillRectangle(&bar, 0, 0, (INT)rc.right, S(kTitleH));
    Pen line(kLine);
    g.DrawLine(&line, 0, S(kTitleH) - 1, (INT)rc.right, S(kTitleH) - 1);

    RectF lr((REAL)S(20), (REAL)S(12), (REAL)S(28), (REAL)S(28));
    LinearGradientBrush lg(PointF(lr.X, lr.Y), PointF(lr.X + lr.Width, lr.Y + lr.Height), kCyan, kViolet);
    GraphicsPath lp;
    float d = S(8);
    lp.AddArc(lr.X, lr.Y, d * 2, d * 2, 180, 90);
    lp.AddArc(lr.X + lr.Width - d * 2, lr.Y, d * 2, d * 2, 270, 90);
    lp.AddArc(lr.X + lr.Width - d * 2, lr.Y + lr.Height - d * 2, d * 2, d * 2, 0, 90);
    lp.AddArc(lr.X, lr.Y + lr.Height - d * 2, d * 2, d * 2, 90, 90);
    lp.CloseFigure();
    g.FillPath(&lg, &lp);
    str(g, L"D", uiState.fBold, kLogoText,
        RectF(lr.X, lr.Y - S(1), lr.Width, lr.Height), StringAlignmentCenter);

    str(g, L"DNS Pilot", uiState.fBold, kText,
        RectF((REAL)S(58), (REAL)S(8), (REAL)S(200), (REAL)S(20)));
    str(g, L"按应用 / 按域名路由", uiState.fTiny, kDim,
        RectF((REAL)S(58), (REAL)S(28), (REAL)S(200), (REAL)S(14)));

    uiState.btnClose = {rc.right - S(46), 0, rc.right, S(kTitleH)};
    uiState.btnMax = {rc.right - S(92), 0, rc.right - S(46), S(kTitleH)};
    uiState.btnMin = {rc.right - S(138), 0, rc.right - S(92), S(kTitleH)};
    uiState.btnTheme = {rc.right - S(184), 0, rc.right - S(138), S(kTitleH)};

    auto glyph = [&](RECT r, int kind) {
        bool hot = uiState.titleHot == kind;
        if (hot) {
            SolidBrush hb(kind == 3 ? kCloseHoverBg : kTitleHoverBg);
            g.FillRectangle(&hb, (INT)r.left, (INT)r.top, (INT)(r.right - r.left), (INT)(r.bottom - r.top));
        }
        Pen p((hot && kind == 3) ? Color(255, 255, 255, 255) : kTitleGlyph,
              (REAL)(1.2 * uiState.dpi / 96.0));
        int cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2;
        if (kind == 1) {
            g.DrawLine(&p, cx - S(5), cy, cx + S(5), cy);
        } else if (kind == 2) {
            if (IsZoomed(uiState.hwnd)) {
                g.DrawRectangle(&p, cx - S(6), cy - S(4), S(9), S(8));
                g.DrawRectangle(&p, cx - S(4), cy - S(6), S(9), S(8));
            } else {
                g.DrawRectangle(&p, cx - S(5), cy - S(5), S(10), S(10));
            }
        } else if (kind == 4) {
            // 主题按钮：浅色显示月亮（切到深色），深色显示太阳（切到浅色）
            SolidBrush b((hot ? kText : kMuted));
            if (gLightTheme) {
                g.FillEllipse(&b, cx - S(6), cy - S(6), S(12), S(12));
                SolidBrush cut(kTitleBg);
                g.FillEllipse(&cut, cx - S(2), cy - S(8), S(12), S(12));
            } else {
                g.DrawEllipse(&p, cx - S(4), cy - S(4), S(8), S(8));
                for (int i = 0; i < 8; i++) {
                    double a = i * 3.14159265358979323846 / 4.0;
                    int x1 = cx + (int)(std::cos(a) * S(6));
                    int y1 = cy + (int)(std::sin(a) * S(6));
                    int x2 = cx + (int)(std::cos(a) * S(9));
                    int y2 = cy + (int)(std::sin(a) * S(9));
                    g.DrawLine(&p, x1, y1, x2, y2);
                }
            }
        } else {
            g.DrawLine(&p, cx - S(5), cy - S(5), cx + S(5), cy + S(5));
            g.DrawLine(&p, cx - S(5), cy + S(5), cx + S(5), cy - S(5));
        }
    };
    glyph(uiState.btnTheme, 4);
    glyph(uiState.btnMin, 1);
    glyph(uiState.btnMax, 2);
    glyph(uiState.btnClose, 3);
}

void paintSidebar(Graphics &g, const RECT &rc) {
    SolidBrush panel(kPanel);
    g.FillRectangle(&panel, 0, S(kTitleH), S(kSidebarW), (INT)(rc.bottom - S(kTitleH)));
    Pen line(kLine);
    g.DrawLine(&line, S(kSidebarW), S(kTitleH), S(kSidebarW), (INT)rc.bottom);

    uiState.navRects.clear();
    int y = S(kTitleH) + S(18);
    for (int i = 0; i < kPageCount; i++) {
        RECT r{S(14), y, S(kSidebarW) - S(14), y + S(38)};
        bool active = (i == uiState.page);
        bool hover = (i == uiState.navHover);
        if (active || hover) {
            roundRect(g, RectF((REAL)r.left, (REAL)r.top, (REAL)(r.right - r.left), (REAL)(r.bottom - r.top)),
                      (REAL)S(8), active ? kNavActiveBg : kNavIdleBg,
                      active ? Color(80, 56, 225, 255) : Color(0, 0, 0, 0));
        }
        if (active) {
            SolidBrush bar(kCyan);
            g.FillRectangle(&bar, (INT)r.left, (INT)(r.top + S(10)), S(3), S(18));
        }
        str(g, kNavNames[i], uiState.fBody, active ? kText : (hover ? kText : kMuted),
            RectF((REAL)(r.left + S(22)), (REAL)r.top, (REAL)(r.right - r.left - S(30)), (REAL)(r.bottom - r.top)));
        uiState.navRects.push_back(r);
        y += S(44);
    }

    // 底部状态（面板地址一行：点击用默认浏览器打开）
    int fy = rc.bottom - S(78);
    RECT panelLink{S(10), fy - S(30), S(kSidebarW) - S(10), fy - S(4)};
    str(g, L"面板 127.0.0.1:" + std::to_wstring(uiState.cfg.resolver.httpPort), uiState.fTiny, kCyan,
        RectF((REAL)(panelLink.left + S(12)), (REAL)panelLink.top, (REAL)(panelLink.right - panelLink.left - S(18)),
              (REAL)(panelLink.bottom - panelLink.top)));
    addAct(panelLink, A_OPEN_PANEL);
    Pen sep(kLine);
    g.DrawLine(&sep, S(22), fy, S(kSidebarW) - S(22), fy);
    str(g, L"v" + widestr(uiState.ctx->version), uiState.fTiny, kDim,
        RectF((REAL)S(24), (REAL)(fy + S(12)), (REAL)S(90), (REAL)S(18)));
    bool adm = util::isAdmin();
    RECT pr{S(104), fy + S(12), S(kSidebarW) - S(22), fy + S(32)};
    badge(g, pr, adm ? L"管理员" : L"普通用户", adm ? kGreen : kAmber,
          adm ? kBadgeGoodBg : kBadgeWarnBg);
}

void paintHeader(Graphics &g, const RECT &rc) {
    int x = S(kSidebarW) + S(26);
    struct Pill {
        std::wstring text;
        Color dot;
        int w;
    };
    std::vector<Pill> pills;
    bool resolverReady = uiState.ctx && uiState.ctx->engine && uiState.ctx->engine->ready();
    pills.push_back({resolverReady ? (L"解析器 :" + std::to_wstring(uiState.cfg.resolver.port)) : L"解析器 未监听",
                     resolverReady ? kGreen : kRed, S(118)});
    pills.push_back({uiState.cfg.takeover.active ? L"系统接管 已接管" : L"系统接管 未接管",
                     uiState.cfg.takeover.active ? kGreen : kAmber, S(132)});
    pills.push_back({util::isAdmin() ? L"管理员" : L"普通用户", util::isAdmin() ? kGreen : kAmber,
                     S(86)});
    pills.push_back({uiState.cfg.resolver.cacheEnabled ? L"缓存 开启" : L"缓存 关闭",
                     uiState.cfg.resolver.cacheEnabled ? kGreen : kDim, S(100)});
    int total = 0;
    for (auto &p : pills) total += p.w + S(8);
    int pillsLeft = rc.right - S(26) - total;
    int textW = std::max(S(240), pillsLeft - x - S(18));

    str(g, kPageTitles[uiState.page], uiState.fH1, kText,
        RectF((REAL)x, (REAL)(S(kTitleH) + S(8)), (REAL)textW, (REAL)S(28)));
    str(g, kPageSubs[uiState.page], uiState.fSmall, kMuted,
        RectF((REAL)x, (REAL)(S(kTitleH) + S(38)), (REAL)textW, (REAL)S(16)));

    int right = rc.right - S(26);
    for (int i = (int)pills.size() - 1; i >= 0; i--) {
        RECT r{right - pills[i].w, S(kTitleH) + S(12), right, S(kTitleH) + S(40)};
        roundRect(g, RectF((REAL)r.left, (REAL)r.top, (REAL)(r.right - r.left), (REAL)(r.bottom - r.top)),
                  (REAL)S(14), kGlassBg, kLine2);
        SolidBrush b(pills[i].dot);
        g.FillEllipse(&b, (REAL)(r.left + S(11)), (REAL)(r.top + S(11)), (REAL)S(7), (REAL)S(7));
        str(g, pills[i].text, uiState.fTiny, kMuted,
            RectF((REAL)(r.left + S(22)), (REAL)r.top, (REAL)(r.right - r.left - S(26)),
                  (REAL)(r.bottom - r.top)));
        right = r.left - S(8);
    }
}

void paintKpi(Graphics &g, RECT r, const std::wstring &label, const std::wstring &value,
              const std::wstring &sub) {
    roundRect(g, RectF((REAL)r.left, (REAL)r.top, (REAL)(r.right - r.left), (REAL)(r.bottom - r.top)),
              (REAL)S(12), kCard, kLine);
    SolidBrush accent(kCyan);
    g.FillRectangle(&accent, (INT)(r.left + S(1)), (INT)(r.top + S(1)), (INT)(r.right - r.left - S(2)), S(2));
    str(g, label, uiState.fSmall, kMuted, RectF((REAL)(r.left + S(16)), (REAL)(r.top + S(12)),
                                          (REAL)(r.right - r.left - S(32)), (REAL)S(18)));
    str(g, value, uiState.fKpi, kText, RectF((REAL)(r.left + S(16)), (REAL)(r.top + S(34)),
                                       (REAL)(r.right - r.left - S(32)), (REAL)S(42)));
    str(g, sub, uiState.fTiny, kDim, RectF((REAL)(r.left + S(16)), (REAL)(r.bottom - S(24)),
                                     (REAL)(r.right - r.left - S(32)), (REAL)S(16)));
}

void paintOverview(Graphics &g, const RECT &content) {
    int cw = content.right - content.left;
    int gap = S(14);
    int cardW = (cw - gap * 3) / 4;
    int y = content.top;
    auto &s = uiState.stats;
    wchar_t buf[64];
    swprintf(buf, 64, L"%llu", (unsigned long long)s.total);
    paintKpi(g, RECT{content.left, y, content.left + cardW, y + S(96)}, L"DNS 查询总数", buf,
             s.total ? (L"失败 " + std::to_wstring(s.failures) + L" 次") : L"等待流量…");
    swprintf(buf, 64, L"%.1f%%", s.hitRate * 100.0);
    paintKpi(g, RECT{content.left + (cardW + gap), y, content.left + (cardW * 2 + gap), y + S(96)},
             L"DNS 缓存命中率", buf, L"命中 " + std::to_wstring(s.cacheHits) + L" 次");
    swprintf(buf, 64, L"%.1f ms", s.avgLatency);
    paintKpi(g, RECT{content.left + (cardW + gap) * 2, y, content.left + (cardW * 3 + gap * 2), y + S(96)},
             L"平均解析延迟", buf, L"仅统计上游请求");
    paintKpi(g, RECT{content.left + (cardW + gap) * 3, y, content.right, y + S(96)}, L"代理连接",
             std::to_wstring(s.proxyConns), L"按应用解析次数");

    // 图表卡片
    RECT chartCard{content.left, y + S(112), content.left + (cw * 62 / 100), y + S(112) + S(196)};
    roundRect(g, RectF((REAL)chartCard.left, (REAL)chartCard.top, (REAL)(chartCard.right - chartCard.left),
                       (REAL)(chartCard.bottom - chartCard.top)),
              (REAL)S(12), kCard, kLine);
    str(g, L"最近 60 秒请求量", uiState.fH2, kText,
        RectF((REAL)(chartCard.left + S(16)), (REAL)(chartCard.top + S(10)), (REAL)S(220), (REAL)S(22)));
    RECT chartRect{chartCard.left + S(14), chartCard.top + S(40), chartCard.right - S(14), chartCard.bottom - S(12)};
    if (uiState.chart) {
        // 只在位置/尺寸变化时调整子窗口，避免每次刷新都触发子窗口重绘
        if (!uiState.chartPlaced || !EqualRect(&uiState.chartRect, &chartRect)) {
            SetWindowPos(uiState.chart, nullptr, chartRect.left, chartRect.top,
                         chartRect.right - chartRect.left, chartRect.bottom - chartRect.top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            uiState.chartRect = chartRect;
            uiState.chartPlaced = true;
        }
        if (!IsWindowVisible(uiState.chart)) ShowWindow(uiState.chart, SW_SHOW);
    }

    // 排行卡片
    RECT topCard{chartCard.right + gap, chartCard.top, content.right, chartCard.bottom};
    roundRect(g, RectF((REAL)topCard.left, (REAL)topCard.top, (REAL)(topCard.right - topCard.left),
                       (REAL)(topCard.bottom - topCard.top)),
              (REAL)S(12), kCard, kLine);
    str(g, L"热门域名", uiState.fSmall, kMuted, RectF((REAL)(topCard.left + S(16)), (REAL)(topCard.top + S(12)),
                                                (REAL)S(150), (REAL)S(18)));
    str(g, L"活跃应用", uiState.fSmall, kMuted, RectF((REAL)(topCard.left + S(190)), (REAL)(topCard.top + S(12)),
                                                (REAL)S(150), (REAL)S(18)));
    int ty = topCard.top + S(36);
    int halfW = (topCard.right - topCard.left - S(48)) / 2;
    auto drawTop = [&](const std::vector<UiCount> &list, int x, int w) {
        for (size_t i = 0; i < list.size() && i < 5; i++) {
            int yy = ty + (int)i * S(28);
            str(g, util::w(list[i].name), uiState.fMono, kText,
                RectF((REAL)x, (REAL)yy, (REAL)(w - S(46)), (REAL)S(20)));
            str(g, std::to_wstring(list[i].count), uiState.fTiny, kDim,
                RectF((REAL)(x + w - S(40)), (REAL)yy, (REAL)S(40), (REAL)S(20)), StringAlignmentFar);
        }
        if (list.empty())
            str(g, L"暂无数据", uiState.fSmall, kDim, RectF((REAL)x, (REAL)ty, (REAL)w, (REAL)S(18)));
    };
    drawTop(s.topDomains, topCard.left + S(16), halfW);
    drawTop(s.topApps, topCard.left + S(16) + halfW + S(16), halfW);

    // 实时流卡片
    RECT live{content.left, chartCard.bottom + gap, content.right, content.bottom};
    roundRect(g, RectF((REAL)live.left, (REAL)live.top, (REAL)(live.right - live.left),
                       (REAL)(live.bottom - live.top)),
              (REAL)S(12), kCard, kLine);
    str(g, L"实时解析流", uiState.fH2, kText,
        RectF((REAL)(live.left + S(16)), (REAL)(live.top + S(10)), (REAL)S(160), (REAL)S(22)));
    str(g, L"共 " + std::to_wstring(s.total) + L" 次 DNS", uiState.fTiny, kDim,
        RectF((REAL)(live.right - S(200)), (REAL)(live.top + S(12)), (REAL)S(184), (REAL)S(18)),
        StringAlignmentFar);
    int hx = live.left + S(16);
    int hw = live.right - live.left - S(32);
    int cy = live.top + S(40);
    int wTime = S(76), wSrc = S(46), wApp = S(112), wRule = S(52), wProf = S(150), wLat = S(72),
        wHit = S(48), wSt = S(70);
    int wDom = hw - (wTime + wSrc + wApp + wRule + wProf + wLat + wHit + wSt);
    if (wDom < S(120)) wDom = S(120);
    int colTime = hx;
    int colSrc = colTime + wTime;
    int colApp = colSrc + wSrc;
    int colDom = colApp + wApp;
    int colRule = colDom + wDom;
    int colProf = colRule + wRule;
    int colLat = colProf + wProf;
    int colHit = colLat + wLat;
    int colSt = colHit + wHit;
    str(g, L"时间", uiState.fTiny, kDim, RectF((REAL)colTime, (REAL)cy, (REAL)(wTime - S(6)), (REAL)S(18)));
    str(g, L"来源", uiState.fTiny, kDim, RectF((REAL)colSrc, (REAL)cy, (REAL)(wSrc - S(4)), (REAL)S(18)));
    str(g, L"应用", uiState.fTiny, kDim, RectF((REAL)colApp, (REAL)cy, (REAL)(wApp - S(8)), (REAL)S(18)));
    str(g, L"域名", uiState.fTiny, kDim, RectF((REAL)colDom, (REAL)cy, (REAL)(wDom - S(8)), (REAL)S(18)));
    str(g, L"规则", uiState.fTiny, kDim, RectF((REAL)colRule, (REAL)cy, (REAL)(wRule - S(4)), (REAL)S(18)));
    str(g, L"DNS 配置", uiState.fTiny, kDim, RectF((REAL)colProf, (REAL)cy, (REAL)(wProf - S(8)), (REAL)S(18)));
    str(g, L"延迟", uiState.fTiny, kDim, RectF((REAL)colLat, (REAL)cy, (REAL)(wLat - S(6)), (REAL)S(18)));
    str(g, L"缓存", uiState.fTiny, kDim, RectF((REAL)colHit, (REAL)cy, (REAL)(wHit - S(6)), (REAL)S(18)));
    str(g, L"状态", uiState.fTiny, kDim, RectF((REAL)colSt, (REAL)cy, (REAL)(wSt - S(8)), (REAL)S(18)));
    int rowY = cy + S(22);
    int rows = (live.bottom - S(12) - rowY) / S(30);
    int count = (int)s.events.size();
    for (int i = 0; i < rows && i < count; i++) {
        const UiEvent &e = s.events[count - 1 - i];
        int ry = rowY + i * S(30);
        if (i % 2) {
            SolidBrush sb(kStripe);
            g.FillRectangle(&sb, (INT)(live.left + S(1)), ry, (INT)(live.right - live.left - S(2)), S(30));
        }
        SYSTEMTIME st{};
        FILETIME ft{}, lft{};
        ULARGE_INTEGER ui;
        ui.QuadPart = e.t * 10000ULL + 116444736000000000ULL;
        ft.dwLowDateTime = ui.LowPart;
        ft.dwHighDateTime = ui.HighPart;
        FileTimeToLocalFileTime(&ft, &lft);
        FileTimeToSystemTime(&lft, &st);
        wchar_t tbuf[16];
        swprintf(tbuf, 16, L"%02d:%02d:%02d", st.wHour, st.wMinute, st.wSecond);
        str(g, tbuf, uiState.fMono, kDim, RectF((REAL)colTime, (REAL)ry, (REAL)(wTime - S(6)), (REAL)S(30)));
        RECT srcR{colSrc, ry + S(7), colSrc + S(40), ry + S(24)};
        badge(g, srcR, e.source == "proxy" ? L"代理" : L"DNS",
              e.source == "proxy" ? kBadgeProxyFg : kBadgeDnsFg,
              e.source == "proxy" ? kBadgeProxyBg : kBadgeInfoBg);
        str(g, util::w(e.client), uiState.fMono, kText,
            RectF((REAL)colApp, (REAL)ry, (REAL)(wApp - S(8)), (REAL)S(30)));
        str(g, util::w(e.domain), uiState.fMono, kDomainText,
            RectF((REAL)colDom, (REAL)ry, (REAL)(wDom - S(8)), (REAL)S(30)));
        RECT ruleR{colRule, ry + S(7), colRule + S(46), ry + S(24)};
        if (e.ruleType == "domain")
            badge(g, ruleR, L"域名", kGreen, kBadgeGoodBg);
        else if (e.ruleType == "app")
            badge(g, ruleR, L"应用", kAmber, kBadgeWarnBg);
        else
            badge(g, ruleR, L"默认", kMuted, kBadgeNeutralBg);
        str(g, util::w(e.profileName), uiState.fSmall, kMuted,
            RectF((REAL)colProf, (REAL)ry, (REAL)(wProf - S(8)), (REAL)S(30)));
        std::wstring lat = e.cached ? L"缓存" : (std::to_wstring((int)e.latencyMs) + L" ms");
        str(g, lat, uiState.fMono, e.cached ? kDim : (e.latencyMs < 60 ? kGreen : (e.latencyMs < 200 ? kAmber : kRed)),
            RectF((REAL)colLat, (REAL)ry, (REAL)(wLat - S(6)), (REAL)S(30)));
        bool ok = e.status == "ok";
        bool nx = e.status == "nxdomain";
        RECT stR{colSt, ry + S(7), colSt + S(60), ry + S(24)};
        badge(g, stR, ok ? L"成功" : (nx ? L"不存在" : L"失败"),
              ok ? kGreen : (nx ? kAmber : kRed),
              ok ? kBadgeGoodBg : (nx ? kBadgeWarnBg : kBadgeBadBg));
        if (e.cached) {
            RECT hitR{colHit, ry + S(7), colHit + S(38), ry + S(24)};
            badge(g, hitR, L"HIT", kGreen, Color(0, 0, 0, 0));
        }
    }
    if (count == 0) {
        std::wstring hint;
        if (uiState.cfg.takeover.active)
            hint = L"已接管系统 DNS，但还没有查询；请确认应用正在访问网络。";
        else if (util::isAdmin())
            hint = L"还没有流量经过本程序。仅运行程序不会自动监测：请到「系统接管」页接管，"
                   L"或让应用使用本机代理 127.0.0.1:8899。";
        else
            hint = L"还没有流量经过本程序。接管系统 DNS 需要管理员：请点「以管理员身份重启」，"
                   L"或让应用使用本机代理 127.0.0.1:8899。";
        str(g, hint, uiState.fSmall, kDim,
            RectF((REAL)(live.left + S(16)), (REAL)rowY, (REAL)hw, (REAL)S(40)));
    }
}

void paintRulePage(Graphics &g, const RECT &content, bool app) {
    const auto &rules = app ? uiState.cfg.appRules : uiState.cfg.domainRules;
    int y = content.top;
    std::wstring hint = app ? L"进程可运行中直接选择，也可手动输入 exe 名称（支持 * 通配）"
                            : L"域名匹配自身及所有子域，例如 netflix.com 会匹配 www.netflix.com";
    str(g, hint, uiState.fSmall, kMuted, RectF((REAL)content.left, (REAL)(y + S(6)), (REAL)S(560), (REAL)S(20)));
    RECT addBtn{content.right - S(150), y - S(2), content.right, y + S(34)};
    roundRect(g, RectF((REAL)addBtn.left, (REAL)addBtn.top, (REAL)(addBtn.right - addBtn.left),
                       (REAL)(addBtn.bottom - addBtn.top)),
              (REAL)S(8), kBtnPrimary, kBtnPrimaryBorder);
    str(g, app ? L"+  选择进程添加" : L"+  添加域名规则", uiState.fBody, kCyan,
        RectF((REAL)addBtn.left, (REAL)addBtn.top, (REAL)(addBtn.right - addBtn.left), (REAL)(addBtn.bottom - addBtn.top)),
        StringAlignmentCenter);
    addAct(addBtn, app ? A_APP_ADD : A_DOM_ADD);

    RECT card{content.left, y + S(48), content.right, content.bottom - S(10)};
    roundRect(g, RectF((REAL)card.left, (REAL)card.top, (REAL)(card.right - card.left),
                       (REAL)(card.bottom - card.top)),
              (REAL)S(12), kCard, kLine);
    int hx = card.left + S(16), hw = card.right - card.left - S(32);
    int hy = card.top + S(12);
    int cMatch = hx, cProf = hx + (int)(hw * 0.36), cNote = hx + (int)(hw * 0.62);
    str(g, app ? L"进程" : L"域名", uiState.fTiny, kDim, RectF((REAL)cMatch, (REAL)hy, (REAL)S(160), (REAL)S(16)));
    str(g, L"DNS 配置", uiState.fTiny, kDim, RectF((REAL)cProf, (REAL)hy, (REAL)S(150), (REAL)S(16)));
    str(g, L"备注", uiState.fTiny, kDim, RectF((REAL)cNote, (REAL)hy, (REAL)S(150), (REAL)S(16)));
    str(g, L"启用", uiState.fTiny, kDim,
        RectF((REAL)(card.right - S(224)), (REAL)hy, (REAL)S(50), (REAL)S(16)));
    str(g, L"操作", uiState.fTiny, kDim,
        RectF((REAL)(card.right - S(112)), (REAL)hy, (REAL)S(80), (REAL)S(16)));
    Pen sep(kLine);
    g.DrawLine(&sep, (INT)(card.left + S(1)), hy + S(22), (INT)(card.right - S(1)), hy + S(22));

    int rowH = S(50);
    int listTop = hy + S(30);
    int visible = (card.bottom - S(8) - listTop) / rowH;
    int total = (int)rules.size();
    int maxScroll = std::max(0, total - visible);
    uiState.maxScroll[uiState.page] = maxScroll;
    if (uiState.scroll[uiState.page] > maxScroll) uiState.scroll[uiState.page] = maxScroll;
    int start = uiState.scroll[uiState.page];
    for (int i = 0; i < visible && start + i < total; i++) {
        int idx = start + i;
        const Rule &r = rules[idx];
        int ry = listTop + i * rowH;
        if (i % 2) {
            SolidBrush sb(kStripe);
            g.FillRectangle(&sb, (INT)(card.left + S(1)), ry, (INT)(card.right - card.left - S(2)), rowH);
        }
        str(g, util::w(r.match), uiState.fMono, kText,
            RectF((REAL)cMatch, (REAL)ry, (REAL)(cProf - cMatch - S(10)), (REAL)rowH));
        bool autoP = isAutoProfile(uiState.cfg, r.profile);
        str(g, util::w(profileNameOf(uiState.cfg, r.profile)) + (autoP ? L"  · 专用" : L""), uiState.fSmall,
            autoP ? kCyan : kMuted, RectF((REAL)cProf, (REAL)ry, (REAL)(cNote - cProf - S(8)), (REAL)rowH));
        str(g, util::w(r.note), uiState.fSmall, kDim,
            RectF((REAL)cNote, (REAL)ry, (REAL)(card.right - cNote - S(210)), (REAL)rowH));
        RECT sw{card.right - S(196), ry + S(16), card.right - S(160), ry + S(34)};
        switchBox(g, sw, r.enabled);
        addAct(sw, app ? A_APP_TOGGLE : A_DOM_TOGGLE, idx);
        RECT ed{card.right - S(118), ry + S(14), card.right - S(74), ry + S(36)};
        str(g, L"编辑", uiState.fTiny, kMuted,
            RectF((REAL)ed.left, (REAL)ed.top, (REAL)(ed.right - ed.left), (REAL)(ed.bottom - ed.top)),
            StringAlignmentCenter);
        addAct(ed, app ? A_APP_EDIT : A_DOM_EDIT, idx);
        RECT del{card.right - S(48), ry + S(14), card.right - S(22), ry + S(36)};
        str(g, L"×", uiState.fH2, kDim,
            RectF((REAL)del.left, (REAL)del.top, (REAL)(del.right - del.left), (REAL)(del.bottom - del.top)),
            StringAlignmentCenter);
        addAct(del, app ? A_APP_DEL : A_DOM_DEL, idx);
    }
    if (total == 0) {
        const wchar_t *empty = app ? L"还没有应用规则。点击右上角“选择进程添加”，从运行中的程序里挑一个。"
                                   : L"还没有域名规则。点击右上角“添加域名规则”，让某个网站走单独的 DNS。";
        str(g, empty, uiState.fSmall, kDim, RectF((REAL)(card.left + S(18)), (REAL)(card.top + S(70)),
                                            (REAL)(card.right - card.left - S(36)), (REAL)S(40)));
    }
    if (maxScroll > 0) {
        str(g, L"滚轮查看更多", uiState.fTiny, kDim,
            RectF((REAL)(card.right - S(140)), (REAL)(card.bottom - S(24)), (REAL)S(124), (REAL)S(16)),
            StringAlignmentFar);
    }
}

void paintProfiles(Graphics &g, const RECT &content) {
    int y = content.top;
    str(g, L"“系统默认”会自动探测当前网卡的 DNS；自定义配置按顺序使用，主服务器失败后回退。",
        uiState.fSmall, kMuted, RectF((REAL)content.left, (REAL)(y + S(6)), (REAL)S(640), (REAL)S(20)));
    RECT addBtn{content.right - S(140), y - S(2), content.right, y + S(34)};
    roundRect(g, RectF((REAL)addBtn.left, (REAL)addBtn.top, (REAL)(addBtn.right - addBtn.left),
                       (REAL)(addBtn.bottom - addBtn.top)),
              (REAL)S(8), kBtnPrimary, kBtnPrimaryBorder);
    str(g, L"+  添加 DNS 配置", uiState.fBody, kCyan,
        RectF((REAL)addBtn.left, (REAL)addBtn.top, (REAL)(addBtn.right - addBtn.left),
              (REAL)(addBtn.bottom - addBtn.top)),
        StringAlignmentCenter);
    addAct(addBtn, A_PROF_ADD);

    RECT card{content.left, y + S(48), content.right, content.bottom - S(10)};
    roundRect(g, RectF((REAL)card.left, (REAL)card.top, (REAL)(card.right - card.left),
                       (REAL)(card.bottom - card.top)),
              (REAL)S(12), kCard, kLine);
    int rowH = S(68);
    int top = card.top + S(10);
    int visible = (card.bottom - S(10) - top) / rowH;
    int total = (int)uiState.cfg.profiles.size();
    int maxScroll = std::max(0, total - visible);
    uiState.maxScroll[uiState.page] = maxScroll;
    if (uiState.scroll[uiState.page] > maxScroll) uiState.scroll[uiState.page] = maxScroll;
    int start = uiState.scroll[uiState.page];
    for (int i = 0; i < visible && start + i < total; i++) {
        int idx = start + i;
        const Profile &p = uiState.cfg.profiles[idx];
        int ry = top + i * rowH;
        bool sys = p.id == "system";
        if (sys) {
            SolidBrush sb(kSysRowBg);
            g.FillRectangle(&sb, (INT)(card.left + S(1)), ry, (INT)(card.right - card.left - S(2)), rowH);
            SolidBrush bar(kAmber);
            g.FillRectangle(&bar, (INT)(card.left + S(1)), ry, S(2), rowH);
        }
        str(g, util::w(p.name), uiState.fBold, kText,
            RectF((REAL)(card.left + S(18)), (REAL)(ry + S(8)), (REAL)S(240), (REAL)S(22)));
        RECT badgeR{card.left + S(18), ry + S(34), card.left + S(104), ry + S(52)};
        badge(g, badgeR, p.strategy == "only" ? L"仅主用" : L"主用+回退", kCyan, kBadgeInfoBg);
        if (sys) {
            RECT sysR{card.left + S(110), ry + S(34), card.left + S(186), ry + S(52)};
            badge(g, sysR, L"自动探测", kAmber, kBadgeWarnBg);
        }
        str(g, joinServers(p.servers), uiState.fMono, kMuted,
            RectF((REAL)(card.left + S(200)), (REAL)(ry + S(8)), (REAL)(card.right - S(230) - card.left - S(200)),
                  (REAL)S(52)));
        if (!sys) {
            RECT ed{card.right - S(118), ry + S(20), card.right - S(74), ry + S(44)};
            str(g, L"编辑", uiState.fTiny, kMuted,
                RectF((REAL)ed.left, (REAL)ed.top, (REAL)(ed.right - ed.left), (REAL)(ed.bottom - ed.top)),
                StringAlignmentCenter);
            addAct(ed, A_PROF_EDIT, idx);
            RECT del{card.right - S(48), ry + S(20), card.right - S(22), ry + S(44)};
            str(g, L"×", uiState.fH2, kDim,
                RectF((REAL)del.left, (REAL)del.top, (REAL)(del.right - del.left), (REAL)(del.bottom - del.top)),
                StringAlignmentCenter);
            addAct(del, A_PROF_DEL, idx);
        }
    }
}

void paintBrowsers(Graphics &g, const RECT &content) {
    int y = content.top;
    RECT info{content.left, y, content.right, y + S(66)};
    roundRect(g, RectF((REAL)info.left, (REAL)info.top, (REAL)(info.right - info.left),
                       (REAL)(info.bottom - info.top)),
              (REAL)S(10), kInfoBg, kInfoBorder);
    str(g, L"浏览器只支持“安全 DNS（DoH）”。选择内置配置会自动填入对应 DoH 地址；", uiState.fSmall,
        kMuted, RectF((REAL)(info.left + S(16)), (REAL)(info.top + S(10)), (REAL)(info.right - info.left - S(32)),
                      (REAL)S(18)));
    str(g, L"选择“系统默认”会关闭浏览器的安全 DNS（回到系统解析）。修改前请完全退出浏览器。",
        uiState.fSmall, kMuted,
        RectF((REAL)(info.left + S(16)), (REAL)(info.top + S(32)), (REAL)(info.right - info.left - S(32)),
              (REAL)S(18)));
    RECT ref{content.right - S(126), y + S(16), content.right - S(16), y + S(48)};
    roundRect(g, RectF((REAL)ref.left, (REAL)ref.top, (REAL)(ref.right - ref.left),
                       (REAL)(ref.bottom - ref.top)),
              (REAL)S(8), kBtnPrimary, kLine2);
    str(g, L"重新扫描", uiState.fSmall, kText,
        RectF((REAL)ref.left, (REAL)ref.top, (REAL)(ref.right - ref.left), (REAL)(ref.bottom - ref.top)),
        StringAlignmentCenter);
    addAct(ref, A_BROWSER_REFRESH);

    RECT card{content.left, y + S(78), content.right, content.bottom - S(10)};
    roundRect(g, RectF((REAL)card.left, (REAL)card.top, (REAL)(card.right - card.left),
                       (REAL)(card.bottom - card.top)),
              (REAL)S(12), kCard, kLine);
    int rowH = S(78);
    int top = card.top + S(10);
    int visible = (card.bottom - S(10) - top) / rowH;
    int total = (int)uiState.browsers.size();
    int maxScroll = std::max(0, total - visible);
    uiState.maxScroll[uiState.page] = maxScroll;
    if (uiState.scroll[uiState.page] > maxScroll) uiState.scroll[uiState.page] = maxScroll;
    int start = uiState.scroll[uiState.page];
    for (int i = 0; i < visible && start + i < total; i++) {
        int idx = start + i;
        auto &b = uiState.browsers[idx];
        int ry = top + i * rowH;
        if (i % 2) {
            SolidBrush sb(kStripe);
            g.FillRectangle(&sb, (INT)(card.left + S(1)), ry, (INT)(card.right - card.left - S(2)), rowH);
        }
        RECT br{card.left + S(16), ry + S(12), card.left + S(86), ry + S(32)};
        badge(g, br, util::w(b.browserName), kCyan, kBadgeInfoBg);
        str(g, util::w(b.name), uiState.fBold, kText,
            RectF((REAL)(card.left + S(96)), (REAL)(ry + S(8)), (REAL)S(220), (REAL)S(22)));
        if (!b.email.empty())
            str(g, util::w(b.email), uiState.fTiny, kDim,
                RectF((REAL)(card.left + S(96)), (REAL)(ry + S(30)), (REAL)S(240), (REAL)S(16)));
        std::wstring cur;
        if (b.mode == "secure")
            cur = L"DoH · " + util::w(b.dohUrl);
        else if (b.mode == "off")
            cur = L"安全 DNS 已关闭";
        else if (b.mode == "automatic")
            cur = L"自动（系统）";
        else
            cur = L"未设置";
        str(g, cur, uiState.fMono, b.mode == "secure" ? kGreen : kMuted,
            RectF((REAL)(card.left + S(340)), (REAL)(ry + S(10)), (REAL)(card.right - card.left - S(560)),
                  (REAL)S(22)));
        str(g, util::w(b.folder), uiState.fTiny, kDim,
            RectF((REAL)(card.left + S(340)), (REAL)(ry + S(34)), (REAL)S(260), (REAL)S(16)));
        RECT stR{card.right - S(360), ry + S(22), card.right - S(286), ry + S(44)};
        badge(g, stR, b.running ? L"运行中" : L"已关闭", b.running ? kAmber : kGreen,
              b.running ? kBadgeWarnBg : kBadgeGoodBg);
        RECT bind{card.right - S(272), ry + S(18), card.right - S(150), ry + S(48)};
        roundRect(g, RectF((REAL)bind.left, (REAL)bind.top, (REAL)(bind.right - bind.left),
                           (REAL)(bind.bottom - bind.top)),
                  (REAL)S(8), b.running ? kBadgeNeutralBg : kBtnPrimary,
                  b.running ? kLine : kBtnPrimaryBorder);
        str(g, L"绑定 DNS", uiState.fSmall, b.running ? kDim : kCyan,
            RectF((REAL)bind.left, (REAL)bind.top, (REAL)(bind.right - bind.left), (REAL)(bind.bottom - bind.top)),
            StringAlignmentCenter);
        if (!b.running) addAct(bind, A_BROWSER_BIND, idx);
        RECT rst{card.right - S(140), ry + S(18), card.right - S(28), ry + S(48)};
        roundRect(g, RectF((REAL)rst.left, (REAL)rst.top, (REAL)(rst.right - rst.left),
                           (REAL)(rst.bottom - rst.top)),
                  (REAL)S(8), kBtnBg, kLine2);
        str(g, L"恢复默认", uiState.fSmall, b.running ? kDim : kMuted,
            RectF((REAL)rst.left, (REAL)rst.top, (REAL)(rst.right - rst.left), (REAL)(rst.bottom - rst.top)),
            StringAlignmentCenter);
        if (!b.running) addAct(rst, A_BROWSER_RESET, idx);
    }
    if (total == 0) {
        str(g, L"没有找到浏览器配置文件夹。Chrome / Edge 默认位于 %LOCALAPPDATA%，Firefox 位于 %APPDATA%。",
            uiState.fSmall, kDim, RectF((REAL)(card.left + S(18)), (REAL)(card.top + S(40)),
                                  (REAL)(card.right - card.left - S(36)), (REAL)S(40)));
    } else if (maxScroll > 0) {
        str(g, L"滚轮查看更多", uiState.fTiny, kDim,
            RectF((REAL)(card.right - S(140)), (REAL)(card.bottom - S(24)), (REAL)S(124), (REAL)S(16)),
            StringAlignmentFar);
    }
}

void paintTakeover(Graphics &g, const RECT &content) {
    int y = content.top;
    RECT status{content.left, y, content.right, y + S(116)};
    bool active = uiState.cfg.takeover.active;
    roundRect(g, RectF((REAL)status.left, (REAL)status.top, (REAL)(status.right - status.left),
                       (REAL)(status.bottom - status.top)),
              (REAL)S(12), active ? kTakeoverOnBg : kTakeoverOffBg,
              active ? kTakeoverOnBorder : kTakeoverOffBorder);
    str(g, active ? L"系统 DNS 已接管" : L"系统 DNS 未接管", uiState.fH2, active ? kGreen : kAmber,
        RectF((REAL)(status.left + S(18)), (REAL)(status.top + S(14)), (REAL)S(320), (REAL)S(24)));
    str(g, active ? L"所有应用的域名解析都会经过本程序，域名规则全系统生效。"
                  : (util::isAdmin() ? L"接管后所有应用都走本机解析器；退出时会自动恢复原 DNS。"
                                     : L"接管需要管理员权限，请先点击“以管理员身份重启”。"),
        uiState.fSmall, kMuted,
        RectF((REAL)(status.left + S(18)), (REAL)(status.top + S(44)), (REAL)(status.right - status.left - S(36)),
              (REAL)S(40)));
    int by = status.top + S(76);
    auto btn = [&](const wchar_t *text, int kind, bool primary, int xoff, int w) {
        RECT r{status.left + S(xoff), by, status.left + S(xoff + w), by + S(30)};
        roundRect(g, RectF((REAL)r.left, (REAL)r.top, (REAL)(r.right - r.left), (REAL)(r.bottom - r.top)),
                  (REAL)S(7), primary ? kBtnPrimary : kBtnBg,
                  primary ? kBtnPrimaryBorder : kLine2);
        str(g, text, uiState.fSmall, primary ? kCyan : kText,
            RectF((REAL)r.left, (REAL)r.top, (REAL)(r.right - r.left), (REAL)(r.bottom - r.top)),
            StringAlignmentCenter);
        addAct(r, kind);
    };
    btn(L"接管系统 DNS", A_TAKEOVER, !active, 18, 132);
    btn(L"恢复原 DNS", A_RESTORE, false, 160, 112);
    if (!util::isAdmin()) btn(L"以管理员身份重启", A_ELEVATE, true, 282, 160);
    if (active) {
        str(g, L"备份 " + std::to_wstring(uiState.cfg.takeover.backup.size()) + L" 项网卡设置",
            uiState.fTiny, kDim, RectF((REAL)(status.right - S(200)), (REAL)(by + S(6)), (REAL)S(184), (REAL)S(18)),
            StringAlignmentFar);
    }

    RECT card{content.left, status.bottom + S(14), content.right, content.bottom - S(10)};
    roundRect(g, RectF((REAL)card.left, (REAL)card.top, (REAL)(card.right - card.left),
                       (REAL)(card.bottom - card.top)),
              (REAL)S(12), kCard, kLine);
    str(g, L"当前网卡 DNS", uiState.fH2, kText,
        RectF((REAL)(card.left + S(16)), (REAL)(card.top + S(10)), (REAL)S(180), (REAL)S(22)));
    int hy = card.top + S(42);
    int hx = card.left + S(16);
    str(g, L"网卡", uiState.fTiny, kDim, RectF((REAL)hx, (REAL)hy, (REAL)S(180), (REAL)S(16)));
    str(g, L"IPv4 DNS", uiState.fTiny, kDim, RectF((REAL)(hx + S(190)), (REAL)hy, (REAL)S(300), (REAL)S(16)));
    str(g, L"IPv6 DNS", uiState.fTiny, kDim, RectF((REAL)(hx + S(500)), (REAL)hy, (REAL)S(200), (REAL)S(16)));
    str(g, L"配置方式", uiState.fTiny, kDim, RectF((REAL)(hx + S(720)), (REAL)hy, (REAL)S(120), (REAL)S(16)));
    Pen sep(kLine);
    g.DrawLine(&sep, card.left + S(1), hy + S(20), card.right - S(1), hy + S(20));
    int rowH = S(44);
    int top = hy + S(28);
    for (size_t i = 0; i < uiState.adapters.size(); i++) {
        auto &a = uiState.adapters[i];
        if (!a.up) continue;
        int ry = top + (int)i * rowH;
        if (ry + rowH > card.bottom - S(6)) break;
        str(g, util::w(a.alias), uiState.fBody, kText,
            RectF((REAL)hx, (REAL)ry, (REAL)S(180), (REAL)S(44)));
        std::wstring d4, d6;
        for (size_t k = 0; k < a.dns4.size(); k++) {
            if (k) d4 += L", ";
            d4 += util::w(a.dns4[k]);
        }
        for (size_t k = 0; k < a.dns6.size(); k++) {
            if (k) d6 += L", ";
            d6 += util::w(a.dns6[k]);
        }
        str(g, d4.empty() ? L"—" : d4, uiState.fMono, kMuted,
            RectF((REAL)(hx + S(190)), (REAL)ry, (REAL)S(300), (REAL)S(44)));
        str(g, d6.empty() ? L"—" : d6, uiState.fMono, kDim,
            RectF((REAL)(hx + S(500)), (REAL)ry, (REAL)S(210), (REAL)S(44)));
        str(g, a.mode4 == "static" ? L"静态" : L"DHCP", uiState.fSmall, kDim,
            RectF((REAL)(hx + S(720)), (REAL)ry, (REAL)S(120), (REAL)S(44)));
    }
}

// 绑定查询页：按程序 / 按域名查看当前生效的 DNS（只读）
void paintLookup(Graphics &g, const RECT &content) {
    int cw = content.right - content.left;
    int gap = S(14);
    int cardW = (cw - gap) / 2;
    int cardH = S(344);
    RECT cards[2] = {
        {content.left, content.top, content.left + cardW, content.top + cardH},
        {content.left + cardW + gap, content.top, content.right, content.top + cardH},
    };

    auto drawCard = [&](const RECT &r, const wchar_t *title, const wchar_t *button, int action) {
        roundRect(g, RectF((REAL)r.left, (REAL)r.top, (REAL)(r.right - r.left), (REAL)(r.bottom - r.top)),
                  (REAL)S(12), kCard, kLine);
        str(g, title, uiState.fH2, kText,
            RectF((REAL)(r.left + S(18)), (REAL)(r.top + S(14)), (REAL)(cardW - S(180)), (REAL)S(22)));
        RECT br{r.right - S(18) - S(124), r.top + S(10), r.right - S(18), r.top + S(42)};
        roundRect(g, RectF((REAL)br.left, (REAL)br.top, (REAL)(br.right - br.left), (REAL)(br.bottom - br.top)),
                  (REAL)S(8), kBtnPrimary, kBtnPrimaryBorder);
        str(g, button, uiState.fBody, kCyan,
            RectF((REAL)br.left, (REAL)br.top, (REAL)(br.right - br.left), (REAL)(br.bottom - br.top)),
            StringAlignmentCenter);
        addAct(br, action);
    };
    drawCard(cards[0], L"按程序查看", L"选择进程…", A_LOOKUP_EXE);
    drawCard(cards[1], L"按域名查看", L"输入域名…", A_LOOKUP_DOMAIN);

    auto drawRows = [&](const RECT &r, const std::wstring &subjectLabel, const std::wstring &subject,
                        const BindingView &bv) {
        struct Row {
            std::wstring label;
            std::wstring value;
            bool mono;
        };
        std::vector<Row> rows;
        rows.push_back({subjectLabel, subject, true});
        rows.push_back({L"生效规则", bv.rule, false});
        rows.push_back({L"DNS 配置", bv.profile, false});
        rows.push_back({L"服务器", bv.servers, true});
        rows.push_back({L"策略", bv.strategy, false});

        int x = r.left + S(20);
        int w = (r.right - r.left) - S(40);
        int rowH = S(52);
        int y = r.top + S(62);
        for (size_t i = 0; i < rows.size(); i++) {
            int ry = y + (int)i * rowH;
            str(g, rows[i].label, uiState.fTiny, kMuted, RectF((REAL)x, (REAL)ry, (REAL)w, (REAL)S(16)));
            str(g, rows[i].value, rows[i].mono ? uiState.fMono : uiState.fBody, kText,
                RectF((REAL)x, (REAL)(ry + S(19)), (REAL)w, (REAL)S(22)));
            if (i + 1 < rows.size()) {
                Pen sep(kLineSoft);
                g.DrawLine(&sep, x, ry + rowH - S(8), x + w, ry + rowH - S(8));
            }
        }
    };

    std::string exe = narrow(uiState.lookupExe);
    if (exe.empty()) {
        str(g, L"选择一个正在运行的程序，查看它当前生效的 DNS。", uiState.fSmall, kDim,
            RectF((REAL)(cards[0].left + S(20)), (REAL)(cards[0].top + S(74)), (REAL)(cardW - S(40)),
                  (REAL)S(44)));
    } else {
        drawRows(cards[0], L"程序", uiState.lookupExe, describeBinding("", exe));
    }

    std::string domain = narrow(uiState.lookupDomain);
    if (domain.empty()) {
        str(g, L"输入域名或粘贴网址，查看它当前生效的 DNS。", uiState.fSmall, kDim,
            RectF((REAL)(cards[1].left + S(20)), (REAL)(cards[1].top + S(74)), (REAL)(cardW - S(40)),
                  (REAL)S(44)));
    } else {
        drawRows(cards[1], L"域名", uiState.lookupDomain, describeBinding(domain, ""));
    }

    // 当前环境
    RECT env{content.left, content.top + cardH + gap, content.right, content.top + cardH + gap + S(116)};
    roundRect(g, RectF((REAL)env.left, (REAL)env.top, (REAL)(env.right - env.left), (REAL)(env.bottom - env.top)),
              (REAL)S(12), kCard, kLine);
    str(g, L"当前环境", uiState.fH2, kText,
        RectF((REAL)(env.left + S(18)), (REAL)(env.top + S(12)), (REAL)S(160), (REAL)S(22)));
    int envColW = (env.right - env.left - S(36)) / 4;
    int envY = env.top + S(50);
    auto envCol = [&](int i, const wchar_t *label, const std::wstring &value) {
        int x = env.left + S(18) + i * envColW;
        str(g, label, uiState.fTiny, kMuted, RectF((REAL)x, (REAL)envY, (REAL)(envColW - S(10)), (REAL)S(16)));
        str(g, value, uiState.fBody, kText,
            RectF((REAL)x, (REAL)(envY + S(20)), (REAL)(envColW - S(10)), (REAL)S(22)));
    };
    envCol(0, L"解析器", util::w(uiState.cfg.resolver.listen) + L":" +
                          std::to_wstring(uiState.cfg.resolver.port));
    envCol(1, L"系统接管", uiState.cfg.takeover.active ? L"已接管" : L"未接管");
    envCol(2, L"默认配置", util::w(profileNameOf(uiState.cfg, uiState.cfg.defaultProfile)));
    envCol(3, L"缓存", uiState.cfg.resolver.cacheEnabled
                           ? L"开启 · TTL " + std::to_wstring(uiState.cfg.resolver.minTtl) + L"–" +
                                 std::to_wstring(uiState.cfg.resolver.maxTtl)
                           : L"关闭");

    // 说明
    RECT info{content.left, env.bottom + gap, content.right, env.bottom + gap + S(116)};
    roundRect(g, RectF((REAL)info.left, (REAL)info.top, (REAL)(info.right - info.left),
                       (REAL)(info.bottom - info.top)),
              (REAL)S(12), kInfoBg, kInfoBorder);
    str(g, L"匹配顺序", uiState.fH2, kText,
        RectF((REAL)(info.left + S(18)), (REAL)(info.top + S(12)), (REAL)S(160), (REAL)S(22)));
    str(g, L"域名规则（最长后缀优先）→ 应用规则（按顺序，首个匹配）→ 默认配置。", uiState.fSmall, kMuted,
        RectF((REAL)(info.left + S(18)), (REAL)(info.top + S(44)), (REAL)(info.right - info.left - S(36)),
              (REAL)S(20)));
    str(g, L"应用绑定只对自带解析器或走本机代理的程序生效；经系统 DNS 的普通程序请用域名规则或代理。",
        uiState.fSmall, kMuted,
        RectF((REAL)(info.left + S(18)), (REAL)(info.top + S(66)), (REAL)(info.right - info.left - S(36)),
              (REAL)S(20)));
    str(g, L"此页只读：不修改配置，也不发起解析请求；system 配置的服务器在程序启动时从网卡自动探测。",
        uiState.fTiny, kDim,
        RectF((REAL)(info.left + S(18)), (REAL)(info.top + S(90)), (REAL)(info.right - info.left - S(36)),
              (REAL)S(18)));
}

void paintSettings(Graphics &g, const RECT &content) {
    int y = content.top;
    RECT editBtn{content.right - S(126), y - S(2), content.right, y + S(34)};
    roundRect(g, RectF((REAL)editBtn.left, (REAL)editBtn.top, (REAL)(editBtn.right - editBtn.left),
                       (REAL)(editBtn.bottom - editBtn.top)),
              (REAL)S(8), kBtnPrimary, kBtnPrimaryBorder);
    str(g, L"修改设置", uiState.fBody, kCyan,
        RectF((REAL)editBtn.left, (REAL)editBtn.top, (REAL)(editBtn.right - editBtn.left),
              (REAL)(editBtn.bottom - editBtn.top)),
        StringAlignmentCenter);
    addAct(editBtn, A_SETTINGS);

    RECT card{content.left, y + S(48), content.right, y + S(48) + S(368)};
    roundRect(g, RectF((REAL)card.left, (REAL)card.top, (REAL)(card.right - card.left),
                       (REAL)(card.bottom - card.top)),
              (REAL)S(12), kCard, kLine);
    struct KV {
        const wchar_t *k;
        std::wstring v;
    };
    std::vector<KV> kv;
    kv.push_back({L"默认 DNS 配置", util::w(profileNameOf(uiState.cfg, uiState.cfg.defaultProfile))});
    kv.push_back({L"缓存", uiState.cfg.resolver.cacheEnabled ? L"开启" : L"关闭"});
    kv.push_back({L"TTL 范围", std::to_wstring(uiState.cfg.resolver.minTtl) + L" – " +
                                  std::to_wstring(uiState.cfg.resolver.maxTtl) + L" 秒"});
    kv.push_back({L"上游超时", std::to_wstring(uiState.cfg.resolver.queryTimeoutMs) + L" ms"});
    kv.push_back({L"应用代理", uiState.cfg.proxy.enabled ? (L"启用 · 127.0.0.1:" + std::to_wstring(uiState.cfg.proxy.port))
                                                   : L"停用"});
    kv.push_back({L"监听端口", L"解析 " + std::to_wstring(uiState.cfg.resolver.port) + L" · 面板 " +
                                  std::to_wstring(uiState.cfg.resolver.httpPort)});
    kv.push_back({L"退出行为", uiState.cfg.ui.restoreOnExit ? L"自动恢复系统 DNS" : L"保留接管状态"});
    kv.push_back({L"桌面窗口", uiState.cfg.ui.nativeWindow ? L"启动时打开" : L"仅 Web 面板"});
    kv.push_back({L"界面主题", uiState.cfg.ui.theme == "dark" ? L"深色" : L"浅色（白底）"});
    int ry = card.top + S(12);
    for (auto &e : kv) {
        str(g, e.k, uiState.fSmall, kMuted, RectF((REAL)(card.left + S(18)), (REAL)ry, (REAL)S(220), (REAL)S(32)));
        str(g, e.v, uiState.fSmall, kText,
            RectF((REAL)(card.left + S(250)), (REAL)ry, (REAL)(card.right - card.left - S(280)), (REAL)S(32)),
            StringAlignmentFar);
        Pen sep(kLineSoft);
        g.DrawLine(&sep, (INT)(card.left + S(18)), ry + S(32), (INT)(card.right - S(18)), ry + S(32));
        ry += S(38);
    }

    RECT info{content.left, card.bottom + S(14), content.right, card.bottom + S(14) + S(116)};
    roundRect(g, RectF((REAL)info.left, (REAL)info.top, (REAL)(info.right - info.left),
                       (REAL)(info.bottom - info.top)),
              (REAL)S(12), kCard, kLine);
    uint64_t up = (util::nowMs() - uiState.startMs) / 1000;
    std::wstring upS = std::to_wstring(up / 3600) + L" 小时 " + std::to_wstring((up % 3600) / 60) +
                       L" 分 " + std::to_wstring(up % 60) + L" 秒";
    str(g, L"程序信息", uiState.fH2, kText,
        RectF((REAL)(info.left + S(16)), (REAL)(info.top + S(10)), (REAL)S(160), (REAL)S(22)));
    str(g, L"版本 v" + widestr(uiState.ctx->version) + L"   ·   PID " + std::to_wstring(GetCurrentProcessId()) +
             L"   ·   运行 " + upS,
        uiState.fSmall, kMuted,
        RectF((REAL)(info.left + S(18)), (REAL)(info.top + S(40)), (REAL)(info.right - info.left - S(36)),
              (REAL)S(20)));
    str(g, L"配置：" + util::w(uiState.ctx->cfg->path()), uiState.fMono, kDim,
        RectF((REAL)(info.left + S(18)), (REAL)(info.top + S(64)), (REAL)(info.right - info.left - S(36)),
              (REAL)S(20)));
    str(g, L"提示：点击「导入配置…」后，可把 JSON 文件拖入弹窗或选择文件（合并，不删除现有配置）", uiState.fTiny, kDim,
        RectF((REAL)(info.left + S(18)), (REAL)(info.top + S(88)), (REAL)(info.right - info.left - S(36)),
              (REAL)S(18)));

    // 底部按钮：配置导入导出 + 缓存/退出
    RECT expBtn{content.left, info.bottom + S(14), content.left + S(120), info.bottom + S(48)};
    RECT oneBtn{content.left + S(132), info.bottom + S(14), content.left + S(252), info.bottom + S(48)};
    RECT impBtn{content.left + S(264), info.bottom + S(14), content.left + S(384), info.bottom + S(48)};
    RECT clear{content.left + S(396), info.bottom + S(14), content.left + S(516), info.bottom + S(48)};
    RECT quit{content.left + S(528), info.bottom + S(14), content.left + S(648), info.bottom + S(48)};
    for (const auto &b : {std::make_pair(expBtn, L"全量导出…"), std::make_pair(oneBtn, L"单个导出…"),
                          std::make_pair(impBtn, L"导入配置…"), std::make_pair(clear, L"清空缓存"),
                          std::make_pair(quit, L"退出程序")}) {
        roundRect(g, RectF((REAL)b.first.left, (REAL)b.first.top, (REAL)(b.first.right - b.first.left),
                           (REAL)(b.first.bottom - b.first.top)),
                  (REAL)S(8), kBtnBg, kLine2);
        str(g, b.second, uiState.fSmall, kText,
            RectF((REAL)b.first.left, (REAL)b.first.top, (REAL)(b.first.right - b.first.left),
                  (REAL)(b.first.bottom - b.first.top)),
            StringAlignmentCenter);
    }
    addAct(expBtn, A_EXPORT_CONFIG);
    addAct(oneBtn, A_EXPORT_PROFILE);
    addAct(impBtn, A_IMPORT_CONFIG);
    addAct(clear, A_CLEAR);
    addAct(quit, A_QUIT);
}

void paintPage(Graphics &g, const RECT &content) {
    switch (uiState.page) {
        case 0: paintOverview(g, content); break;
        case 1: paintRulePage(g, content, true); break;
        case 2: paintRulePage(g, content, false); break;
        case 3: paintProfiles(g, content); break;
        case 4: paintBrowsers(g, content); break;
        case 5: paintTakeover(g, content); break;
        case 6: paintLookup(g, content); break;
        case 7: paintSettings(g, content); break;
    }
}

void paintToast(Graphics &g, const RECT &rc) {
    if (uiState.toast.empty() || util::nowMs() > uiState.toastUntil) return;
    int w = S(320), h = S(48);
    RECT r{rc.right - w - S(20), rc.bottom - h - S(20), rc.right - S(20), rc.bottom - S(20)};
    roundRect(g, RectF((REAL)r.left, (REAL)r.top, (REAL)(r.right - r.left), (REAL)(r.bottom - r.top)),
              (REAL)S(10), kBtnBg, kBtnPrimaryBorder);
    str(g, uiState.toast, uiState.fSmall, kText,
        RectF((REAL)(r.left + S(14)), (REAL)r.top, (REAL)(r.right - r.left - S(28)), (REAL)(r.bottom - r.top)));
}

// ---------------------------------------------------------------- 交互

void onAction(int kind, int index) {
    switch (kind) {
        case A_APP_ADD: openRuleDialog(true, -1); break;
        case A_DOM_ADD: openRuleDialog(false, -1); break;
        case A_APP_EDIT: openRuleDialog(true, index); break;
        case A_DOM_EDIT: openRuleDialog(false, index); break;
        case A_PROF_ADD: openProfileDialog(-1); break;
        case A_PROF_EDIT: openProfileDialog(index); break;
        case A_SETTINGS: openSettingsDialog(); break;
        case A_BROWSER_BIND: openBrowserDialog(index); break;
        case A_LOOKUP_EXE: {
            procpick::Picked p;
            if (procpick::Show(uiState.hwnd, p) && !p.exeName.empty()) {
                uiState.lookupExe = p.exeName;
                InvalidateRect(uiState.hwnd, nullptr, FALSE);
            }
            break;
        }
        case A_LOOKUP_DOMAIN:
            openDomainLookupDialog();
            break;
        case A_EXPORT_CONFIG: exportConfigFile(); break;
        case A_EXPORT_PROFILE: exportSingleProfile(); break;
        case A_IMPORT_CONFIG: importConfigFile(); break;
        case A_OPEN_PANEL: {
            std::wstring url = L"http://127.0.0.1:" + std::to_wstring(uiState.cfg.resolver.httpPort) + L"/";
            ShellExecuteW(nullptr, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            toast(L"已在浏览器打开面板");
            break;
        }
        case A_TAKEOVER: applyTakeover(false); break;
        case A_RESTORE: applyTakeover(true); break;
        case A_ELEVATE: elevate(); break;
        case A_CLEAR:
            uiState.ctx->engine->flushCache();
            toast(L"缓存已清空");
            break;
        case A_QUIT: DestroyWindow(uiState.hwnd); break;
        case A_BROWSER_REFRESH:
            uiState.browsers = browsers::scan();
            toast(L"已重新扫描浏览器配置");
            InvalidateRect(uiState.hwnd, nullptr, FALSE);
            break;
        case A_APP_TOGGLE:
        case A_DOM_TOGGLE: {
            bool app = kind == A_APP_TOGGLE;
            std::string err;
            uiState.ctx->cfg->update(
                [&](Config &c) {
                    auto &rules = app ? c.appRules : c.domainRules;
                    if (index >= 0 && index < (int)rules.size()) rules[index].enabled = !rules[index].enabled;
                    return true;
                },
                err);
            refreshData();
            InvalidateRect(uiState.hwnd, nullptr, FALSE);
            break;
        }
        case A_APP_DEL:
        case A_DOM_DEL: {
            bool app = kind == A_APP_DEL;
            if (MessageBoxW(uiState.hwnd, app ? L"删除这条应用规则？" : L"删除这条域名规则？", L"DNS Pilot",
                            MB_YESNO | MB_ICONQUESTION) != IDYES)
                return;
            std::string err;
            uiState.ctx->cfg->update(
                [&](Config &c) {
                    auto &rules = app ? c.appRules : c.domainRules;
                    if (index >= 0 && index < (int)rules.size()) rules.erase(rules.begin() + index);
                    cleanupAutoProfiles(c);
                    return true;
                },
                err);
            refreshData();
            toast(L"规则已删除");
            InvalidateRect(uiState.hwnd, nullptr, FALSE);
            break;
        }
        case A_PROF_DEL: {
            if (index < 0 || index >= (int)uiState.cfg.profiles.size()) return;
            const Profile &p = uiState.cfg.profiles[index];
            if (p.id == "system") return;
            if (MessageBoxW(uiState.hwnd, L"删除该 DNS 配置？引用它的规则会改为默认配置。", L"DNS Pilot",
                            MB_YESNO | MB_ICONQUESTION) != IDYES)
                return;
            std::string id = p.id, err;
            uiState.ctx->cfg->update(
                [&](Config &c) {
                    c.profiles.erase(std::remove_if(c.profiles.begin(), c.profiles.end(),
                                                    [&](const Profile &x) { return x.id == id; }),
                                     c.profiles.end());
                    if (c.defaultProfile == id) c.defaultProfile = "system";
                    for (auto &r : c.appRules)
                        if (r.profile == id) r.profile = c.defaultProfile;
                    for (auto &r : c.domainRules)
                        if (r.profile == id) r.profile = c.defaultProfile;
                    cleanupAutoProfiles(c);
                    return true;
                },
                err);
            refreshData();
            toast(L"DNS 配置已删除");
            InvalidateRect(uiState.hwnd, nullptr, FALSE);
            break;
        }
        case A_BROWSER_RESET: {
            if (index < 0 || index >= (int)uiState.browsers.size()) return;
            auto b = uiState.browsers[index];
            std::string msg, backup;
            if (!browsers::apply(b.browser, b.folder, "", true, msg, backup)) {
                msgErr(uiState.hwnd, util::w(msg));
                return;
            }
            uiState.browsers = browsers::scan();
            toast(L"已恢复浏览器默认 DNS");
            InvalidateRect(uiState.hwnd, nullptr, FALSE);
            break;
        }
    }
}

// ---------------------------------------------------------------- 窗口

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE:
            uiState.hwnd = hwnd;
            SetTimer(hwnd, ID_TIMER, 700, nullptr);
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            RECT rc;
            GetClientRect(hwnd, &rc);
            Bitmap bmp(rc.right, rc.bottom);
            Graphics gr(&bmp);
            gr.SetSmoothingMode(SmoothingModeAntiAlias);
            gr.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);
            SolidBrush bg(kBg);
            gr.FillRectangle(&bg, 0, 0, (INT)rc.right, (INT)rc.bottom);
            // 顶部微光
            LinearGradientBrush glow(PointF(0, 0), PointF((REAL)rc.right, 0), kGlowA, kGlowB);
            gr.FillRectangle(&glow, S(kSidebarW), 0, rc.right - S(kSidebarW), S(2));

            clearActions();
            paintTitleBar(gr, rc);
            paintSidebar(gr, rc);
            paintHeader(gr, rc);
            RECT content{S(kSidebarW) + S(26), S(kTitleH) + S(kHeaderH) + S(6), rc.right - S(26), rc.bottom - S(14)};
            if (uiState.page != 0 && uiState.chart && IsWindowVisible(uiState.chart))
                ShowWindow(uiState.chart, SW_HIDE);
            paintPage(gr, content);
            paintToast(gr, rc);

            Graphics screen(hdc);
            screen.DrawImage(&bmp, 0, 0);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_TIMER:
            if (wp == ID_TIMER) {
                refreshData();
                if (util::nowMs() - uiState.lastAdapter > 2000) {
                    uiState.adapters = SystemDns::adapters();
                    uiState.lastAdapter = util::nowMs();
                }
                if (uiState.page == 0 && uiState.chart) {
                    std::wstring label = L"合计 " + std::to_wstring(uiState.stats.total) + L" 次";
                    uichart::SetData(uiState.chart, uiState.stats.timeline, label);
                }
                if (util::quitFlag()) {
                    DestroyWindow(hwnd);
                    return 0;
                }
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        case WM_MOUSEMOVE: {
            int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
            POINT pt{x, y};
            int hot = 0;
            if (y < S(kTitleH)) {
                if (PtInRect(&uiState.btnTheme, pt)) hot = 4;
                else if (PtInRect(&uiState.btnMin, pt)) hot = 1;
                else if (PtInRect(&uiState.btnMax, pt)) hot = 2;
                else if (PtInRect(&uiState.btnClose, pt)) hot = 3;
            }
            if (hot != uiState.titleHot) {
                uiState.titleHot = hot;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            int hover = -1;
            for (size_t i = 0; i < uiState.navRects.size(); i++)
                if (PtInRect(&uiState.navRects[i], pt)) hover = (int)i;
            if (hover != uiState.navHover) {
                uiState.navHover = hover;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }
        case WM_LBUTTONDOWN: {
            int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
            POINT p{x, y};
            if (y < S(kTitleH)) {
                if (PtInRect(&uiState.btnTheme, p)) {
                    toggleTheme();
                    return 0;
                }
                if (PtInRect(&uiState.btnClose, p)) {
                    PostMessageW(hwnd, WM_CLOSE, 0, 0);
                    return 0;
                }
                if (PtInRect(&uiState.btnMax, p)) {
                    ShowWindow(hwnd, IsZoomed(hwnd) ? SW_RESTORE : SW_MAXIMIZE);
                    return 0;
                }
                if (PtInRect(&uiState.btnMin, p)) {
                    ShowWindow(hwnd, SW_MINIMIZE);
                    return 0;
                }
                return 0;
            }
            for (size_t i = 0; i < uiState.navRects.size(); i++) {
                if (PtInRect(&uiState.navRects[i], p)) {
                    if (uiState.page != (int)i) {
                        uiState.page = (int)i;
                        uiState.scroll[uiState.page] = 0;
                        if (uiState.page == 4) uiState.browsers = browsers::scan();
                        if (uiState.page == 5) uiState.adapters = SystemDns::adapters();
                        InvalidateRect(hwnd, nullptr, FALSE);
                    }
                    return 0;
                }
            }
            for (auto it = uiState.actions.rbegin(); it != uiState.actions.rend(); ++it) {
                if (PtInRect(&it->r, p)) {
                    onAction(it->kind, it->index);
                    return 0;
                }
            }
            return 0;
        }
        case WM_MOUSEWHEEL: {
            int delta = GET_WHEEL_DELTA_WPARAM(wp);
            int step = delta > 0 ? -1 : 1;
            int &s = uiState.scroll[uiState.page];
            s = std::max(0, std::min(uiState.maxScroll[uiState.page], s + step));
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case WM_NCCALCSIZE:
            if (wp) {
                if (IsZoomed(hwnd)) {
                    auto *p = (NCCALCSIZE_PARAMS *)lp;
                    MONITORINFO mi{};
                    mi.cbSize = sizeof(mi);
                    if (GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi))
                        p->rgrc[0] = mi.rcWork;
                }
                return 0;
            }
            break;
        case WM_NCHITTEST: {
            POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            POINT c = pt;
            ScreenToClient(hwnd, &c);
            RECT rc;
            GetClientRect(hwnd, &rc);
            int b = S(6);
            bool L = c.x < b, R = c.x >= rc.right - b, T = c.y < b, B = c.y >= rc.bottom - b;
            if (T && L) return HTTOPLEFT;
            if (T && R) return HTTOPRIGHT;
            if (B && L) return HTBOTTOMLEFT;
            if (B && R) return HTBOTTOMRIGHT;
            if (L) return HTLEFT;
            if (R) return HTRIGHT;
            if (T) return HTTOP;
            if (B) return HTBOTTOM;
            if (c.y < S(kTitleH)) {
                if (PtInRect(&uiState.btnTheme, c) || PtInRect(&uiState.btnMin, c) ||
                    PtInRect(&uiState.btnMax, c) || PtInRect(&uiState.btnClose, c))
                    return HTCLIENT;
                return HTCAPTION;
            }
            return HTCLIENT;
        }
        case WM_GETMINMAXINFO:
            ((MINMAXINFO *)lp)->ptMinTrackSize.x = S(1120);
            ((MINMAXINFO *)lp)->ptMinTrackSize.y = S(760);
            return 0;
        case WM_QUERYENDSESSION:
            // 系统注销/关机时不再弹“关闭还是最小化”，直接允许结束
            uiState.sessionEnding = true;
            return TRUE;
        case WM_CLOSE:
            requestClose(hwnd);
            return 0;
        case WM_DESTROY:
            KillTimer(hwnd, ID_TIMER);
            util::quitFlag() = true;
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void initFonts() {
    uiState.family = new FontFamily(L"Microsoft YaHei UI");
    if (!uiState.family->IsAvailable()) {
        delete uiState.family;
        uiState.family = new FontFamily(L"Segoe UI");
    }
    uiState.fH1 = new Font(uiState.family, (REAL)S(21), FontStyleRegular, UnitPixel);
    uiState.fH2 = new Font(uiState.family, (REAL)S(15), FontStyleRegular, UnitPixel);
    uiState.fBold = new Font(uiState.family, (REAL)S(14), FontStyleBold, UnitPixel);
    uiState.fBody = new Font(uiState.family, (REAL)S(13), FontStyleRegular, UnitPixel);
    uiState.fSmall = new Font(uiState.family, (REAL)S(12), FontStyleRegular, UnitPixel);
    uiState.fTiny = new Font(uiState.family, (REAL)S(11), FontStyleRegular, UnitPixel);
    uiState.fKpi = new Font(uiState.family, (REAL)S(30), FontStyleRegular, UnitPixel);
    uiState.fMono = new Font(L"Consolas", (REAL)S(12), FontStyleRegular, UnitPixel);
    uiState.editFont = CreateFontW(-S(14), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                             OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                             DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
}

}  // namespace

namespace ui {

int Run(HINSTANCE inst, Context &ctx) {
    GdiplusStartupInput gsi;
    ULONG_PTR token = 0;
    if (GdiplusStartup(&token, &gsi, nullptr) != Ok) return 1;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    uiState.inst = inst;
    uiState.ctx = &ctx;
    uiState.startMs = util::nowMs();
    uiState.cfg = ctx.cfg->snapshot();
    uiState.stats = ctx.stats->view(60);
    uiState.browsers = browsers::scan();
    uiState.adapters = SystemDns::adapters();
    // 旧配置没有 theme 字段时 model 已归一为 light，这里只需区分 dark
    applyTheme(uiState.cfg.ui.theme != "dark");

    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_STANDARD_CLASSES | ICC_LISTVIEW_CLASSES};
    InitCommonControlsEx(&icc);
    uichart::Register(inst);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    wc.hIcon = util::appIcon();
    wc.hbrBackground = nullptr;
    wc.lpszClassName = L"DNSPilotNative";
    if (!RegisterClassExW(&wc)) {
        GdiplusShutdown(token);
        return 1;
    }

    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    int w = 1320, h = 860;
    int workW = work.right - work.left, workH = work.bottom - work.top;
    if (w > workW - 40) w = workW - 40;
    if (h > workH - 40) h = workH - 40;
    int x = work.left + (workW - w) / 2;
    int y = work.top + (workH - h) / 2;
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"DNS Pilot",
                                // WS_CLIPCHILDREN：父窗口整窗重绘时裁掉图表子窗口区域，
                                // 否则每 700ms 刷新会把图表覆盖一帧，表现为持续闪烁
                                WS_POPUP | WS_CLIPCHILDREN | WS_THICKFRAME | WS_MINIMIZEBOX |
                                    WS_MAXIMIZEBOX | WS_SYSMENU,
                                x, y, w, h, nullptr, nullptr, inst, nullptr);
    if (!hwnd) {
        GdiplusShutdown(token);
        return 1;
    }
    uiState.dpi = GetDpiForWindow(hwnd);
    initFonts();
    uiState.chart = uichart::Create(hwnd, ID_CHART);
    applyDwmTheme(hwnd);
    int corner = 2;  // DWMWCP_ROUND
    DwmSetWindowAttribute(hwnd, 33, &corner, sizeof(corner));

    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);
    SetForegroundWindow(hwnd);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (uiState.chart) DestroyWindow(uiState.chart);
    uiState.chart = nullptr;
    delete uiState.fH1;
    delete uiState.fH2;
    delete uiState.fBold;
    delete uiState.fBody;
    delete uiState.fSmall;
    delete uiState.fTiny;
    delete uiState.fKpi;
    delete uiState.fMono;
    delete uiState.family;
    DeleteObject(uiState.editFont);
    DeleteObject(uiState.editBrush);
    DeleteObject(uiState.formBrush);
    GdiplusShutdown(token);
    return 0;
}

}  // namespace ui
