"""EPD 墨水屏天气站 —— Windows 客户端 BLE 自动同步

安装/卸载用 install_task.ps1，本文件只负责同步逻辑。

为什么用"当前用户登录时启动的任务"而不是 Windows 服务（SYSTEM）：
    ESP32 是通过 Bluetooth LE 通信的，Windows 上 bleak 走 WinRT 的
    Windows.Devices.Bluetooth 接口，这套 API 依赖当前用户的蓝牙运行时。
    SYSTEM 服务跑在 session 0，没有用户会话上下文，拿不到蓝牙设备枚举，
    BLE 扫描会直接失败（或需要不可靠的 WinRT 打包/IPC 中转）。
    因此业界通行做法就是：用户登录时以当前用户身份运行后台进程。
    本程序即按此方式安装，注册表/任务计划里表现为一个"隐藏窗口后台任务"。
"""

from __future__ import annotations

import argparse
import asyncio
import json
import logging
import logging.handlers
import sys
import threading
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Optional

import httpx
from bleak import BleakClient, BleakScanner
from bleak.exc import BleakError

# advice.py 与本文件同目录。显式加入搜索路径，
# 保证从任意工作目录（含计划任务/服务方式）启动都能 import 到。
sys.path.insert(0, str(Path(__file__).resolve().parent))

# ---------------------------------------------------------------- 常量

NUS_SERVICE = "6e400001-b5a3-f393-e0a9-e50e24dcca9e"
NUS_RX = "6e400002-b5a3-f393-e0a9-e50e24dcca9e"
DEVICE_NAME = "EPD-Weather"

# 固件在广播包 Manufacturer Data 里塞的状态标志（company id 用 0xFFFF 测试段）
#   [0]=0xFF [1]=0xFF  company id
#   [2]=协议版本 0x01
#   [3]=标志位  bit0=1 表示数据过期、请推送
MANUF_ID = 0xFFFF
MANUF_VER = 0x01
FLAG_NEED_SYNC = 0x01

GEO_URL = "https://geocoding-api.open-meteo.com/v1/search"
WX_URL = "https://api.open-meteo.com/v1/forecast"

# WMO 天气代码 -> ≤16 字符 ASCII 描述（墨水屏字库只有 ASCII）
WMO_DESC = {
    0: "Clear", 1: "Mainly Clear", 2: "Partly Cloudy", 3: "Overcast",
    45: "Fog", 48: "Freezing Fog",
    51: "Light Drizzle", 53: "Drizzle", 55: "Heavy Drizzle",
    56: "Freezing Drizzle", 57: "Freezing Drizzle",
    61: "Light Rain", 63: "Rain", 65: "Heavy Rain",
    66: "Freezing Rain", 67: "Freezing Rain",
    71: "Light Snow", 73: "Snow", 75: "Heavy Snow", 77: "Snow Grains",
    80: "Rain Showers", 81: "Rain Showers", 82: "Heavy Showers",
    85: "Snow Showers", 86: "Snow Showers",
    95: "Thunderstorm", 96: "Thunderstorm", 99: "Thunderstorm",
}

# WMO -> wttr.in 风格代码，驱动固件左上角天气图标
WMO_ICON = {
    0: 113, 1: 113, 2: 116, 3: 122,
    45: 143, 48: 143,
    51: 266, 53: 266, 55: 296,
    56: 311, 57: 311,
    61: 296, 63: 299, 65: 305,
    66: 311, 67: 314,
    71: 326, 73: 329, 75: 335, 77: 323,
    80: 353, 81: 356, 82: 359,
    85: 368, 86: 371,
    95: 200, 96: 200, 99: 200,
}

# 哪些 WMO 码算"下雨/下雪"（用于"几小时后有雨"判断）
RAIN_CODES = {51, 53, 55, 56, 57, 61, 63, 65, 66, 67, 80, 81, 82, 95, 96, 99}

BASE_DIR = Path(__file__).resolve().parent
CONFIG_PATH = BASE_DIR / "config.json"
LOG_DIR = BASE_DIR / "logs"
STATE_PATH = BASE_DIR / "state.json"

