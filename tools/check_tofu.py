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
FONT10_PATH = REPO / "assets" / "ark10" / "ark-pixel-10px-monospaced-latin.ms.bitmap.ttf"
sys.path.insert(0, str(REPO / "service"))
import advice  # noqa: E402

CELL = 12
TIME_CELL = 10

# 标准 tofu：上下边框 + 左右竖边
TOFU = (
    [0xFFF] + [0x801] * 10 + [0xFFF]
)


def rows_of(font, ch, y_off=0, cell: int = CELL):
    img = Image.new("1", (cell, cell), 0)
    d = ImageDraw.Draw(img)
    bbox = d.textbbox((0, 0), ch, font=font)
    oy = 0
    if bbox:
        h = bbox[3] - bbox[1]
        oy = max(0, (cell - h) // 2) - bbox[1]
    d.text((0, oy), ch, font=font, fill=1)
    out = []
    for y in range(cell):
        bits = 0
        for x in range(cell):
            if img.getpixel((x, y)):
                bits |= 1 << (cell - 1 - x)
        out.append(bits)
    return out


def is_tofu(rows) -> bool:
    return rows == TOFU


def check_time_font() -> int:
    """10px 时间字库（0-9 与冒号）同样要防缺字。"""
    if not FONT10_PATH.exists():
        print("10px 字体不存在，跳过时间字库检查")
        return 0
    font = ImageFont.truetype(str(FONT10_PATH), TIME_CELL)
    # 10px 的 tofu 边框也是 10x10
    tofu10 = [0x3FF] + [0x201] * 8 + [0x3FF]
    bad = []
    for ch in "0123456789:":
        rows = rows_of(font, ch, cell=TIME_CELL)
        if rows == tofu10:
            bad.append(ch)
    print(f"\n10px 时间字库检查 {len('0123456789:')} 个字符")
    if bad:
        print(f"发现缺字方框: {''.join(bad)}")
        return 1
    print("全部正常")
    return 0


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
    rc = 0
    if bad:
        print(f"发现 {len(bad)} 个缺字方框: {''.join(bad)}")
        for ch in bad:
            print(f"  U+{ord(ch):04X}  {ch}")
        rc = 1
    else:
        print("全部正常，无缺字")

    return check_time_font() or rc


if __name__ == "__main__":
    sys.exit(main())
