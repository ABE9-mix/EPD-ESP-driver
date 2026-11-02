"""模拟固件的双栏布局，渲染成 PNG 预览。

作用：在没有硬件的情况下先看效果、调分栏位置。
绘制逻辑与 epd_layout.cpp 保持一致（同样的坐标、同样的字库）。
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

from PIL import Image, ImageDraw

BASE = Path(__file__).resolve().parent
REPO = BASE.parent
HEADER = REPO / "firmware" / "20_epd_weather_station" / "font_zh12.h"

SCR_W, SCR_H = 212, 104
COL_SPLIT_X = 80
RIGHT_PAD_X = 84
CELL = 12

ENTRY = re.compile(r"\{0x([0-9A-Fa-f]{4}),\s*(\d+),\s*\{([^}]*)\}\}")


def load_glyphs() -> dict[int, tuple[int, list[int]]]:
    """返回 {码位: (字宽, 行位图)}。"""
    text = HEADER.read_text(encoding="utf-8")
    out = {}
    for m in ENTRY.finditer(text):
        cp = int(m.group(1), 16)
        w = int(m.group(2))
        vals = [int(v, 16) for v in m.group(3).split(",") if v.strip()]
        out[cp] = (w, [(vals[i] << 8) | vals[i + 1] for i in range(0, len(vals), 2)])
    return out


G = load_glyphs()


class Canvas:
    def __init__(self):
        self.img = Image.new("L", (SCR_W, SCR_H), 255)   # 白底
        self.d = ImageDraw.Draw(self.img)

    def px(self, x, y):
        if 0 <= x < SCR_W and 0 <= y < SCR_H:
            self.img.putpixel((x, y), 0)

    def fill_rect(self, x, y, w, h):
        for r in range(h):
            for c in range(w):
                self.px(x + c, y + r)

    def rect(self, x, y, w, h):
        for c in range(w):
            self.px(x + c, y); self.px(x + c, y + h - 1)
        for r in range(h):
            self.px(x, y + r); self.px(x + w - 1, y + r)

    def line(self, x1, y1, x2, y2):
        dx, dy = abs(x2 - x1), abs(y2 - y1)
        sx = 1 if x1 < x2 else -1
        sy = 1 if y1 < y2 else -1
        err = dx - dy
        while True:
            self.px(x1, y1)
            if x1 == x2 and y1 == y2:
                break
            e2 = err * 2
            if e2 > -dy:
                err -= dy; x1 += sx
            if e2 < dx:
                err += dx; y1 += sy

    def circle(self, cx, cy, r, fill=False):
        for y in range(cy - r, cy + r + 1):
            for x in range(cx - r, cx + r + 1):
                dd = (x - cx) ** 2 + (y - cy) ** 2
                if fill:
                    if dd <= r * r:
                        self.px(x, y)
                else:
                    inner = (r - 1) ** 2
                    if inner <= dd <= r * r:
                        self.px(x, y)

    def char(self, x, y, cp, scale=1):
        ent = G.get(cp)
        if ent is None:
            self.rect(x + 1, y + 1, CELL - 2, CELL - 2)
            return CELL
        w, rows = ent
        for r, bits in enumerate(rows):
            for c in range(CELL):
                if bits & (1 << (CELL - 1 - c)):
                    if scale == 1:
                        self.px(x + c, y + r)
                    else:
                        self.fill_rect(x + c * scale, y + r * scale, scale, scale)
        return w

    def text(self, x, y, s, scale=1):
        cx = x
        for ch in s:
            adv = self.char(cx, y, ord(ch), scale)
            cx += (adv or CELL) * scale
        return cx - x

    def text_width(self, s, scale=1):
        w = 0
        for ch in s:
            ent = G.get(ord(ch))
            w += (ent[0] if ent else CELL)
        return w * scale


def draw_icon(c: Canvas, cx: int, cy: int, code: int):
    import math
    if code == 113:
        c.circle(cx, cy, 9)
        for a in range(0, 360, 45):
            rad = math.radians(a)
            c.line(cx + int(math.cos(rad) * 11), cy + int(math.sin(rad) * 11),
                   cx + int(math.cos(rad) * 13), cy + int(math.sin(rad) * 13))
        return
    if code in (143, 248, 260):
        for i in range(4):
            c.line(cx - 14, cy - 7 + i * 5, cx + 14, cy - 7 + i * 5)
        return
    snow = 323 <= code <= 338 or 368 <= code <= 371
    rain = (176 <= code <= 200) or (263 <= code <= 308) or (353 <= code <= 359)
    if rain or snow:
        c.circle(cx - 6, cy - 4, 7)
        c.circle(cx + 4, cy - 6, 8)
        c.circle(cx + 11, cy - 2, 6)
        c.line(cx - 13, cy + 1, cx + 15, cy + 1)
        for i in range(4):
            rx = cx - 9 + i * 6
            if snow:
                c.circle(rx, cy + 8, 1, fill=True)
            else:
                c.line(rx, cy + 6, rx - 1, cy + 11)
        return
    c.circle(cx - 6, cy - 2, 7)
    c.circle(cx + 4, cy - 5, 8)
    c.circle(cx + 11, cy, 6)
    c.line(cx - 13, cy + 4, cx + 15, cy + 4)


def render(temp: str, code: int, uv: str, a1: str, a2: str, a3: str,
           forecast: str, out_path: Path):
    c = Canvas()

    LEFT_CX = 40
    # 分隔点线
    for y in range(6, 98, 4):
        c.px(COL_SPLIT_X - 1, y)
        c.px(COL_SPLIT_X - 1, y + 1)

    draw_icon(c, LEFT_CX, 22, code)

    uv_s = f"UV{uv}"
    c.text(LEFT_CX - c.text_width(uv_s) // 2, 46, uv_s)

    # 温度 24px（12px×2），含 °C，水平居中
    t_s = temp if "°" in temp else f"{temp}°C"
    tw = c.text_width(t_s, scale=2)
    tx = max(2, LEFT_CX - tw // 2)
    c.text(tx, 72, t_s, scale=2)

    ry = 12
    for line in (a1, a2):
        if line:
            c.text(RIGHT_PAD_X, ry, line)
            ry += CELL + 10

    if forecast:
        c.line(RIGHT_PAD_X, 68, RIGHT_PAD_X + 112, 68)
        # forecast 已是完整字符串（含"明日"前缀），与固件一致
        c.text(RIGHT_PAD_X, 84, forecast)

    # 加粗边框，模拟墨水屏边界
    img = c.img
    img = img.resize((SCR_W * 3, SCR_H * 3), Image.NEAREST)  # 放大 3 倍便于看
    img.save(out_path)
    print(f"已渲染: {out_path}")


if __name__ == "__main__":
    # 输出到 preview/（已在 .gitignore 中忽略），避免污染仓库
    out_dir = REPO / "preview"
    out_dir.mkdir(exist_ok=True)
    out = out_dir / "preview_scene.png"
    # forecast 传完整字符串（含"明日"前缀与 °C），与固件拼法一致
    # a3 当前布局不使用，传空串
    render("26", 113, "7", "紫外线强", "注意防晒", "", "明日 18~27°C", out)
