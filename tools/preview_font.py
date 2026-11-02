"""把生成的字库渲染成 ASCII 预览，肉眼确认字形正确。

用法:
    python preview_font.py "2小时后有雨" "记得带伞"
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

BASE = Path(__file__).resolve().parent
REPO = BASE.parent
HEADER = REPO / "firmware" / "20_epd_weather_station" / "font_zh12.h"

# 格式：{0x4E2D, 12, {0x..,0x.., ...}}  中间那个数字是字宽（ASCII=6 / CJK=12）
ENTRY = re.compile(r"\{0x([0-9A-Fa-f]{4}),\s*(\d+),\s*\{([^}]*)\}\}")


def load() -> dict[int, tuple[int, list[int]]]:
    """返回 {码位: (字宽, 行位图)}。"""
    text = HEADER.read_text(encoding="utf-8")
    out = {}
    for m in ENTRY.finditer(text):
        cp = int(m.group(1), 16)
        w = int(m.group(2))
        vals = [int(v, 16) for v in m.group(3).split(",") if v.strip()]
        rows = [(vals[i] << 8) | vals[i + 1] for i in range(0, len(vals), 2)]
        out[cp] = (w, rows)
    return out


def main(phrases: list[str]) -> int:
    glyphs = load()
    print(f"载入 {len(glyphs)} 个字形\n")

    missing = set()
    for phrase in phrases:
        print(f"[{phrase}]")
        for row in range(12):
            line = []
            for ch in phrase:
                ent = glyphs.get(ord(ch))
                if ent is None:
                    missing.add(ch)
                    line.append("??????")
                    continue
                _w, g = ent
                line.append("".join("#" if (g[row] >> (11 - x)) & 1 else "."
                                    for x in range(12)))
            print("  " + " ".join(line))
        print()

    if missing:
        print("缺少字形:", "".join(sorted(missing)))
        return 1
    return 0


if __name__ == "__main__":
    args = sys.argv[1:] or ["2小时后有雨", "记得带伞", "明日有雨"]
    sys.exit(main(args))
