#!/usr/bin/env python3
"""把 web/index.html 内嵌为 C++ 字符串，生成 src/web_asset.h。"""
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "web", "index.html")
OUT = os.path.join(ROOT, "src", "web_asset.h")


def main() -> int:
    with open(SRC, "r", encoding="utf-8") as f:
        html = f.read()
    if ")DNSHTML" in html:
        print("错误：index.html 包含保留分隔符 ')DNSHTML'", file=sys.stderr)
        return 1
    with open(OUT, "w", encoding="utf-8", newline="\n") as f:
        f.write("// 由 tools/embed.py 生成，请勿手工编辑\n")
        f.write("#pragma once\n\n")
        f.write('static const char *kIndexHtml = R"DNSHTML(')
        f.write(html)
        f.write(')DNSHTML";\n')
    print(f"已生成 {OUT} ({len(html)} 字节)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
