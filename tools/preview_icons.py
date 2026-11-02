"""把 icons.h 里的点阵数据还原成图片，肉眼核对点阵化是否失真。

这是"所见即所得"的校验：固件里画的就是这些位，所以这张图 = 屏上效果。
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

from PIL import Image

BASE = Path(__file__).resolve().parent
REPO = BASE.parent
HEADER = REPO / "firmware" / "20_epd_weather_station" / "icons.h"
OUT = REPO / "preview" / "icons_bitmap.png"

ICON_W = 30
ICON_H = 30
BLACK = (0, 0, 0)
RED = (190, 30, 40)
WHITE = (255, 255, 255)

# 匹配：{id, {black rows...},\n {red rows...}},
ENTRY = re.compile(
    r"\{(\d+),\s*\{([^}]*)\},\s*\{([^}]*)\}\}", re.S)


def parse_rows(s: str) -> list[int]:
    return [int(v, 16) for v in re.findall(r"0x([0-9A-Fa-f]{8})u", s)]


def main() -> int:
    text = HEADER.read_text(encoding="utf-8")
    entries = []
    for m in ENTRY.finditer(text):
        iid = int(m.group(1))
        black = parse_rows(m.group(2))
        red = parse_rows(m.group(3))
        if len(black) != ICON_H or len(red) != ICON_H:
            print(f"图标 {iid} 行数异常: {len(black)}/{len(red)}", file=sys.stderr)
            return 1
        entries.append((iid, black, red))

    print(f"解析出 {len(entries)} 个图标")

    # 横排，每个留 6px 间距
    gap = 6
    scale = 4          # 放大 4 倍便于看清
    total_w = (ICON_W * len(entries) + gap * (len(entries) - 1)) * scale
    total_h = ICON_H * scale
    img = Image.new("RGB", (total_w, total_h), WHITE)

    for i, (iid, black, red) in enumerate(entries):
        ox = i * (ICON_W + gap) * scale
        for y in range(ICON_H):
            for x in range(ICON_W):
                bit = 1 << (ICON_W - 1 - x)
                color = None
                if red[y] & bit:
                    color = RED
                elif black[y] & bit:
                    color = BLACK
                if color:
                    for dy in range(scale):
                        for dx in range(scale):
                            img.putpixel((ox + x * scale + dx, y * scale + dy), color)

    img.save(OUT)
    print(f"已输出: {OUT}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