DEFAULT_CONFIG = {
    "city": "Hangzhou",
    "language": "zh",
    "interval_minutes": 15,
    "device_name": DEVICE_NAME,
    "device_address": "",
    "scan_timeout_s": 12.0,
    "connect_timeout_s": 25.0,
    "retry_backoff_s": [30, 60, 120, 300, 600],
    "log_level": "INFO",
    "only_when_changed": True,
    "startup_delay_s": 3,
    "idle_poll_s": 60.0,
    "probe_timeout_s": 3.0,
    "refresh_grace_s": 25.0,
}

log = logging.getLogger("epd")


# ---------------------------------------------------------------- 配置

def load_config() -> dict:
    cfg = dict(DEFAULT_CONFIG)
    if CONFIG_PATH.exists():
        try:
            # utf-8-sig：PowerShell 写出的 JSON 可能带 BOM，用 utf-8 会解析失败
            raw = CONFIG_PATH.read_text(encoding="utf-8-sig")
            user = json.loads(raw)
            if isinstance(user, dict):
                cfg.update(user)
        except Exception as e:
            log.warning("配置文件解析失败，用默认值：%s", e)
    else:
        try:
            CONFIG_PATH.write_text(
                json.dumps(cfg, ensure_ascii=False, indent=2), encoding="utf-8")
            log.info("已生成默认配置：%s", CONFIG_PATH)
        except Exception as e:
            log.warning("无法写入默认配置：%s", e)
    return cfg


# ---------------------------------------------------------------- 天气

def _safe_int(v) -> Optional[int]:
    try:
        if v is None or isinstance(v, bool):
            return None
        f = float(v)
        if f != f:
            return None
        return int(round(f))
    except (TypeError, ValueError):
        return None


def _desc(code) -> str:
    try:
        return WMO_DESC.get(int(code), "Unknown")
    except (TypeError, ValueError):
        return "Unknown"


def _icon(code) -> int:
    try:
        return WMO_ICON.get(int(code), 113)
    except (TypeError, ValueError):
        return 113


def geocode(client: httpx.Client, name: str, lang: str) -> dict:
    r = client.get(GEO_URL, params={"name": name, "count": 1, "language": lang})
    r.raise_for_status()
    data = r.json()
    # 坑：查不到城市时 Open-Meteo 返回 200 但没有 results
    results = data.get("results")
    if not results:
        raise ValueError(f"找不到城市：{name}")
    g = results[0]
    return {"name": g.get("name", name), "admin": g.get("admin1") or "",
            "country": g.get("country") or "",
            "lat": g["latitude"], "lon": g["longitude"]}


def fetch_weather(client: httpx.Client, lat: float, lon: float) -> dict:
    params = {
        "latitude": lat, "longitude": lon,
        "current": ("temperature_2m,apparent_temperature,relative_humidity_2m,"
                    "weather_code,uv_index,wind_speed_10m"),
        # 加 hourly 用于判断"几小时后有雨"
        "hourly": "precipitation_probability,weather_code",
        "daily": "temperature_2m_max,temperature_2m_min,weather_code",
        "timezone": "auto", "forecast_days": 2,
    }
    r = client.get(WX_URL, params=params)
    r.raise_for_status()
    return r.json()


def _rain_in_hours(wx: dict) -> int | None:
    """判断未来几小时会下雨。返回小时数（1~9），已在雨里返回 0，否则 None。"""
    hourly = wx.get("hourly") or {}
    codes = hourly.get("weather_code") or []
    probs = hourly.get("precipitation_probability") or []
    times = hourly.get("time") or []
    if not codes or not times:
        return None

    # 找到"当前小时"在 hourly 里的位置
    now_key = time.strftime("%Y-%m-%dT%H:00", time.localtime())
    idx = None
    for i, t in enumerate(times):
        if str(t) == now_key:
            idx = i
            break
    if idx is None:
        # 时区/日期对不上时退化：取第一个
        idx = 0

    cur_code = codes[idx] if idx < len(codes) else None
    if cur_code in RAIN_CODES:
        return 0

    for step in range(1, 10):
        j = idx + step
        if j >= len(codes) or j >= len(probs):
            break
        if codes[j] in RAIN_CODES or (isinstance(probs[j], (int, float)) and probs[j] >= 60):
            return step
    return None


