"""把方舟像素 12px 字体生成 ESP32 用的点阵字库（C 数组）。

为什么必须这么做：
    ESP32 没有系统字体，墨水屏上要显示中文只能预先点阵化、烧进固件。
    本脚本直接读取 advice.py 的字符表，保证"生成的字库"恰好覆盖
    "程序会说的话"，不会出现方框。

输出：
    font_zh12.h  —— 字库数据（PROGMEM）
用法：
    python gen_font.py
"""

from __future__ import annotations

import sys
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

# 目录约定（本仓库布局）：
#   tools/     <- 本脚本
#   service/   <- advice.py（文案表，也是字表的唯一真相来源）
#   assets/    <- 方舟像素字体
#   firmware/20_epd_weather_station/  <- 生成物落点
BASE = Path(__file__).resolve().parent
REPO = BASE.parent
FONT_PATH = REPO / "assets" / "ark12" / "ark-pixel-12px-monospaced-zh_hans.ms.bitmap.ttf"
FW_DIR = REPO / "firmware" / "20_epd_weather_station"
OUT_PATH = FW_DIR / "font_zh12.h"

# 12px 方舟像素：汉字 12x12，ASCII 半宽 6x10（等宽版英文是整个 12 格里的左 6 格）
CELL_W = 12
CELL_H = 12
FONT_SIZE = 12

# advice.py 在 service/ 下，两个目录都要能 import
sys.path.insert(0, str(REPO / "service"))
import advice  # noqa: E402


def char_width(font: ImageFont.FreeTypeFont, ch: str) -> int:
    """等宽像素字体里：ASCII 半角 6px，CJK 全角 12px。"""
    try:
        adv = font.getlength(ch)
    except Exception:
        adv = CELL_W
    return 6 if adv <= CELL_W / 2 + 0.5 else CELL_W


def render_char(font: ImageFont.FreeTypeFont, ch: str) -> tuple[int, list[int]]:
    """渲染单字符，返回 (字宽, 行位图)。

    字宽很关键：ASCII 是 6px、汉字 12px。若一律按 12px 推进，
    英文就会出现「U V 7」这种被拉散的观感。
    """
    w = char_width(font, ch)
    img = Image.new("1", (CELL_W, CELL_H), 0)
    d = ImageDraw.Draw(img)

    bbox = d.textbbox((0, 0), ch, font=font)
    oy = 0
    if bbox:
        h = bbox[3] - bbox[1]
        oy = max(0, (CELL_H - h) // 2) - bbox[1]
    d.text((0, oy), ch, font=font, fill=1)

    rows = []
    for y in range(CELL_H):
        bits = 0
        for x in range(CELL_W):
            if img.getpixel((x, y)):
                bits |= 1 << (CELL_W - 1 - x)
        rows.append(bits)
    return w, rows


def main() -> int:
    if not FONT_PATH.exists():
        print(f"字体不存在: {FONT_PATH}", file=sys.stderr)
        return 1
    if not FW_DIR.exists():
        print(f"固件目录不存在: {FW_DIR}", file=sys.stderr)
        return 1

    font = ImageFont.truetype(str(FONT_PATH), FONT_SIZE)
    chars = advice.charset()

    # 只保留实际能渲染出内容的字符（空格也要保留，用于断词）
    entries = []
    for ch in chars:
        if ch == "\n":
            continue
        w, rows = render_char(font, ch)
        entries.append((ch, w, rows))

    # ---- 写头文件 ----
    total_bytes = len(entries) * CELL_H * 2
    lines = []
    lines.append("// 由 gen_font.py 自动生成，请勿手改")
    lines.append(f"// 字体: 方舟像素 12px (Ark Pixel Font, OFL-1.1)")
    lines.append(f"// 字符数: {len(entries)}  单字 {CELL_W}x{CELL_H} ({CELL_H * 2} 字节)")
    lines.append(f"// 数据总量: {total_bytes} 字节")
    lines.append("#pragma once")
    lines.append("#include <Arduino.h>")
    lines.append("")
    lines.append(f"#define ZH_CELL_W {CELL_W}")
    lines.append(f"#define ZH_CELL_H {CELL_H}")
    lines.append(f"#define ZH_GLYPH_BYTES {CELL_H * 2}")
    lines.append(f"#define ZH_COUNT {len(entries)}")
    lines.append("")
    lines.append("struct ZhGlyph {")
    lines.append("  uint32_t cp;    // Unicode 码位")
    lines.append("  uint8_t  w;     // 字宽：ASCII=6, CJK=12")
    lines.append("  uint8_t  bits[ZH_GLYPH_BYTES];  // 每行 2 字节，高位在前")
    lines.append("};")
    lines.append("")
    lines.append("static const ZhGlyph ZH_GLYPHS[ZH_COUNT] PROGMEM = {")
    for ch, w, rows in entries:
        cp = ord(ch)
        # 每行一个 16 位值，拆成高字节在前、低字节在后
        bs = ",".join(f"0x{(r >> 8) & 0xFF:02X},0x{r & 0xFF:02X}" for r in rows)
        lines.append(f"  {{0x{cp:04X}, {w:2d}, {{{bs}}}}},   // {ch if ch != ' ' else '(space)'}")
    lines.append("};")
    lines.append("")
    lines.append("// 全角/半角：按字宽推进光标，避免英文被拉散")
    lines.append("static inline uint8_t zhGlyphWidth(uint32_t cp) {")
    lines.append("  for (int i = 0; i < ZH_COUNT; i++) {")
    lines.append("    if (pgm_read_dword(&ZH_GLYPHS[i].cp) == cp) {")
    lines.append("      return pgm_read_byte(&ZH_GLYPHS[i].w);")
    lines.append("    }")
    lines.append("  }")
    lines.append("  return ZH_CELL_W;")
    lines.append("}")
    lines.append("")

    OUT_PATH.write_text("\n".join(lines), encoding="utf-8")
    print(f"已生成: {OUT_PATH}")
    print(f"字符数: {len(entries)}  数据: {total_bytes} 字节 ({total_bytes/1024:.1f} KB)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
