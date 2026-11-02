"""检查字库里哪些字符是「缺字方框」(tofu)。

方舟像素字体缺字时会渲染成 12x12 的空心方框：
    第一行和最后一行全亮，中间每行只有首尾两列亮。
这种字形上屏后是一堆方框，必须提前发现。
"""
from __future__ import annotations

import sys
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

BASE = Path(__file__).resolve().parent
REPO = BASE.parent
FONT_PATH = REPO / "assets" / "ark12" / "ark-pixel-12px-monospaced-zh_hans.ms.bitmap.ttf"
sys.path.insert(0, str(REPO / "service"))
import advice  # noqa: E402

CELL = 12

# 标准 tofu：上下边框 + 左右竖边
TOFU = (
    [0xFFF] + [0x801] * 10 + [0xFFF]
)


def rows_of(font, ch, y_off=0):
    img = Image.new("1", (CELL, CELL), 0)
    d = ImageDraw.Draw(img)
    bbox = d.textbbox((0, 0), ch, font=font)
    oy = 0
    if bbox:
        h = bbox[3] - bbox[1]
        oy = max(0, (CELL - h) // 2) - bbox[1]
    d.text((0, oy), ch, font=font, fill=1)
    out = []
    for y in range(CELL):
        bits = 0
        for x in range(CELL):
            if img.getpixel((x, y)):
                bits |= 1 << (CELL - 1 - x)
        out.append(bits)
    return out


def is_tofu(rows) -> bool:
    return rows == TOFU


def main() -> int:
    font = ImageFont.truetype(str(FONT_PATH), CELL)
    chars = advice.charset()
    bad = []
    for ch in chars:
        if ch == " ":
            continue
        rows = rows_of(font, ch)
        if is_tofu(rows):
            bad.append(ch)

    print(f"检查 {len(chars)} 个字符")
    if bad:
        print(f"发现 {len(bad)} 个缺字方框: {''.join(bad)}")
        for ch in bad:
            print(f"  U+{ord(ch):04X}  {ch}")
        return 1
    print("全部正常，无缺字")
    return 0


if __name__ == "__main__":
    sys.exit(main())