def build_payload(wx: dict) -> dict:
    cur = wx.get("current") or {}
    daily = wx.get("daily") or {}
    tmax = daily.get("temperature_2m_max") or []
    tmin = daily.get("temperature_2m_min") or []
    dcode = daily.get("weather_code") or []
    days = daily.get("time") or []

    p: dict = {"epoch": int(time.time())}

    for key, src in (("temp", "temperature_2m"), ("feels", "apparent_temperature"),
                     ("hum", "relative_humidity_2m"), ("uv", "uv_index")):
        v = _safe_int(cur.get(src))
        if v is not None:
            p[key] = v

    ccode = cur.get("weather_code")
    if ccode is not None:
        p["desc"] = _desc(ccode)
        p["code"] = _icon(ccode)

    if len(tmin) > 0:
        v = _safe_int(tmin[0])
        if v is not None:
            p["tlow"] = v
    if len(tmax) > 0:
        v = _safe_int(tmax[0])
        if v is not None:
            p["thigh"] = v

    if len(days) > 1:
        mmdd = str(days[1])[5:10]
        if len(mmdd) == 5 and mmdd[2] == "-":
            p["fc_date"] = mmdd
    if len(dcode) > 1 and dcode[1] is not None:
        p["fc_desc"] = _desc(dcode[1])
    if len(tmin) > 1:
        v = _safe_int(tmin[1])
        if v is not None:
            p["fc_low"] = v
    if len(tmax) > 1:
        v = _safe_int(tmax[1])
        if v is not None:
            p["fc_high"] = v

    # ---------- v4：中文建议（右栏）----------
    try:
        import advice as _adv
        rain_in = _rain_in_hours(wx)
        uv_val = _safe_int(cur.get("uv_index")) or 0
        wind = cur.get("wind_speed_10m")
        lines = _adv.build_advice({
            "rain_in_hours": rain_in,
            "uv": uv_val,
            "temp": p.get("temp"),
            "thigh": p.get("thigh"),
            "tlow": p.get("tlow"),
            "hum": p.get("hum") or 0,
            "wind": wind if isinstance(wind, (int, float)) else 0,
            "tmrw_code": dcode[1] if len(dcode) > 1 else None,
        })
        for i, line in enumerate(lines[:2], start=1):
            p[f"a{i}"] = line
    except Exception as e:  # noqa: BLE001
        log.warning("生成中文建议失败：%s", e)

    return p


# ---------------------------------------------------------------- 状态持久化

def load_state() -> dict:
    if STATE_PATH.exists():
        try:
            return json.loads(STATE_PATH.read_text(encoding="utf-8-sig")) or {}
        except Exception:
            pass
    return {}


def save_state(st: dict) -> None:
    try:
        STATE_PATH.write_text(json.dumps(st, ensure_ascii=False, indent=2),
                              encoding="utf-8")
    except Exception as e:
        log.debug("保存状态失败：%s", e)


# ---------------------------------------------------------------- 同步器

@dataclass
class Stats:
    attempts: int = 0
    ok: int = 0
    fail: int = 0
    last_ok_ts: float = 0.0
    last_error: str = ""
    consecutive_fail: int = 0
    last_payload: dict = field(default_factory=dict)


