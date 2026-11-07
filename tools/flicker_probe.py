"""Sample the chart region of the main window to detect repaint flicker.

Captures the top-level window (which includes child controls) with
PrintWindow(PW_RENDERFULLCONTENT) several times per second and hashes the
chart rectangle. A stable UI produces one hash; a flickering child usually
produces several (blank / partially painted frames).

    python tools/flicker_probe.py [samples] [interval_ms]
"""
import ctypes
import ctypes.wintypes as wt
import hashlib
import sys
import time

user32 = ctypes.WinDLL("user32", use_last_error=True)
gdi32 = ctypes.WinDLL("gdi32", use_last_error=True)

user32.FindWindowW.restype = wt.HWND
user32.FindWindowW.argtypes = [wt.LPCWSTR, wt.LPCWSTR]
user32.GetDlgItem.argtypes = [wt.HWND, ctypes.c_int]
user32.GetDlgItem.restype = wt.HWND
user32.GetWindowRect.argtypes = [wt.HWND, ctypes.POINTER(wt.RECT)]
user32.GetClientRect.argtypes = [wt.HWND, ctypes.POINTER(wt.RECT)]
user32.ClientToScreen.argtypes = [wt.HWND, ctypes.POINTER(wt.POINT)]
user32.GetWindowDC.restype = wt.HDC
user32.GetWindowDC.argtypes = [wt.HWND]
user32.ReleaseDC.argtypes = [wt.HWND, wt.HDC]
user32.PrintWindow.argtypes = [wt.HWND, wt.HDC, wt.UINT]

gdi32.CreateCompatibleDC.restype = wt.HDC
gdi32.CreateCompatibleDC.argtypes = [wt.HDC]
gdi32.CreateDIBSection.restype = wt.HBITMAP
gdi32.CreateDIBSection.argtypes = [wt.HDC, ctypes.c_void_p, wt.UINT, ctypes.POINTER(ctypes.c_void_p),
                                   wt.HANDLE, wt.DWORD]
gdi32.SelectObject.restype = wt.HGDIOBJ
gdi32.SelectObject.argtypes = [wt.HDC, wt.HGDIOBJ]
gdi32.DeleteObject.argtypes = [wt.HGDIOBJ]
gdi32.DeleteDC.argtypes = [wt.HDC]


class BITMAPINFOHEADER(ctypes.Structure):
    _fields_ = [
        ("biSize", wt.DWORD), ("biWidth", wt.LONG), ("biHeight", wt.LONG),
        ("biPlanes", wt.WORD), ("biBitCount", wt.WORD), ("biCompression", wt.DWORD),
        ("biSizeImage", wt.DWORD), ("biXPelsPerMeter", wt.LONG), ("biYPelsPerMeter", wt.LONG),
        ("biClrUsed", wt.DWORD), ("biClrImportant", wt.DWORD),
    ]


def main(samples, interval_ms):
    parent = user32.FindWindowW("DNSPilotNative", None)
    if not parent:
        raise SystemExit("main window not found")
    chart = user32.GetDlgItem(parent, 100)
    if not chart:
        raise SystemExit("chart control not found")

    wr = wt.RECT()
    user32.GetWindowRect(parent, ctypes.byref(wr))
    cr = wt.RECT()
    user32.GetClientRect(chart, ctypes.byref(cr))
    pt = wt.POINT(0, 0)
    user32.ClientToScreen(chart, ctypes.byref(pt))
    ox, oy = pt.x - wr.left, pt.y - wr.top
    cw, ch = cr.right, cr.bottom
    w, h = wr.right - wr.left, wr.bottom - wr.top

    src = user32.GetWindowDC(parent)
    mem = gdi32.CreateCompatibleDC(src)
    bmi = BITMAPINFOHEADER()
    bmi.biSize = ctypes.sizeof(BITMAPINFOHEADER)
    bmi.biWidth = w
    bmi.biHeight = -h
    bmi.biPlanes = 1
    bmi.biBitCount = 32
    bits = ctypes.c_void_p()
    hbmp = gdi32.CreateDIBSection(src, ctypes.byref(bmi), 0, ctypes.byref(bits), None, 0)
    old = gdi32.SelectObject(mem, hbmp)

    hashes = []
    frames = []
    for i in range(samples):
        user32.PrintWindow(parent, mem, 2)
        data = ctypes.string_at(bits, w * h * 4)
        region = bytearray()
        for y in range(oy, oy + ch):
            off = (y * w + ox) * 4
            region += data[off:off + cw * 4]
        digest = hashlib.sha256(bytes(region)).hexdigest()[:12]
        hashes.append(digest)
        frames.append(bytes(region))
        print("frame %02d %s" % (i, digest))
        time.sleep(interval_ms / 1000.0)

    gdi32.SelectObject(mem, old)
    gdi32.DeleteObject(hbmp)
    gdi32.DeleteDC(mem)
    user32.ReleaseDC(parent, src)

    distinct = sorted(set(hashes))
    print("samples=%d distinct=%d" % (samples, len(distinct)))
    if len(distinct) > 1:
        # 报告变化像素比例，帮助判断是文字刷新还是整块空白
        base = frames[0]
        for i, f in enumerate(frames[1:], 1):
            diff = sum(1 for a, b in zip(base, f) if a != b)
            print("frame %02d differs in %d/%d bytes (%.1f%%)" %
                  (i, diff, len(base), 100.0 * diff / len(base)))


if __name__ == "__main__":
    n = int(sys.argv[1]) if len(sys.argv) > 1 else 40
    ms = int(sys.argv[2]) if len(sys.argv) > 2 else 80
    main(n, ms)
