"""Capture a window's DWM surface to PNG without external dependencies.

Uses PrintWindow(PW_RENDERFULLCONTENT) into a 32-bit DIB, so the capture is not
affected by other windows covering ours. PNG is written with zlib from stdlib.

    python tools/shot.py DNSPilotNative out.png
"""
import ctypes
import ctypes.wintypes as wt
import struct
import sys
import time
import zlib

user32 = ctypes.WinDLL("user32", use_last_error=True)
gdi32 = ctypes.WinDLL("gdi32", use_last_error=True)

user32.FindWindowW.restype = wt.HWND
user32.FindWindowW.argtypes = [wt.LPCWSTR, wt.LPCWSTR]
user32.GetWindowRect.argtypes = [wt.HWND, ctypes.POINTER(wt.RECT)]
user32.GetWindowDC.restype = wt.HDC
user32.GetWindowDC.argtypes = [wt.HWND]
user32.ReleaseDC.argtypes = [wt.HWND, wt.HDC]
user32.PrintWindow.argtypes = [wt.HWND, wt.HDC, wt.UINT]
user32.ShowWindow.argtypes = [wt.HWND, ctypes.c_int]
user32.IsIconic.argtypes = [wt.HWND]
user32.SendMessageW.restype = ctypes.c_ssize_t
user32.SendMessageW.argtypes = [wt.HWND, wt.UINT, wt.WPARAM, wt.LPARAM]
user32.GetClientRect.argtypes = [wt.HWND, ctypes.POINTER(wt.RECT)]
user32.ClientToScreen.argtypes = [wt.HWND, ctypes.POINTER(wt.POINT)]
user32.GetDlgItem.argtypes = [wt.HWND, ctypes.c_int]
user32.GetDlgItem.restype = wt.HWND

gdi32.CreateCompatibleDC.restype = wt.HDC
gdi32.CreateCompatibleDC.argtypes = [wt.HDC]
gdi32.CreateDIBSection.restype = wt.HBITMAP
gdi32.CreateDIBSection.argtypes = [wt.HDC, ctypes.c_void_p, wt.UINT, ctypes.POINTER(ctypes.c_void_p),
                                   wt.HANDLE, wt.DWORD]
gdi32.SelectObject.restype = wt.HGDIOBJ
gdi32.SelectObject.argtypes = [wt.HDC, wt.HGDIOBJ]
gdi32.DeleteObject.argtypes = [wt.HGDIOBJ]
gdi32.DeleteDC.argtypes = [wt.HDC]

DIB_RGB_COLORS = 0
PW_RENDERFULLCONTENT = 2


class BITMAPINFOHEADER(ctypes.Structure):
    _fields_ = [
        ("biSize", wt.DWORD), ("biWidth", wt.LONG), ("biHeight", wt.LONG),
        ("biPlanes", wt.WORD), ("biBitCount", wt.WORD), ("biCompression", wt.DWORD),
        ("biSizeImage", wt.DWORD), ("biXPelsPerMeter", wt.LONG), ("biYPelsPerMeter", wt.LONG),
        ("biClrUsed", wt.DWORD), ("biClrImportant", wt.DWORD),
    ]


def png_chunk(tag, data):
    return (struct.pack(">I", len(data)) + tag + data +
            struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))


def write_png(path, width, height, rows):
    raw = b"".join(b"\x00" + row for row in rows)
    ihdr = struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(png_chunk(b"IHDR", ihdr))
        f.write(png_chunk(b"IDAT", zlib.compress(raw, 6)))
        f.write(png_chunk(b"IEND", b""))


