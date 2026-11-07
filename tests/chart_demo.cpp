// 图表控件演示：720x260 深色窗口，柱状图铺满客户区
#include <windows.h>

#include <cstdint>
#include <vector>

#include "../src/ui_chart.h"

namespace {

const wchar_t *kClass = L"DNSPilotChartDemo";
const wchar_t *kTitle = L"DNS Pilot · Chart Demo";

HWND gChart = nullptr;

void fillFake(std::vector<uint32_t> &data) {
    data.assign(60, 0);
    uint32_t seed = 0x12345678u;
    for (size_t i = 0; i < data.size(); i++) {
        seed = seed * 1664525u + 1013904223u;
        uint32_t v = (seed >> 16) % 11;  // 绝大多数在 0-10
        if (((seed >> 8) & 7u) == 0) v = 0;
        data[i] = v;
    }
    // 几个尖峰，便于观察高度与渐变
    data[7] = 18;
    data[16] = 27;
    data[28] = 41;
    data[39] = 63;
    data[44] = 12;
    data[52] = 34;
    data[58] = 22;
    data[59] = 9;
}

void updateChart(HWND hwnd) {
    RECT rc;
    GetClientRect(hwnd, &rc);
    if (gChart) MoveWindow(gChart, 0, 0, rc.right, rc.bottom, TRUE);
}

LRESULT CALLBACK DemoProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE: {
            gChart = uichart::Create(hwnd, 1);
            std::vector<uint32_t> data;
            fillFake(data);
            if (gChart) uichart::SetData(gChart, data, L"合计 1234 次");
            updateChart(hwnd);
            return 0;
        }
        case WM_SIZE:
            updateChart(hwnd);
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd, &ps);
            RECT rc;
            GetClientRect(hwnd, &rc);
            HBRUSH brush = CreateSolidBrush(RGB(7, 11, 21));
            FillRect(dc, &rc, brush);
            DeleteObject(brush);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_KEYDOWN:
            if (wp == VK_ESCAPE) {
                DestroyWindow(hwnd);
                return 0;
            }
            break;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace

int main() {
    SetProcessDPIAware();
    HINSTANCE inst = GetModuleHandleW(nullptr);
    uichart::Register(inst);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DemoProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));  // IDC_ARROW
    wc.lpszClassName = kClass;
    if (!RegisterClassExW(&wc)) return 1;

    // 客户区固定 720x260，截图裁掉标题栏后正好是图表
    RECT rc{0, 0, 720, 260};
    AdjustWindowRectEx(&rc, WS_OVERLAPPEDWINDOW, FALSE, 0);
    HWND hwnd = CreateWindowExW(0, kClass, kTitle, WS_OVERLAPPEDWINDOW, 80, 80, rc.right - rc.left,
                                rc.bottom - rc.top, nullptr, nullptr, inst, nullptr);
    if (!hwnd) return 1;
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}
