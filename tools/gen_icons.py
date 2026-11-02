"""把 assets/icons/*.svg 点阵化成 ESP32 用的双色图标字库。

流程（用户要求的 SVG → 渲染 → 点阵化）：
    1. 读 assets/icons/*.svg
    2. 内联进一个 HTML，用 Chrome headless 渲染成 PNG
       （每个图标画 300x300，即 30x30 的 10 倍，抗锯齿更平滑）
    3. 按 10x10 块采样回 30x30，分「黑 / 红 / 空白」三态
    4. 输出 firmware/20_epd_weather_station/icons.h

为什么这样做：ESP32 上没有矢量渲染能力，必须预先把图标位图化烧进固件。
用 SVG 做源是为了好改 —— 改完重跑本脚本即可，不用手抠像素。

用法:
    python gen_icons.py
"""

from __future__ import annotations

import subprocess
import sys
import tempfile
from pathlib import Path

from PIL import Image

BASE = Path(__file__).resolve().parent
REPO = BASE.parent
SVG_DIR = REPO / "assets" / "icons"
FW_DIR = REPO / "firmware" / "20_epd_weather_station"
OUT_PATH = FW_DIR / "icons.h"
TMP_PNG = REPO / "preview" / "_icons_raw.png"

CHROME_CANDIDATES = [
    r"C:\Program Files\Google\Chrome\Application\chrome.exe",
    r"C:\Program Files (x86)\Google\Chrome\Application\chrome.exe",
    r"C:\Program Files\Microsoft\Edge\Application\msedge.exe",
]

ICON_W = 30          # 逻辑尺寸（px）
ICON_H = 30
SCALE = 10           # 渲染倍率 → 300x300
CELL = ICON_W * SCALE

# id, 文件名, 说明（顺序即 icons.h 里的索引）
ICONS = [
    (0, "sunny",      "晴"),
    (1, "partly",     "局部多云"),
    (2, "cloudy",     "阴"),
    (3, "fog",        "雾"),
    (4, "rain_light", "小雨"),
    (5, "rain_heavy", "大雨"),
    (6, "snow",       "雪"),
    (7, "thunder",    "雷暴"),
]

# 采样阈值：10x10 块里有多少个同色像素才算实心
PIX_THRESHOLD = 12


def find_chrome() -> str:
    for p in CHROME_CANDIDATES:
        if Path(p).exists():
            return p
    raise SystemExit("找不到 Chrome / Edge，无法渲染 SVG")


def build_html() -> str:
    parts = ['<!DOCTYPE html><html><head><meta charset="utf-8">',
             '<style>html,body{margin:0;padding:0;background:#fff}</style>',
             '</head><body><div style="display:flex">']
    for _id, name, _desc in ICONS:
        svg = (SVG_DIR / f"{name}.svg").read_text(encoding="utf-8")
        # 强制成 300x300
        svg = svg.replace("<svg ", f'<svg width="{CELL}" height="{CELL}" ', 1)
        parts.append(f'<div style="width:{CELL}px;height:{CELL}px;flex:0 0 auto">{svg}</div>')
    parts.append("</div></body></html>")
    return "".join(parts)


def render(html_path: Path, png_path: Path) -> None:
    chrome = find_chrome()
    total_w = CELL * len(ICONS)
    # 用独立 user-data-dir，避免和用户正在跑的 Chrome 冲突
    user_dir = Path(tempfile.gettempdir()) / "epd_icon_render"
    user_dir.mkdir(parents=True, exist_ok=True)
    cmd = [
        chrome, "--headless", "--disable-gpu", "--no-sandbox",
        "--hide-scrollbars", "--force-device-scale-factor=1",
        f"--user-data-dir={user_dir}",
        f"--window-size={total_w},{CELL}",
        f"--screenshot={png_path}",
        html_path.as_uri(),
    ]
    # 注意：不能用 capture_output / PIPE —— 沙箱下捕获子进程 stdio 会 EPERM。
    # DEVNULL 不是管道，可以正常工作。
    subprocess.run(cmd, check=True,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                   timeout=120)