def capture(cls, out, flag=PW_RENDERFULLCONTENT, compose=False):
    hwnd = user32.FindWindowW(cls, None)
    if not hwnd and cls.startswith("hwnd:"):
        hwnd = int(cls[5:])
    if not hwnd and cls.startswith("title:"):
        hwnd = user32.FindWindowW(None, cls[6:])
    if not hwnd and "#" in cls:
        # 支持 "父窗口类名#控件ID" 定位子窗口（FindWindow 只能找顶层窗口）
        parent_cls, cid = cls.split("#", 1)
        parent = user32.FindWindowW(parent_cls, None)
        if parent:
            hwnd = user32.GetDlgItem(parent, int(cid))
    if not hwnd:
        raise SystemExit("window not found: " + cls)
    if user32.IsIconic(hwnd):
        user32.ShowWindow(hwnd, 9)  # SW_RESTORE
        time.sleep(0.5)
    rc = wt.RECT()
    user32.GetWindowRect(hwnd, ctypes.byref(rc))
    w = rc.right - rc.left
    h = rc.bottom - rc.top
    if w <= 0 or h <= 0:
        raise SystemExit("bad window rect")

    src = user32.GetWindowDC(hwnd)
    mem = gdi32.CreateCompatibleDC(src)
    bmi = BITMAPINFOHEADER()
    bmi.biSize = ctypes.sizeof(BITMAPINFOHEADER)
    bmi.biWidth = w
    bmi.biHeight = -h  # 自上而下，方便逐行转 PNG
    bmi.biPlanes = 1
    bmi.biBitCount = 32
    bmi.biCompression = 0
    bits = ctypes.c_void_p()
    hbmp = gdi32.CreateDIBSection(src, ctypes.byref(bmi), DIB_RGB_COLORS,
                                  ctypes.byref(bits), None, 0)
    if not hbmp or not bits:
        raise SystemExit("CreateDIBSection failed")
    old = gdi32.SelectObject(mem, hbmp)
    ok = user32.PrintWindow(hwnd, mem, flag)

    rows = None
    if compose:
        # owner-draw 的 ListView 在 PW_RENDERFULLCONTENT 下空白区不擦背景，
        # 这里再用 PrintWindow(0) 渲染一份（会走 WM_ERASEBKGND），只取客户区覆盖。
        cr = wt.RECT()
        user32.GetClientRect(hwnd, ctypes.byref(cr))
        cw = cr.right - cr.left
        ch = cr.bottom - cr.top
        pt = wt.POINT(0, 0)
        user32.ClientToScreen(hwnd, ctypes.byref(pt))
        ox = pt.x - rc.left
        oy = pt.y - rc.top
        mem2 = gdi32.CreateCompatibleDC(src)
        bmi2 = BITMAPINFOHEADER()
        bmi2.biSize = ctypes.sizeof(BITMAPINFOHEADER)
        bmi2.biWidth = w
        bmi2.biHeight = -h
        bmi2.biPlanes = 1
        bmi2.biBitCount = 32
        bits2 = ctypes.c_void_p()
        hbmp2 = gdi32.CreateDIBSection(src, ctypes.byref(bmi2), DIB_RGB_COLORS,
                                       ctypes.byref(bits2), None, 0)
        old2 = gdi32.SelectObject(mem2, hbmp2)
        user32.PrintWindow(hwnd, mem2, 0)
        full = ctypes.string_at(bits2, w * h * 4)
        base = bytearray(ctypes.string_at(bits, w * h * 4))
        stride = w * 4
        for y in range(ch):
            dst = (oy + y) * stride + ox * 4
            srcoff = (oy + y) * stride + ox * 4
            base[dst:dst + cw * 4] = full[srcoff:srcoff + cw * 4]
        gdi32.SelectObject(mem2, old2)
        gdi32.DeleteObject(hbmp2)
        gdi32.DeleteDC(mem2)
        data = bytes(base)
    else:
        data = ctypes.string_at(bits, w * h * 4)

    rows = []
    for y in range(h):
        line = data[y * w * 4:(y + 1) * w * 4]
        rgb = bytearray(w * 3)
        rgb[0::3] = line[2::4]  # B -> R
        rgb[1::3] = line[1::4]
        rgb[2::3] = line[0::4]
        rows.append(bytes(rgb))

    gdi32.SelectObject(mem, old)
    gdi32.DeleteObject(hbmp)
    gdi32.DeleteDC(mem)
    user32.ReleaseDC(hwnd, src)
    write_png(out, w, h, rows)
    print("saved %s %dx%d printwindow=%d" % (out, w, h, 1 if ok else 0))


if __name__ == "__main__":
    if len(sys.argv) < 3:
        raise SystemExit(__doc__)
    flag = int(sys.argv[3]) if len(sys.argv) > 3 and sys.argv[3].isdigit() else PW_RENDERFULLCONTENT
    compose = "compose" in sys.argv[3:]
    capture(sys.argv[1], sys.argv[2], flag, compose)
