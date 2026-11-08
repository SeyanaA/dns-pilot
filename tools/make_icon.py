#!/usr/bin/env python3
"""生成 DNS Pilot 的 Windows 图标（assets/dnspilot.ico）。

设计沿用界面左上角品牌标：深色六边形 + 青紫渐变描边 + 白色 D。
运行：python tools/make_icon.py
"""
import os

from PIL import Image, ImageDraw

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "assets")
S = 1024
K = S / 48.0


def pt(x, y):
    return (x * K, y * K)


def cubic(p0, p1, p2, p3, n=40):
    out = []
    for i in range(1, n + 1):
        t = i / n
        u = 1 - t
        x = u**3 * p0[0] + 3 * u**2 * t * p1[0] + 3 * u * t**2 * p2[0] + t**3 * p3[0]
        y = u**3 * p0[1] + 3 * u**2 * t * p1[1] + 3 * u * t**2 * p2[1] + t**3 * p3[1]
        out.append((x, y))
    return out


def lerp(a, b, t):
    return tuple(int(round(a[i] + (b[i] - a[i]) * t)) for i in range(3))


def make():
    os.makedirs(OUT, exist_ok=True)
    base = Image.new("RGBA", (S, S), (0, 0, 0, 0))

    hex_pts = [pt(24, 3.5), pt(41.5, 13.6), pt(41.5, 33.8), pt(24, 44), pt(6.5, 33.8), pt(6.5, 13.6)]

    # 深色底：纵向渐变
    fill = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    fdraw = ImageDraw.Draw(fill)
    top, bottom = (10, 20, 36), (22, 37, 62)
    for y in range(S):
        fdraw.line([(0, y), (S, y)], fill=lerp(top, bottom, y / (S - 1)) + (255,))
    mask = Image.new("L", (S, S), 0)
    ImageDraw.Draw(mask).polygon(hex_pts, fill=255)
    base.paste(fill, (0, 0), mask)

    # 青紫渐变（用于描边和右侧竖条）
    grad = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    gdraw = ImageDraw.Draw(grad)
    c0, c1 = (56, 225, 255), (139, 123, 255)
    for x in range(S):
        gdraw.line([(x, 0), (x, S)], fill=lerp(c0, c1, x / (S - 1)) + (255,))

    border = Image.new("L", (S, S), 0)
    ImageDraw.Draw(border).line(hex_pts + [hex_pts[0]], fill=255, width=58, joint="curve")
    base.paste(grad, (0, 0), border)

    # 白色 D：按品牌 SVG 路径展平贝塞尔曲线
    dpts = [pt(15, 30.5), pt(15, 17.5), pt(20.6, 17.5)]
    dpts += cubic(pt(20.6, 17.5), pt(24.5, 17.5), pt(27, 19.9), pt(27, 24))
    dpts += cubic(pt(27, 24), pt(27, 28.1), pt(24.5, 30.5), pt(20.6, 30.5))
    dpts += [pt(15, 30.5)]
    ImageDraw.Draw(base).polygon(dpts, fill=(230, 237, 247, 255))

    # 右侧竖条（品牌标里的“I”）
    bar = Image.new("L", (S, S), 0)
    ImageDraw.Draw(bar).rounded_rectangle(
        [pt(30.4, 17.5)[0], pt(30.4, 17.5)[1], pt(32.6, 30.5)[0], pt(32.6, 30.5)[1]],
        radius=int(1.1 * K),
        fill=255,
    )
    base.paste(grad, (0, 0), bar)

    # 小尺寸单独加粗 D，避免 16px 下发虚
    sizes = [16, 20, 24, 32, 40, 48, 64, 128, 256]
    images = []
    for size in sizes:
        im = base.resize((size, size), Image.LANCZOS)
        images.append(im)

    ico = os.path.join(OUT, "dnspilot.ico")
    images[-1].save(
        ico,
        format="ICO",
        sizes=[(s, s) for s in sizes],
        append_images=images[:-1],
    )
    images[-1].save(os.path.join(OUT, "dnspilot-256.png"))
    images[3].save(os.path.join(OUT, "dnspilot-32.png"))
    print("written", ico)
    print("previews", os.path.join(OUT, "dnspilot-256.png"), os.path.join(OUT, "dnspilot-32.png"))


if __name__ == "__main__":
    make()