class BleSync:
    def __init__(self, cfg: dict):
        self.cfg = cfg
        self.stats = Stats()
        self._stop = threading.Event()
        self._loop: Optional[asyncio.AbstractEventLoop] = None
        self._thread: Optional[threading.Thread] = None
        self._wake = threading.Event()
        self._force_next = False
        self._skip_polls = 0

        st = load_state()
        if st.get("last_payload"):
            self.stats.last_payload = st["last_payload"]
        if st.get("last_ok_ts"):
            self.stats.last_ok_ts = float(st["last_ok_ts"])

    # -------------------------- 线程

    def start(self) -> None:
        self._thread = threading.Thread(target=self._run_loop, name="ble-loop", daemon=True)
        self._thread.start()

    def _run_loop(self) -> None:
        self._loop = asyncio.new_event_loop()
        asyncio.set_event_loop(self._loop)
        try:
            self._loop.run_forever()
        finally:
            self._loop.close()

    def stop(self) -> None:
        self._stop.set()
        self._wake.set()
        if self._loop and self._loop.is_running():
            self._loop.call_soon_threadsafe(self._loop.stop)
        if self._thread:
            self._thread.join(timeout=10)

    def request_sync_now(self) -> None:
        """外部立刻触发一次（不等待下一轮探测）。"""
        self._force_next = True
        self._wake.set()

    # -------------------------- 设备发现

    async def _find_device(self):
        name = self.cfg.get("device_name") or DEVICE_NAME
        addr = (self.cfg.get("device_address") or "").strip()
        timeout = float(self.cfg.get("scan_timeout_s", 12.0))

        if addr:
            log.info("用记住的地址直连：%s", addr)
            return addr

        log.info("扫描 BLE 设备（找 %s）…", name)
        by_name, by_uuid, by_prefix = [], [], []
        anonymous = 0

        def _cb(dev, adv):
            nonlocal anonymous
            nm = (adv.local_name or dev.name or "").strip()
            uuids = [str(u).lower() for u in (adv.service_uuids or [])]
            if nm == name:
                by_name.append(dev)
            elif any(NUS_SERVICE in u for u in uuids):
                by_uuid.append(dev)
            elif nm and name.lower() in nm.lower():
                by_prefix.append(dev)
            if not nm and not uuids:
                anonymous += 1

        scanner = BleakScanner(detection_callback=_cb)
        await scanner.start()
        try:
            await asyncio.sleep(timeout)
        finally:
            await scanner.stop()

        pick, why = None, ""
        if by_name:
            pick, why = by_name[0], "名字完全匹配"
        elif by_uuid:
            pick, why = by_uuid[0], "NUS 服务 UUID 匹配"
        elif by_prefix:
            pick, why = by_prefix[0], "名字前缀匹配"

        if pick is None:
            log.warning("没找到 %s（期间匿名广播 %d 个）。检查：①墨水屏上电 "
                        "②没被手机连着 ③刚连过需等固件重新广播", name, anonymous)
            return None

        log.info("找到设备：%s [%s]（%s）", pick.name or "(无名)", pick.address, why)
        self.cfg["device_address"] = pick.address
        self._remember_address(pick.address)
        return pick

    def _remember_address(self, addr: str) -> None:
        """把地址写回 config.json，下次免扫描。"""
        try:
            cfg = {}
            if CONFIG_PATH.exists():
                cfg = json.loads(CONFIG_PATH.read_text(encoding="utf-8-sig")) or {}
            if cfg.get("device_address") == addr:
                return
            cfg["device_address"] = addr
            CONFIG_PATH.write_text(json.dumps(cfg, ensure_ascii=False, indent=2),
                                   encoding="utf-8")
            log.info("已记住设备地址：%s", addr)
        except Exception as e:
            log.debug("写地址失败：%s", e)

    # -------------------------- 广播探测（低成本）
    #
    # 不做连接、只收广播，因此耗电极低（一次 3 秒扫描）。
    # 关键是能读到固件广播的 need_sync 标志：设备一重启就置 1，
    # 主机看到就立刻推数据，不用等整点 —— 这是"刚开机就能收到"的核心。

    async def probe(self, timeout: float) -> tuple[bool, bool | None]:
        """短扫描。

        返回 (present, needs_sync)：
          present=False    设备没在广播（没上电 / 被别的设备连着）
          needs_sync=True  设备说数据过期（刚开机），应立即推送
          needs_sync=False 设备数据是新的
          needs_sync=None  老固件没有这个标志，按时间判断
        """
        name = self.cfg.get("device_name") or DEVICE_NAME
        addr = (self.cfg.get("device_address") or "").strip()
        state: dict = {"dev": None, "needs": None}

        def _cb(dev, adv):
            nm = (adv.local_name or dev.name or "").strip()
            uuids = [str(u).lower() for u in (adv.service_uuids or [])]
            hit = (nm == name) or any(NUS_SERVICE in u for u in uuids)
            # 有固定地址时也认地址，避免设备改名后失联
            if not hit and addr and dev.address.upper() == addr.upper():
                hit = True
            if not hit:
                return
            md = (adv.manufacturer_data or {}).get(MANUF_ID)
            needs = None
            if md and len(md) >= 2 and md[0] == MANUF_VER:
                needs = bool(md[1] & FLAG_NEED_SYNC)
            state["dev"] = dev
            state["needs"] = needs

        scanner = BleakScanner(detection_callback=_cb)
        await scanner.start()
        try:
            await asyncio.sleep(timeout)
        finally:
            await scanner.stop()

        dev = state["dev"]
        if dev is not None:
            # 记住地址：之后同步可直接连，省掉一次 12 秒扫描
            if not addr or addr.upper() != dev.address.upper():
                self.cfg["device_address"] = dev.address
                self._remember_address(dev.address)
            log.debug("探测到设备 %s，need_sync=%s", dev.address, state["needs"])
            return True, state["needs"]
        return False, None

    async def _tick(self, interval: float, probe_to: float) -> str:
        """一次巡检。返回 'sync' / 'skip' / 'absent' / 'fail'。

        'absent' 单独区分：设备没在广播是常态（关机、被手机连着），
        不该按失败计入退避，否则日志会一直报错。
        """
        forced = self._force_next
        self._force_next = False

        if forced:
            log.info("手动触发 → 直接同步")
            return "sync" if await self.sync_once(force=True) else "fail"

        present, needs = await self.probe(probe_to)
        if not present:
            log.info("设备不在广播（没上电 / 被其它设备连着 / 刚断开还未重新广播）")
            return "absent"

        # 没有历史记录时用 None 表示"从未同步"，不要拿 1e18 去算，
        # 否则日志会打出 16666666666666666 这种天文数字。
        age = (time.time() - self.stats.last_ok_ts) if self.stats.last_ok_ts else None

        if needs is True:
            log.info("设备广播 need_sync=1（刚开机 / 数据过期）→ 立即推送")
            return "sync" if await self.sync_once(force=True) else "fail"

        if needs is None:
            log.debug("设备未提供 need_sync 标志（老固件），按时间判断")

        if age is None:
            log.info("本机还没有同步记录 → 立即刷新")
            return "sync" if await self.sync_once() else "fail"

        if age >= interval:
            log.info("距上次同步 %.0f 分钟 ≥ %.0f 分钟 → 正常刷新",
                     age / 60.0, interval / 60.0)
            return "sync" if await self.sync_once() else "fail"

        # 跳过时不刷日志（每分钟一次会淹没日志），但每 10 轮打个心跳，
        # 让人能确认服务还活着。
        self._skip_polls += 1
        if self._skip_polls % 10 == 0:
            log.info("设备在线，数据新鲜（%.0f 分钟前同步），已跳过 %d 轮",
                     age / 60.0, self._skip_polls)
        else:
            log.debug("设备在线且数据新鲜（%.0f 分钟前同步），本轮跳过", age / 60.0)
        return "skip"

    # -------------------------- 主流程

    async def sync_once(self, force: bool = False) -> bool:
        """取天气并推送。

        force=True 时跳过"天气无变化就不写屏"的判断 —— 用于设备刚开机、
        屏幕上是 NVS 旧数据的情形：哪怕内容一样也必须写一次，
        否则设备广播里的 need_sync 标志永远清不掉，会反复触发。
        """
        self.stats.attempts += 1
        try:
            payload = await asyncio.to_thread(self._build_payload_sync)
        except Exception as e:  # noqa: BLE001
            self._on_fail(f"取天气失败：{e}")
            return False

        if not force and self.cfg.get("only_when_changed", True):
            a = {k: v for k, v in (self.stats.last_payload or {}).items() if k != "epoch"}
            b = {k: v for k, v in payload.items() if k != "epoch"}
            if a and a == b:
                log.info("天气无变化，跳过写屏（保护墨水屏）")
                self.stats.last_ok_ts = time.time()
                self.stats.consecutive_fail = 0
                self._persist()
                return True
        elif force:
            log.info("设备请求同步（刚开机/数据过期）→ 强制写一次，忽略去重")

        ok = await self._push(payload)
        if ok:
            self.stats.ok += 1
            self.stats.last_ok_ts = time.time()
            self.stats.consecutive_fail = 0
            self.stats.last_error = ""
            self.stats.last_payload = payload
        else:
            self.stats.fail += 1
            self.stats.consecutive_fail += 1
        self._persist()
        return ok

    def _persist(self) -> None:
        save_state({"last_payload": self.stats.last_payload,
                    "last_ok_ts": self.stats.last_ok_ts,
                    "last_error": self.stats.last_error})

    def _build_payload_sync(self) -> dict:
        city = self.cfg.get("city") or "Hangzhou"
        lang = self.cfg.get("language") or "zh"
        timeout = httpx.Timeout(20.0, connect=10.0)

        # 国内直连 Open-Meteo 偶尔会被掐连接（WinError 10054）。
        # 单次抖动不应该让整个同步失败并退避 30 秒，这里就地重试 3 次。
        last_err: Exception | None = None
        for attempt in range(3):
            try:
                with httpx.Client(timeout=timeout, follow_redirects=True) as client:
                    g = geocode(client, city, lang)
                    log.info("城市：%s %s %s (%.4f, %.4f)",
                             g["name"], g["admin"], g["country"], g["lat"], g["lon"])
                    wx = fetch_weather(client, g["lat"], g["lon"])
                return build_payload(wx)
            except Exception as e:  # noqa: BLE001
                last_err = e
                if attempt < 2:
                    wait = 1.5 * (attempt + 1)
                    log.warning("取天气失败（第 %d 次）：%s，%.1fs 后重试",
                                attempt + 1, e, wait)
                    time.sleep(wait)
        raise last_err if last_err else RuntimeError("取天气失败")

    async def _push(self, payload: dict) -> bool:
        dev = await self._find_device()
        if dev is None:
            self._on_fail("未找到设备")
            return False

        timeout = float(self.cfg.get("connect_timeout_s", 25.0))
        text = json.dumps(payload, ensure_ascii=False, separators=(",", ":"))
        body = (text + "\n").encode("utf-8")

        try:
            log.info("连接设备…（超时 %.0fs）", timeout)
            async with BleakClient(dev, timeout=timeout) as client:
                if not client.is_connected:
                    self._on_fail("连接失败")
                    return False
                log.info("已连接：%s", client.address)

                # 整包写；失败则退化为 180 字节分片
                try:
                    await client.write_gatt_char(NUS_RX, body, response=False)
                except BleakError:
                    for i in range(0, len(body), 180):
                        await client.write_gatt_char(NUS_RX, body[i:i + 180],
                                                     response=False)
                        await asyncio.sleep(0.03)

                log.info("已写入 %d 字节：%s", len(body), text)
                await asyncio.sleep(0.6)
                return True
        except asyncio.TimeoutError:
            self._on_fail("连接或写入超时")
            return False
        except BleakError as e:
            self._on_fail(f"BLE 错误：{e}")
            return False
        except Exception as e:  # noqa: BLE001
            self._on_fail(f"未知错误：{e}")
            return False

    def _on_fail(self, msg: str) -> None:
        self.stats.last_error = msg
        log.warning("同步失败：%s（连续 %d 次）", msg, self.stats.consecutive_fail + 1)

    # -------------------------- 长期运行

    async def run_forever(self) -> None:
        """主循环：短扫描探测 → 需要才同步。

        调度逻辑（取代原来的"固定睡 N 分钟再同步一次"）：
          每 idle_poll_s（默认 60s）做一次 3 秒广播探测：
            · 读不到设备            → 退避后重试
            · 设备广播 need_sync    → 立刻推送（设备刚开机就是这个情形）
            · 距上次同步 ≥ interval → 正常刷新
            · 否则                  → 什么都不做（不连接，不耗电）
        """
        interval = float(self.cfg.get("interval_minutes", 15)) * 60.0
        poll = max(15.0, float(self.cfg.get("idle_poll_s", 60.0)))
        probe_to = float(self.cfg.get("probe_timeout_s", 3.0))
        backoff = list(self.cfg.get("retry_backoff_s") or [30, 60, 120, 300, 600])

        log.info("服务启动：城市=%s｜常规刷新 %.0f 分钟｜空闲每 %.0f 秒探测一次",
                 self.cfg.get("city"), interval / 60.0, poll)

        delay0 = float(self.cfg.get("startup_delay_s", 3))
        if delay0 > 0:
            log.info("启动等待 %.0f 秒…", delay0)
            await asyncio.sleep(delay0)

        # 启动后先同步一次（不受"距上次多久"限制）：
        # 保证电脑刚开机、或服务被重启后，屏幕上立刻是当前天气。
        # 仍然尊重 only_when_changed —— 天气没变就不白刷一次墨水屏。
        log.info("启动首次同步…")
        await self.sync_once()

        # 墨水屏刷一次要 15~20 秒，这期间固件主循环被占住、来不及重新广播。
        # 等它刷完再开始探测，免得第一轮扑空。
        await asyncio.sleep(float(self.cfg.get("refresh_grace_s", 25.0)))

        while not self._stop.is_set():
            result = await self._tick(interval, probe_to)

            if result == "fail":
                idx = min(max(self.stats.consecutive_fail - 1, 0), len(backoff) - 1)
                wait = float(backoff[idx])
                log.info("本轮失败，%.0f 秒后重试", wait)
            else:
                wait = poll
            end = time.monotonic() + wait
            while time.monotonic() < end and not self._stop.is_set():
                if self._wake.is_set():
                    self._wake.clear()
                    log.info("收到手动触发，立即同步")
                    break
                await asyncio.sleep(1.0)