def classify(px: tuple[int, int, int]) -> int:
    """0=空 1=黑 2=红"""
    r, g, b = px[0], px[1], px[2]
    if r > 140 and g < 110 and b < 110:
        return 2
    if r < 110 and g < 110 and b < 110:
        return 1
    return 0


def sample(img: Image.Image, ox: int) -> tuple[list[int], list[int]]:
    """把这块 300x300 采样成 30x30，返回 (black_rows, red_rows)。"""
    black = [0] * ICON_H
    red = [0] * ICON_H
    px = img.load()
    for gy in range(ICON_H):
        for gx in range(ICON_W):
            cnt = {0: 0, 1: 0, 2: 0}
            for y in range(gy * SCALE, (gy + 1) * SCALE):
                for x in range(ox + gx * SCALE, ox + (gx + 1) * SCALE):
                    cnt[classify(px[x, y])] += 1
            bit = 1 << (ICON_W - 1 - gx)
            # 红色优先（两者不重叠，正常不会同时触发）
            if cnt[2] >= PIX_THRESHOLD:
                red[gy] |= bit
            elif cnt[1] >= PIX_THRESHOLD:
                black[gy] |= bit
    return black, red


def main() -> int:
    for _id, name, _d in ICONS:
        if not (SVG_DIR / f"{name}.svg").exists():
            print(f"缺少 SVG: {name}.svg", file=sys.stderr)
            return 1

    with tempfile.TemporaryDirectory() as td:
        html_path = Path(td) / "icons.html"
        html_path.write_text(build_html(), encoding="utf-8")
        TMP_PNG.parent.mkdir(parents=True, exist_ok=True)
        render(html_path, TMP_PNG)

    img = Image.open(TMP_PNG).convert("RGB")
    expect_w = CELL * len(ICONS)
    if img.width < expect_w or img.height < CELL:
        print(f"渲染尺寸异常: {img.size}，期望至少 {expect_w}x{CELL}", file=sys.stderr)
        return 1

    results = []
    for i, (iid, name, desc) in enumerate(ICONS):
        black, red = sample(img, i * CELL)
        results.append((iid, name, desc, black, red))

    # ---- 写 icons.h ----
    total_bytes = len(results) * (4 + ICON_H * 4 * 2)
    L = []
    L.append("// 由 gen_icons.py 自动生成，请勿手改")
    L.append("// 源文件: assets/icons/*.svg（双色面性图标）")
    L.append(f"// 图标数: {len(results)}  单图 {ICON_W}x{ICON_H}  黑/红双层")
    L.append(f"// 数据量: {total_bytes} 字节")
    L.append("#pragma once")
    L.append("#include <Arduino.h>")
    L.append("")
    L.append(f"#define ICON_W {ICON_W}")
    L.append(f"#define ICON_H {ICON_H}")
    L.append(f"#define ICON_COUNT {len(results)}")
    L.append("")
    L.append("struct WeatherIcon {")
    L.append("  uint8_t  id;")
    L.append("  uint32_t black[ICON_H];   // 每行一个 uint32，高位=左边")
    L.append("  uint32_t red[ICON_H];")
    L.append("};")
    L.append("")
    L.append("static const WeatherIcon ICONS[ICON_COUNT] PROGMEM = {")
    for iid, name, desc, black, red in results:
        b = ", ".join(f"0x{v:08X}u" for v in black)
        r = ", ".join(f"0x{v:08X}u" for v in red)
        L.append(f"  // {iid}: {name} — {desc}")
        L.append(f"  {{{iid}, {{{b}}},")
        L.append(f"        {{{r}}}}},")
    L.append("};")
    L.append("")

    OUT_PATH.write_text("\n".join(L), encoding="utf-8")
    print(f"已生成: {OUT_PATH}")
    print(f"图标 {len(results)} 个，数据 {total_bytes} 字节 ({total_bytes/1024:.1f} KB)")
    for iid, name, desc, black, red in results:
        nb = sum(bin(v).count("1") for v in black)
        nr = sum(bin(v).count("1") for v in red)
        print(f"  {iid} {name:<8} 黑 {nb:4d} px  红 {nr:4d} px")
    return 0


if __name__ == "__main__":
    sys.exit(main())
