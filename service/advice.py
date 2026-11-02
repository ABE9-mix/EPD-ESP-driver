"""天气建议生成 —— 中文字句（墨水屏右侧显示）

设计约束（重要）：
    墨水屏左侧显示英文（温度/UV），右侧显示中文建议。
    但 ESP32 没有系统字体，中文必须预先点阵化烧进固件。
    因此**所有可能出现的汉字必须限定在本文件的字表内**，
    且字体生成脚本会直接读取本文件的 PHRASES 来生产字库，
    保证"生成的字库"与"程序说的话"永远一致，不会出现方框。

    单行最多 10 个汉字（右侧区域宽 128px，12px 字宽 = 120px）。
"""

from __future__ import annotations

from typing import List

# ---------------------------------------------------------------- 字句模板
# 每条都用「短语片段」拼装，片段可以复用，减少字表体积
PHRASES = {
    # 下雨
    "rain_in":      "{n}小时后有雨",     # n 为 1~9
    # 注意：不要用「即」——方舟像素 12px 的 zh_hans 子集里该码位是缺字方框
    "rain_soon":    "正在下雨",
    "rain_today":   "今日有雨",
    "rain_tmrw":    "明日有雨",
    "snow_tmrw":    "明日有雪",
    # 紫外线
    "uv_strong":    "紫外线强",
    "uv_very":      "紫外线很强",
    # 温度
    "hot":          "高温注意补水",
    "cold":         "低温注意保暖",
    "warm":         "气温舒适",
    "big_diff":     "温差大注意加衣",
    # 其他
    "windy":        "风力较大",
    "wet":          "湿度较大",
    "dry":          "空气干燥",
    "clear":        "天气不错",
    "outside":      "适合外出",
    "umbrella":     "记得带伞",
    "sunscreen":    "注意防晒",
    "sunglasses":   "出门戴墨镜",
    "water":        "注意补水",
    "windproof":    "注意防风",
    "extra":        "适当加衣",
    "stay_in":      "减少外出",
    "calm":         "风平浪静",
}

# 预报行前缀
FORECAST_PREFIX = "明日"
# 连接符（ASCII，已在字库内）
DASH = "~"


def _chars() -> set:
    """收集所有会用到的字符（含数字 0-9、ASCII 常用符号）。"""
    s = set()
    for tpl in PHRASES.values():
        s.update(tpl)
    s.update(FORECAST_PREFIX)
    s.update(DASH)
    s.update("0123456789")
    s.update("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz")
    s.update(" °C%~|:+-./")
    # 模板里的 {n} {r} 只是占位，去掉
    s.discard("{")
    s.discard("}")
    return {c for c in s if c.strip() or c == " "}


def charset() -> List[str]:
    """给字体生成脚本用：返回排序后的字符列表。"""
    return sorted(_chars())


# ---------------------------------------------------------------- 业务逻辑

def _fmt(tpl: str, **kw) -> str:
    return tpl.format(**kw)


def build_advice(wx: dict, now_epoch: float | None = None) -> List[str]:
    """根据天气数据生成最多 3 行建议，每行 ≤10 个汉字。

    wx 需要包含：
        rain_in_hours : 未来几小时开始下雨（int 或 None）
        uv            : 紫外线指数
        temp / thigh / tlow
        hum           : 湿度
        wind          : 风速 km/h（可选）
        tmrw_code     : 明日天气代码（可选）
    """
    lines: List[str] = []

    def add(s: str) -> None:
        if not s:
            return
        # 硬性限制 10 个字符，避免超出右栏
        if len(s) > 10:
            s = s[:10]
        if s not in lines:
            lines.append(s)

    rain_in = wx.get("rain_in_hours")
    uv = wx.get("uv") or 0
    temp = wx.get("temp")
    thigh = wx.get("thigh")
    tlow = wx.get("tlow")
    hum = wx.get("hum") or 0
    wind = wx.get("wind") or 0
    tmrw = wx.get("tmrw_code")

    # ---- 第 1 行：最重要的提醒 ----
    if rain_in is not None and 1 <= rain_in <= 9:
        add(_fmt(PHRASES["rain_in"], n=int(rain_in)))
    elif rain_in == 0:
        add(PHRASES["rain_soon"])
    elif uv >= 8:
        add(PHRASES["uv_very"])
    elif uv >= 6:
        add(PHRASES["uv_strong"])
    elif isinstance(thigh, int) and thigh >= 33:
        add(PHRASES["hot"])
    elif isinstance(tlow, int) and tlow <= 2:
        add(PHRASES["cold"])
    elif wind >= 30:
        add(PHRASES["windy"])
    elif hum >= 85:
        add(PHRASES["wet"])
    elif hum <= 25:
        add(PHRASES["dry"])
    else:
        add(PHRASES["clear"])

    # ---- 第 2 行：配套动作 ----
    if rain_in is not None and 0 <= rain_in <= 9:
        add(PHRASES["umbrella"])
    elif uv >= 6:
        add(PHRASES["sunscreen"])
    elif isinstance(thigh, int) and thigh >= 33:
        add(PHRASES["water"])
    elif isinstance(tlow, int) and tlow <= 2:
        add(PHRASES["extra"])
    elif wind >= 30:
        add(PHRASES["windproof"])
    elif isinstance(thigh, int) and isinstance(tlow, int) and (thigh - tlow) >= 12:
        add(PHRASES["big_diff"])
    else:
        add(PHRASES["outside"])

    # ---- 第 3 行：明日预告 ----
    if tmrw in (71, 73, 75, 77, 85, 86):
        add(PHRASES["snow_tmrw"])
    elif tmrw in (51, 53, 55, 56, 57, 61, 63, 65, 66, 67, 80, 81, 82, 95, 96, 99):
        add(PHRASES["rain_tmrw"])

    return lines[:3]


if __name__ == "__main__":
    cs = charset()
    print(f"字符集大小: {len(cs)}")
    print("字符:", "".join(cs))
    demo = build_advice({"rain_in_hours": 2, "uv": 3, "temp": 26,
                         "thigh": 28, "tlow": 18, "hum": 60, "wind": 10})
    print("示例建议:", demo)
