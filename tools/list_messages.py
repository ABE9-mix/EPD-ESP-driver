"""统计墨水屏上所有可能出现的提示文字。

用途：确认文案总量、找出「定义了但从未触发」的废弃条目。
"""
from __future__ import annotations

import sys
from pathlib import Path

BASE = Path(__file__).resolve().parent
sys.path.insert(0, str(BASE.parent / "service"))
import advice as A  # noqa: E402


def main() -> int:
    print("=" * 66)
    print("一、左栏天气标签（图标下方）")
    print("=" * 66)
    for i, lab in enumerate(A.WEATHER_LABEL):
        print(f"  {i}  {lab}")

    print()
    print("=" * 66)
    print("二、右栏第 1 行（最重要提醒）候选")
    print("=" * 66)

    row1 = [
        ("雷暴（实况）",     A.PHRASES["thunder"]),
        ("正在下雨（实况）", A.PHRASES["rain_soon"]),
        ("正在下雪（实况）", A.PHRASES["snow_soon"]),
        ("雾霾（实况）",     A.PHRASES["low_vis"]),
    ]
    for n in range(1, 10):
        row1.append((f"雨在 {n} 小时后", A.PHRASES["rain_in"].format(n=n)))
    for n in range(1, 10):
        row1.append((f"雪在 {n} 小时后", A.PHRASES["snow_in"].format(n=n)))
    row1 += [
        ("UV ≥ 8",       A.PHRASES["uv_very"]),
        ("UV 6~7",       A.PHRASES["uv_strong"]),
        ("最高温 ≥ 33",   A.PHRASES["hot"]),
        ("最低温 ≤ 2",    A.PHRASES["cold"]),
        ("风速 ≥ 30",    A.PHRASES["windy"]),
        ("湿度 ≥ 85",    A.PHRASES["wet"]),
        ("湿度 ≤ 25",    A.PHRASES["dry"]),
        ("其他",         A.PHRASES["clear"]),
    ]
    for cond, text in row1:
        print(f"  {cond:<16} → {text}")
    print(f"  小计：{len(row1)} 种")

    print()
    print("=" * 66)
    print("三、右栏第 2 行（配套动作）候选")
    print("=" * 66)
    row2 = [
        ("雷暴",          A.PHRASES["avoid_bolt"]),
        ("正在下雨",       A.PHRASES["umbrella"]),
        ("正在下雪",       A.PHRASES["slippery"]),
        ("雾霾",          A.PHRASES["mask"]),
        ("未来有雨",       A.PHRASES["umbrella"]),
        ("未来有雪",       A.PHRASES["slippery"]),
        ("UV ≥ 6",        A.PHRASES["sunscreen"]),
        ("最高温 ≥ 33",    A.PHRASES["water"]),
        ("最低温 ≤ 2",     A.PHRASES["extra"]),
        ("风速 ≥ 30",     A.PHRASES["windproof"]),
        ("温差 ≥ 12",     A.PHRASES["big_diff"]),
        ("其他",          A.PHRASES["outside"]),
    ]
    shown = []
    for cond, text in row2:
        if text in shown:
            continue
        shown.append(text)
        print(f"  {cond:<16} → {text}")
    print(f"  小计：{len(shown)} 种（去重后）")

    print()
    print("=" * 66)
    print("四、明日预告（第 3 行 / 右下）候选")
    print("=" * 66)
    row3 = [
        ("明日雪",   A.PHRASES["snow_tmrw"]),
        ("明日雨",   A.PHRASES["rain_tmrw"]),
    ]
    for cond, text in row3:
        print(f"  {cond:<10} → {text}")
    print("  另外还有「明日 13~20°C」这种温度格式（数字来自 API，非固定文案）")
    print(f"  小计：{len(row3)} 种纯文案 + 1 种温度格式")

    print()
    print("=" * 66)
    print("五、定义了但从未被触发（死代码）")
    print("=" * 66)
    used = {
        # 降水
        "rain_in", "rain_soon", "rain_tmrw",
        "snow_in", "snow_soon", "snow_tmrw", "slippery",
        # 雾霾 / 雷暴
        "low_vis", "mask", "thunder", "avoid_bolt",
        # 紫外线 / 温度
        "uv_strong", "uv_very", "hot", "cold", "big_diff",
        # 环境 / 通用
        "windy", "wet", "dry", "clear", "outside",
        "umbrella", "sunscreen", "water", "windproof", "extra",
    }
    dead = [k for k in A.PHRASES if k not in used]
    for k in dead:
        print(f"  {k:<14} 「{A.PHRASES[k]}」")
    if not dead:
        print("  无")
    print(f"  小计：{len(dead)} 条从未使用")

    print()
    print("=" * 66)
    print("汇总")
    print("=" * 66)
    total_defined = len(A.PHRASES)
    actual = len(row1) + len(row2) + len(row3) + len(A.WEATHER_LABEL)
    print(f"  PHRASES 里定义的总条数 : {total_defined}")
    print(f"  其中实际会显示的        : {total_defined - len(dead)}")
    print(f"  实际可变输出（含枚举）  : {actual} 种")
    print(f"    ├ 左栏天气标签        : {len(A.WEATHER_LABEL)}")
    print(f"    ├ 第 1 行             : {len(row1)}")
    print(f"    ├ 第 2 行             : {len(row2)}")
    print(f"    └ 明日预告            : {len(row3)}")
    print()
    print(f"  字库字符数            : {len(A.charset())}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
