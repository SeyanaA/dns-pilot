"""Print RGB values from a PNG produced by shot.py (filter 0, 8-bit RGB)."""
import struct
import sys
import zlib


def read_png(path):
    data = open(path, "rb").read()
    assert data[:8] == b"\x89PNG\r\n\x1a\n"
    pos = 8
    w = h = 0
    idat = b""
    while pos < len(data):
        ln = struct.unpack(">I", data[pos:pos + 4])[0]
        tag = data[pos + 4:pos + 8]
        body = data[pos + 8:pos + 8 + ln]
        pos += 12 + ln
        if tag == b"IHDR":
            w, h, depth, ctype = struct.unpack(">IIBB", body[:10])
            assert depth == 8 and ctype == 2, "expect 8-bit RGB"
        elif tag == b"IDAT":
            idat += body
        elif tag == b"IEND":
            break
    raw = zlib.decompress(idat)
    stride = w * 3
    rows = []
    for y in range(h):
        off = y * (stride + 1)
        assert raw[off] == 0, "expect filter 0"
        rows.append(raw[off + 1:off + 1 + stride])
    return w, h, rows


if __name__ == "__main__":
    path = sys.argv[1]
    w, h, rows = read_png(path)
    print("size %dx%d" % (w, h))
    x = int(sys.argv[2])
    for arg in sys.argv[3:]:
        y = int(arg)
        row = rows[y]
        r, g, b = row[x * 3], row[x * 3 + 1], row[x * 3 + 2]
        print("y=%d x=%d #%02X%02X%02X" % (y, x, r, g, b))
