"""Small Win32 UI probe used by the manual/automated theme checks.

Only uses ctypes + user32, so it works on a stock Python install. Input is sent
with real window messages (WM_CHAR for EDIT text) because SetWindowText does
not update the internal text of a cross-process EDIT control.

Examples:
    python tools/ui_probe.py info DNSPilotNative
    python tools/ui_probe.py click DNSPilotNative 1159 26
    python tools/ui_probe.py children DNSPilotForm
    python tools/ui_probe.py typeedit DNSPilotForm 0 Hello
    python tools/ui_probe.py key DNSPilotForm 27
"""
import ctypes
import ctypes.wintypes as wt
import sys

import time

user32 = ctypes.WinDLL("user32", use_last_error=True)
user32.FindWindowW.restype = wt.HWND
user32.FindWindowW.argtypes = [wt.LPCWSTR, wt.LPCWSTR]
user32.SendMessageW.restype = ctypes.c_ssize_t
user32.SendMessageW.argtypes = [wt.HWND, wt.UINT, wt.WPARAM, wt.LPARAM]
user32.GetClientRect.argtypes = [wt.HWND, ctypes.POINTER(wt.RECT)]
user32.GetWindowRect.argtypes = [wt.HWND, ctypes.POINTER(wt.RECT)]
user32.GetWindowTextW.argtypes = [wt.HWND, wt.LPWSTR, ctypes.c_int]
user32.GetClassNameW.argtypes = [wt.HWND, wt.LPWSTR, ctypes.c_int]
user32.GetDlgCtrlID.argtypes = [wt.HWND]
user32.GetDpiForWindow.argtypes = [wt.HWND]
user32.GetWindowLongPtrW.argtypes = [wt.HWND, ctypes.c_int]
user32.GetDlgItem.argtypes = [wt.HWND, ctypes.c_int]
user32.GetDlgItem.restype = wt.HWND

WM_LBUTTONDOWN = 0x0201
WM_LBUTTONUP = 0x0202
WM_CHAR = 0x0102
WM_KEYDOWN = 0x0100
WM_KEYUP = 0x0101
WM_CLOSE = 0x0010

GW_CHILD = 5
GW_HWNDNEXT = 2


def find(cls, title=None):
    hwnd = user32.FindWindowW(cls, title)
    if not hwnd:
        fail("window not found: class=%s title=%s" % (cls, title))
    return hwnd


def fail(msg):
    sys.stderr.write(msg + "\n")
    sys.exit(1)


def class_name(hwnd):
    buf = ctypes.create_unicode_buffer(256)
    user32.GetClassNameW(hwnd, buf, 256)
    return buf.value


def text_of(hwnd):
    n = user32.SendMessageW(hwnd, 0x000E, 0, 0)  # WM_GETTEXTLENGTH
    buf = ctypes.create_unicode_buffer(int(n) + 1)
    # 跨进程 WM_GETTEXT 由系统编组，lParam 传本进程缓冲区的地址即可
    user32.SendMessageW(hwnd, 0x000D, n + 1, ctypes.addressof(buf))
    return buf.value


def children(hwnd):
    out = []
    child = user32.GetWindow(hwnd, GW_CHILD)
    while child:
        out.append(child)
        child = user32.GetWindow(child, GW_HWNDNEXT)
    return out


def dpi_of(hwnd):
    try:
        d = user32.GetDpiForWindow(hwnd)
        return d or 96
    except Exception:
        return 96


def rect_of(hwnd, client=False):
    r = wt.RECT()
    if client:
        user32.GetClientRect(hwnd, ctypes.byref(r))
    else:
        user32.GetWindowRect(hwnd, ctypes.byref(r))
    return r


def cmd_info(cls):
    hwnd = find(cls)
    wr = rect_of(hwnd)
    cr = rect_of(hwnd, True)
    print("hwnd=%d class=%s dpi=%d window=%d,%d,%d,%d client=%dx%d" % (
        hwnd, class_name(hwnd), dpi_of(hwnd),
        wr.left, wr.top, wr.right, wr.bottom, cr.right - cr.left, cr.bottom - cr.top))


def cmd_children(cls):
    hwnd = find(cls)
    for c in children(hwnd):
        r = rect_of(c, True)
        tid = user32.GetDlgCtrlID(c)
        print("hwnd=%d id=%d class=%s rect=%d,%d,%d,%d text=%r" % (
            c, tid, class_name(c), r.left, r.top, r.right, r.bottom, text_of(c)))


def ensure_visible(hwnd):
    """最小化时先恢复：最小化窗口不处理 WM_PAINT，命中区会停留在旧页面。"""
    if user32.IsIconic(hwnd):
        user32.ShowWindow(hwnd, 9)  # SW_RESTORE
        time.sleep(0.35)


def cmd_click(cls, x, y, post=False):
    hwnd = find(cls)
    ensure_visible(hwnd)
    lparam = (int(y) << 16) | (int(x) & 0xFFFF)
    send = user32.PostMessageW if post else user32.SendMessageW
    if post:
        send.argtypes = [wt.HWND, wt.UINT, wt.WPARAM, wt.LPARAM]
    send(hwnd, WM_LBUTTONDOWN, 1, lparam)
    send(hwnd, WM_LBUTTONUP, 0, lparam)
    print("clicked %d,%d on %s%s" % (int(x), int(y), cls, " (post)" if post else ""))


