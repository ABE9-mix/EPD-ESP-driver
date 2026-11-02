"""模拟固件的双栏布局，渲染成 PNG 预览。

作用：在没有硬件的情况下先看效果、调坐标。
绘制逻辑与 epd_layout.cpp 保持一致（同样的坐标、同样的字库、同样的颜色语义）。

颜色：墨水屏三色 = 白底 / 黑(1) / 红(2)。
预览用 RGB，把红画成接近 EPD 实际的红（偏暗的红）。
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

from PIL import Image, ImageDraw

BASE = Path(__file__).resolve().parent
REPO = BASE.parent
FW = REPO / "firmware" / "20_epd_weather_station"
HEADER = FW / "font_zh12.h"
HEADER_TIME = FW / "font_time10.h"

SCR_W, SCR_H = 212, 104
COL_SPLIT_X = 80
RIGHT_PAD_X = 84
CELL = 12
TIME_CELL = 10

# 三色
WHITE = (255, 255, 255)
BLACK = (0, 0, 0)
RED = (190, 30, 40)          # 接近 EPD Spectra Red 的观感

ENTRY = re.compile(r"\{0x([0-9A-Fa-f]{4}),\s*(\d+),\s*\{([^}]*)\}\}")
TIME_ENTRY = re.compile(r"\{'(.)',\s*\{([^}]*)\}\}")


def load_glyphs():
    """12px 字库：{码位: (字宽, 行位图)}"""
    text = HEADER.read_text(encoding="utf-8")
    out = {}
    for m in ENTRY.finditer(text):
        cp = int(m.group(1), 16)
        w = int(m.group(2))
        vals = [int(v, 16) for v in m.group(3).split(",") if v.strip()]
        out[cp] = (w, [(vals[i] << 8) | vals[i + 1] for i in range(0, len(vals), 2)])
    return out


def load_time_glyphs():
    """10px 时间字库：{字符: 行位图}"""
    text = HEADER_TIME.read_text(encoding="utf-8")
    out = {}
    for m in TIME_ENTRY.finditer(text):
        ch = m.group(1)
        vals = [int(v, 16) for v in m.group(2).split(",") if v.strip()]
        out[ch] = [(vals[i] << 8) | vals[i + 1] for i in range(0, len(vals), 2)]
    return out


G = load_glyphs()
GT = load_time_glyphs()


class Canvas:
    def __init__(self):
        self.img = Image.new("RGB", (SCR_W, SCR_H), WHITE)

    def px(self, x, y, color=BLACK):
        if 0 <= x < SCR_W and 0 <= y < SCR_H:
            self.img.putpixel((x, y), color)

    def fill_rect(self, x, y, w, h, color=BLACK):
        for r in range(h):
            for c in range(w):
                self.px(x + c, y + r, color)

    def rect(self, x, y, w, h, color=BLACK):
        for c in range(w):
            self.px(x + c, y, color); self.px(x + c, y + h - 1, color)
        for r in range(h):
            self.px(x, y + r, color); self.px(x + w - 1, y + r, color)

    def fill_round_rect(self, x, y, w, h, r, color=BLACK):
        """与固件同一算法：四角按 45° 逐行收进。"""
        if w <= 0 or h <= 0:
            return
        r = min(r, w // 2, h // 2)
        for row in range(h):
            d = min(row, h - 1 - row)
            inset = (r - d) if d < r else 0
            span = w - 2 * inset
            if span > 0:
                self.fill_rect(x + inset, y + row, span, 1, color)

    def line(self, x1, y1, x2, y2, color=BLACK):
        dx, dy = abs(x2 - x1), abs(y2 - y1)
        sx = 1 if x1 < x2 else -1
        sy = 1 if y1 < y2 else -1
        err = dx - dy
        while True:
            self.px(x1, y1, color)
            if x1 == x2 and y1 == y2:
                break
            e2 = err * 2
            if e2 > -dy:
                err -= dy; x1 += sx
            if e2 < dx:
                err += dx; y1 += sy

    def circle(self, cx, cy, r, fill=False, color=BLACK):
        for y in range(cy - r, cy + r + 1):
            for x in range(cx - r, cx + r + 1):
                dd = (x - cx) ** 2 + (y - cy) ** 2
                if fill:
                    if dd <= r * r:
                        self.px(x, y, color)
                else:
                    inner = (r - 1) ** 2
                    if inner <= dd <= r * r:
                        self.px(x, y, color)

    def char(self, x, y, cp, scale=1, color=BLACK):
        ent = G.get(cp)
        if ent is None:
            self.rect(x + 1, y + 1, CELL - 2, CELL - 2, color)
            return CELL
        w, rows = ent
        for r, bits in enumerate(rows):
            for c in range(CELL):
                if bits & (1 << (CELL - 1 - c)):
                    if scale == 1:
                        self.px(x + c, y + r, color)
                    else:
                        self.fill_rect(x + c * scale, y + r * scale, scale, scale, color)
        return w

    def text(self, x, y, s, scale=1, color=BLACK):
        cx = x
        for ch in s:
            adv = self.char(cx, y, ord(ch), scale, color)
            cx += (adv or CELL) * scale
        return cx - x

    def text_width(self, s, scale=1):
        w = 0
        for ch in s:
            ent = G.get(ord(ch))
            w += (ent[0] if ent else CELL)
        return w * scale

    def time_text(self, x, y, s, color=BLACK):
        """10px 时间数字，步进 5px。"""
        cx = x
        for ch in s:
            rows = GT.get(ch)
            if rows:
                for r, bits in enumerate(rows):
                    for c in range(TIME_CELL):
                        if bits & (1 << (TIME_CELL - 1 - c)):
                            self.px(cx + c, y + r, color)
            cx += 5
        return cx - x


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
           forecast: str, time_str: str, tlow: str, thigh: str, out_path: Path):
    c = Canvas()
    LEFT_CX = 40

    # 分隔点线
    for y in range(6, 98, 4):
        c.px(COL_SPLIT_X - 1, y)
        c.px(COL_SPLIT_X - 1, y + 1)

    # 右上角：黑底圆角 + 阴刻时间
    if time_str:
        pad_x, box_h = 3, 12
        tw = len(time_str) * 5
        box_w = tw + pad_x * 2
        bx = SCR_W - box_w - 3
        by = 2
        c.fill_round_rect(bx, by, box_w, box_h, 3, BLACK)
        c.time_text(bx + pad_x, by + 1, time_str, WHITE)

    draw_icon(c, LEFT_CX, 18, code)

    uv_s = f"UV{uv}"
    c.text(LEFT_CX - c.text_width(uv_s) // 2, 38, uv_s)

    # 温度 24px
    t_s = temp if "°" in temp else f"{temp}°C"
    tw_ = c.text_width(t_s, scale=2)
    tx = max(2, LEFT_CX - tw_ // 2)
    c.text(tx, 54, t_s, scale=2)

    # 温度区间：低~黑 / 高红 / °C 黑
    if tlow and thigh:
        low_s = f"{tlow}~"
        w_low = c.text_width(low_s)
        w_high = c.text_width(thigh)
        w_unit = c.text_width("°C")
        total = w_low + w_high + w_unit
        rx = max(2, LEFT_CX - total // 2)
        c.text(rx, 82, low_s, color=BLACK)
        c.text(rx + w_low, 82, thigh, color=RED)
        c.text(rx + w_low + w_high, 82, "°C", color=BLACK)

    # 右栏建议
    ry = 20
    for line in (a1, a2):
        if line:
            c.text(RIGHT_PAD_X, ry, line)
            ry += 18

    if forecast:
        c.line(RIGHT_PAD_X, 66, RIGHT_PAD_X + 112, 66)
        c.text(RIGHT_PAD_X, 82, forecast)

    img = c.img.resize((SCR_W * 3, SCR_H * 3), Image.NEAREST)
    img.save(out_path)
    print(f"已渲染: {out_path}")


if __name__ == "__main__":
    out_dir = REPO / "preview"
    out_dir.mkdir(exist_ok=True)

    # 与固件拼法一致：forecast 含"明日"前缀与 °C
    cases = [
        ("preview_scene.png", "18", 113, "4", "天气不错", "适合外出", "明日 13~20°C", "15:20", "13", "20"),
        ("preview_rain.png",  "21", 296, "2", "2小时后有雨", "记得带伞", "明日 16~22°C", "08:05", "16", "22"),
        ("preview_cold.png",  "-3", 326, "0", "低温注意保暖", "适当加衣", "明日 -6~1°C", "23:59", "-6", "1"),
    ]
    for fn, t, code, uv, a1, a2, fc, tm, lo, hi in cases:
        render(t, code, uv, a1, a2, "", fc, tm, lo, hi, out_dir / fn)
