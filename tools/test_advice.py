"""离线测试：用构造的天气数据验证中文建议生成，不依赖网络。"""
from __future__ import annotations

import json
import sys
from pathlib import Path

BASE = Path(__file__).resolve().parent
REPO = BASE.parent
# advice.py 与 epd_sync.py 都在 service/ 下
sys.path.insert(0, str(REPO / "service"))

import advice as A  # noqa: E402
import epd_sync as S  # noqa: E402

FAIL = 0
PASS = 0


def check(name: str, cond: bool, extra: str = "") -> None:
    global PASS, FAIL
    if cond:
        PASS += 1
        print(f"  OK   {name}")
    else:
        FAIL += 1
        print(f"  FAIL {name}  {extra}")


def wx(cur_code=0, uv=0, temp=20, high=25, low=15, hum=50, wind=5,
       hourly_codes=None, hourly_probs=None):
    """构造一个 Open-Meteo 风格的响应。"""
    return {
        "current": {
            "temperature_2m": temp,
            "apparent_temperature": temp,
            "relative_humidity_2m": hum,
            "weather_code": cur_code,
            "uv_index": uv,
            "wind_speed_10m": wind,
        },
        "hourly": {
            "time": [],          # 留空 -> _rain_in_hours 退化处理
            "weather_code": hourly_codes or [],
            "precipitation_probability": hourly_probs or [],
        },
        "daily": {
            "time": ["2026-11-01", "2026-11-02"],
            "temperature_2m_max": [high, high - 2],
            "temperature_2m_min": [low, low - 1],
            "weather_code": [cur_code, 61],
        },
    }


print("== 1. 建议生成：各场景 ==")
cases = [
    ("晴天 UV 低", dict(uv=1), ["天气不错", "适合外出"]),
    ("UV 强", dict(uv=7), ["紫外线强", "注意防晒"]),
    ("UV 很强", dict(uv=9), ["紫外线很强", "注意防晒"]),
    ("高温", dict(uv=1, high=35), ["高温注意补水", "注意补水"]),
    ("低温", dict(uv=1, low=1), ["低温注意保暖", "适当加衣"]),
    ("大风", dict(uv=1, wind=40), ["风力较大", "注意防风"]),
    ("高湿", dict(uv=1, hum=90), ["湿度较大", "适合外出"]),
    ("干燥", dict(uv=1, hum=20), ["空气干燥", "适合外出"]),
]
for name, kw, expect in cases:
    lines = A.build_advice({
        "rain_in_hours": None, "uv": kw.get("uv", 0),
        "temp": kw.get("temp", 20), "thigh": kw.get("high", 25),
        "tlow": kw.get("low", 15), "hum": kw.get("hum", 50),
        "wind": kw.get("wind", 5), "tmrw_code": None,
    })
    ok = lines[:2] == expect
    check(f"{name} -> {lines[:2]}", ok, f"期望 {expect}")

print("\n== 2. 降雨倒计时 ==")
for n in range(1, 10):
    lines = A.build_advice({"rain_in_hours": n, "uv": 0, "temp": 20,
                            "thigh": 25, "tlow": 15, "hum": 50, "wind": 0})
    check(f"{n}小时后有雨", lines[0] == f"{n}小时后有雨", lines[0])
lines = A.build_advice({"rain_in_hours": 0, "uv": 0, "temp": 20,
                        "thigh": 25, "tlow": 15, "hum": 50, "wind": 0})
check("正在下雨 -> 正在下雨", lines[0] == "正在下雨", lines[0])
# 回归防护：字体 zh_hans 子集里「即」是缺字方框，任何文案都不许用它
check("文案不含缺字「即」", "即" not in "".join(A.PHRASES.values()),
      [t for t in A.PHRASES.values() if "即" in t])

print("\n== 3. 明日预告 ==")
lines = A.build_advice({"rain_in_hours": None, "uv": 1, "temp": 20, "thigh": 25,
                        "tlow": 15, "hum": 50, "wind": 0, "tmrw_code": 61})
check("明日有雨", "明日有雨" in lines, str(lines))
lines = A.build_advice({"rain_in_hours": None, "uv": 1, "temp": 20, "thigh": 25,
                        "tlow": 15, "hum": 50, "wind": 0, "tmrw_code": 73})
check("明日有雪", "明日有雪" in lines, str(lines))

print("\n== 4. 每行不超过 10 个字符 ==")
all_tpl = list(A.PHRASES.values())
check("所有模板 <=10 字", all(len(t) <= 10 for t in all_tpl),
      str([t for t in all_tpl if len(t) > 10]))
check("所有模板 <= 120px", all(len(t) * 12 <= 120 for t in all_tpl))

print("\n== 5. 字表覆盖检查 ==")
cs = set(A.charset())
used = set()
for t in A.PHRASES.values():
    # 模板占位符 {n} 换成数字
    for ch in t.replace("{n}", "").replace("{r}", ""):
        used.add(ch)
used.update("0123456789")
used.update(A.FORECAST_PREFIX)
used.update(" °C~")
missing = used - cs
check("模板用字都在字表内", not missing, f"缺: {missing}")

print("\n== 6. build_payload 集成 ==")
p = S.build_payload(wx(uv=7, high=30, low=18))
check("含 a1 字段", "a1" in p, str(p))
check("含 a2 字段", "a2" in p, str(p))
check("a1 是中文", any(ord(c) > 127 for c in p.get("a1", "")), p.get("a1"))
check("含 epoch", "epoch" in p)
print(f"   payload = {json.dumps(p, ensure_ascii=False)}")
payload_bytes = len(json.dumps(p, ensure_ascii=False,
                               separators=(",", ":")).encode("utf-8"))
print(f"   字节数 = {payload_bytes}")
check("载荷 < 480 字节（BLE MTU 内）", payload_bytes < 480, str(payload_bytes))

print("\n== 7. payload 里的所有非 ASCII 字符都在字表内 ==")
text = json.dumps(p, ensure_ascii=False)
missing = [c for c in text if ord(c) > 127 and c not in cs]
check("无缺字", not missing, f"缺: {set(missing)}")

print(f"\n===== {PASS} 通过 / {FAIL} 失败 =====")
sys.exit(1 if FAIL else 0)