# ---------------------------------------------------------------- 入口

def setup_logging(level: str, log_dir: Path) -> None:
    log_dir.mkdir(parents=True, exist_ok=True)
    fmt = logging.Formatter("%(asctime)s [%(levelname)s] %(message)s",
                            datefmt="%Y-%m-%d %H:%M:%S")
    root = logging.getLogger()
    root.setLevel(getattr(logging, level.upper(), logging.INFO))

    fh = logging.handlers.RotatingFileHandler(
        log_dir / "epd-sync.log", maxBytes=1024 * 1024, backupCount=3, encoding="utf-8")
    fh.setFormatter(fmt)
    root.addHandler(fh)

    # 控制台：Windows 传统控制台默认 GBK，中文会变乱码，强制切成 UTF-8。
    # pythonw 下没有控制台，这一步会失败，忽略即可。
    try:
        if sys.stdout is not None and sys.stdout.isatty():
            try:
                sys.stdout.reconfigure(encoding="utf-8", errors="replace")
                sys.stderr.reconfigure(encoding="utf-8", errors="replace")
            except Exception:
                pass
        sh = logging.StreamHandler(sys.stdout)
        sh.setFormatter(fmt)
        root.addHandler(sh)
    except Exception:
        pass


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description="EPD 墨水屏天气 BLE 自动同步")
    ap.add_argument("--once", action="store_true", help="只同步一次就退出（调试用）")
    ap.add_argument("--city", help="覆盖城市名")
    ap.add_argument("--interval", type=float, help="覆盖同步间隔（分钟）")
    ap.add_argument("--log-level", default=None)
    args = ap.parse_args(argv)

    cfg = load_config()
    if args.city:
        cfg["city"] = args.city
    if args.interval:
        cfg["interval_minutes"] = args.interval
    if args.log_level:
        cfg["log_level"] = args.log_level

    setup_logging(cfg.get("log_level", "INFO"), LOG_DIR)
    sync = BleSync(cfg)

    if args.once:
        async def _once():
            return await sync.sync_once(force=True)
        ok = asyncio.run(_once())
        log.info("单次同步结束：%s", "成功" if ok else "失败")
        return 0 if ok else 1

    sync.start()
    for _ in range(100):
        if sync._loop and sync._loop.is_running():
            break
        time.sleep(0.05)
    assert sync._loop is not None
    asyncio.run_coroutine_threadsafe(sync.run_forever(), sync._loop)

    stop = {"v": False}

    def _handle(signum, frame):  # noqa: ARG001
        log.info("收到退出信号 %s", signum)
        stop["v"] = True

    try:
        import signal as _sig
        _sig.signal(_sig.SIGINT, _handle)
        _sig.signal(_sig.SIGTERM, _handle)
    except (ValueError, OSError, ImportError):
        pass

    try:
        while not stop["v"]:
            time.sleep(0.5)
    except KeyboardInterrupt:
        pass
    finally:
        log.info("正在停止…")
        sync.stop()
    return 0


if __name__ == "__main__":
    sys.exit(main())