def cmd_type(cls, index, value, select_all=False):
    hwnd = find(cls)
    ensure_visible(hwnd)
    edits = [c for c in children(hwnd) if class_name(c).upper() == "EDIT"]
    if int(index) >= len(edits):
        fail("only %d EDIT controls" % len(edits))
    target = edits[int(index)]
    if select_all:
        user32.SendMessageW(target, 0x00B1, 0, -1)  # EM_SETSEL(0, -1)
    # 关键：跨进程 EDIT 必须逐字符 WM_CHAR，内部文本才会真正改变
    for ch in value:
        user32.SendMessageW(target, WM_CHAR, ord(ch), 1)
    print("typed %r into edit[%d]%s" % (value, int(index), " (replaced)" if select_all else ""))


def cmd_key(cls, vk):
    hwnd = find(cls)
    user32.SendMessageW(hwnd, WM_KEYDOWN, int(vk), 1)
    user32.SendMessageW(hwnd, WM_KEYUP, int(vk), 0xC0000001)
    print("key %d -> %s" % (int(vk), cls))


def cmd_close(cls, post=False):
    hwnd = find(cls)
    if post:
        user32.PostMessageW(hwnd, WM_CLOSE, 0, 0)
    else:
        user32.SendMessageW(hwnd, WM_CLOSE, 0, 0)
    print("closed %s%s" % (cls, " (post)" if post else ""))


def cmd_wheel(cls, delta, times=1):
    hwnd = find(cls)
    # WM_MOUSEWHEEL: wParam 高 16 位是有符号滚动量，lParam 为屏幕坐标（这里用 0,0 即可）
    wparam = (int(delta) & 0xFFFF) << 16
    for _ in range(int(times)):
        user32.SendMessageW(hwnd, 0x020A, wparam, 0)
    print("wheel %d x%d on %s" % (int(delta), int(times), cls))


def cmd_btn(cls, ctrl_id, post=True):
    hwnd = find(cls)
    ctrl = user32.GetDlgItem(hwnd, int(ctrl_id))
    if not ctrl:
        fail("control %s not found" % ctrl_id)
    if post:
        user32.PostMessageW(ctrl, 0x00F5, 0, 0)  # BM_CLICK
    else:
        user32.SendMessageW(ctrl, 0x00F5, 0, 0)
    print("clicked button %s in %s" % (ctrl_id, cls))


def cmd_pidwindows(pid):
    want = int(pid)
    found = []

    @ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)
    def cb(hwnd, _lp):
        p = wt.DWORD()
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(p))
        if p.value == want:
            found.append(hwnd)
        return True

    user32.EnumWindows(cb, 0)
    for hwnd in found:
        print("hwnd=%d title=%r class=%s" % (hwnd, text_of(hwnd), class_name(hwnd)))


def cmd_child(cls, ctrl_id):
    hwnd = find(cls)
    ctrl = user32.GetDlgItem(hwnd, int(ctrl_id))
    if not ctrl:
        fail("control %s not found" % ctrl_id)
    wr = rect_of(ctrl)
    cr = rect_of(ctrl, True)
    print("hwnd=%d class=%s window=%dx%d client=%dx%d" % (
        ctrl, class_name(ctrl), wr.right - wr.left, wr.bottom - wr.top, cr.right, cr.bottom))


def cmd_cclick(cls, ctrl_id, x, y):
    hwnd = find(cls)
    ctrl = user32.GetDlgItem(hwnd, int(ctrl_id))
    if not ctrl:
        fail("control %s not found" % ctrl_id)
    lparam = (int(y) << 16) | (int(x) & 0xFFFF)
    user32.PostMessageW(ctrl, WM_LBUTTONDOWN, 1, lparam)
    user32.PostMessageW(ctrl, WM_LBUTTONUP, 0, lparam)
    print("clicked child %s at %d,%d in %s" % (ctrl_id, int(x), int(y), cls))


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 1
    cmd = argv[1]
    if cmd == "info":
        cmd_info(argv[2])
    elif cmd == "children":
        cmd_children(argv[2])
    elif cmd == "click":
        cmd_click(argv[2], argv[3], argv[4])
    elif cmd == "pclick":
        cmd_click(argv[2], argv[3], argv[4], post=True)
    elif cmd == "type":
        cmd_type(argv[2], argv[3], argv[4])
    elif cmd == "typeall":
        cmd_type(argv[2], argv[3], argv[4], select_all=True)
    elif cmd == "key":
        cmd_key(argv[2], argv[3])
    elif cmd == "wheel":
        cmd_wheel(argv[2], argv[3], argv[4] if len(argv) > 4 else 1)
    elif cmd == "close":
        cmd_close(argv[2])
    elif cmd == "pclose":
        cmd_close(argv[2], post=True)
    elif cmd == "btn":
        cmd_btn(argv[2], argv[3])
    elif cmd == "cclick":
        cmd_cclick(argv[2], argv[3], argv[4], argv[5])
    elif cmd == "child":
        cmd_child(argv[2], argv[3])
    elif cmd == "pidwindows":
        cmd_pidwindows(argv[2])
    else:
        print(__doc__)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
