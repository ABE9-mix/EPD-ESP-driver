# EPD 墨水屏天气站 · 蓝牙同步改造说明

改造对象：`examples\20_epd_weather_station`（PDI 2.13" 三色墨水屏 212×104）
改造日期：2026-11-01
编译验证：**已通过** —— `arduino-cli compile`，`esp32:esp32:esp32s3`，`FlashSize=16M, PartitionScheme=huge_app`
→ 固件 1,259,521 B（**40%** of 3 MB），RAM 58,628 B（17%，余 269 KB）

---

## 一、先说结论：为什么走这个方案

### 1.1 你的原始想法（手机共享蓝牙网络）——**不可行**

| 你以为的路径 | 实际情况 |
|---|---|
| 手机开「蓝牙网络共享」，ESP32 连上去当网卡 | ❌ **做不到** |

原因是硬件的，不是配置问题：

1. **ESP32-S3 只有 Bluetooth LE（BLE 5.0），没有蓝牙经典（BR/EDR）**。
   蓝牙网络共享（Bluetooth PAN）用的是**经典蓝牙**的 BNEP 协议，S3 的射频/协议栈里根本没有这一层。
2. 所以 Arduino 的 `BluetoothSerial`（SPP 串口，也是经典蓝牙）在 **S3 上同样不可用**——
   该库只对 ESP32 / ESP32-S2 等支持经典蓝牙的型号有效。
3. iPhone 的「个人热点」根本不能经蓝牙共享；Android 的「蓝牙网络共享」也只对经典蓝牙 PAN 客户端生效。
4. BLE 本身没有 IP 承载层，不能当网卡用。

> 一句话：**这条路在 ESP32-S3 上不存在，别在这上面花时间。**

### 1.2 你的真实痛点，其实是 WiFi 频段

ESP32-S3 的 WiFi **只支持 2.4GHz**。家里路由器如果是 5GHz-only 或者 2.4G 关着，
它就永远连不上，也就没法走 `wttr.in` 取天气——这才是"2.4G 网络比较少"的根源。

### 1.3 正确路线：BLE GATT 写入

```
手机（取天气）  --BLE 写入 JSON-->  ESP32-S3（解析 → 刷墨水屏）
```

手机有 4G/5G/WiFi 随便上网，取到天气后用蓝牙把结果**写进** ESP32。
ESP32 不需要上网，也就不再受 2.4G 限制。

---

## 二、代码改了什么

### 2.1 新增两个文件

| 文件 | 作用 |
|---|---|
| `eink_weather.h` | 模块接口（启动、轮询、连接状态、天气结构体） |
| `eink_weather.cpp` | BLE GATT Server 实现 + 极简 JSON 解析 |

> **没有引入 ArduinoJson**：本机未安装该库，且协议字段固定，手写解析器约 40 行，省一个依赖。

### 2.2 修改 `20_epd_weather_station.ino`

| 位置 | 改动 |
|---|---|
| 文件头 | v2 → v3，写明 BLE 方案与"无经典蓝牙"的结论 |
| 功能开关 | 新增 `ENABLE_BLE_SYNC 1`；`ENABLE_WEATHER_SYNC` 默认改 **0**（旧 WiFi 取天气关闭，代码保留） |
| `loopNormalMode()` 尾部 | 调 `bleLoop()`；有 pending 数据就 `applyBleWeather()` → `displayWeather()` 刷屏 |
| 新增 `applyBleWeather()` | 把 BLE 字段搬到原 `weather_*` 全局变量（**增量合并**，缺省字段保留旧值） |
| `setup()` | EPD 初始化后立即 `bleBegin()`；**WiFi 连不上时不再进 AP 热点**，直接留 BLE-only 模式 |

> 为什么 WiFi 连不上不再进 AP 模式：BLE 和 WiFi 共享 2.4G 射频，同时跑会互相降速。
> 既然主路径是蓝牙，就没必要为了配网把射频让给热点。

---

## 三、BLE 协议（客户端照这个实现）

设备名：**`EPD-Weather`**

采用 **Nordic UART Service (NUS)** UUID —— 好处是 nRF Connect、BLE 串口助手
这类通用 App **不用改代码就能直接连**，方便你自己先手动验证。

### 3.0 广播包实际长什么样（已查 core 源码确认）

128 位 UUID 占 18 字节，所以广播包放不下「UUID + 设备名」，**必须确认谁在 adv、谁在 scan response**。
已读 `BLEAdvertising.cpp` 的 `buildRawAdvData()` / `start()` 确认本 core 的实际行为：

| 包 | 内容 | 字节 |
|---|---|---|
| **ADV**（广播包，31B 上限） | Flags(3) + **NUS UUID 128-bit(18)** | **21 B** |
| **SCAN RESP**（扫描响应） | 设备名 `EPD-Weather`(13) | 13 B |

逻辑是：先算 adv 里放了 UUID 还剩多少；名字塞不下就**自动挪到 scan response**（`m_nameInScanResp`）。

> ✅ **结论：按 `services: [NUS UUID]` 过滤可以正常扫到设备**，因为 UUID 就在广播包里。
> ✅ 按 `name: "EPD-Weather"` 过滤也能扫到，因为扫描响应带名字（手机会主动发 scan request）。
> 所以客户端那个「兼容模式（acceptAllDevices）」**只是保险，不是必需**。

| 用途 | UUID | 属性 |
|---|---|---|
| Service | `6e400001-b5a3-f393-e0a9-e50e24dcca9e` | — |
| RX（手机 → ESP32） | `6e400002-b5a3-f393-e0a9-e50e24dcca9e` | Write / WriteNoResponse |
| TX（ESP32 → 手机） | `6e400003-b5a3-f393-e0a9-e50e24dcca9e` | Notify |

### 3.1 写天气

UTF-8 JSON，**以 `\n` 结尾**。单包建议 ≤180 字节切片，MTU 已设为 500。

```json
{"temp":23,"feels":25,"desc":"Sunny","hum":52,"uv":3,"tlow":18,"thigh":27,"fc_date":"11-03","fc_desc":"Cloudy","fc_low":16,"fc_high":24}
```

字段**全部可选**（缺省则保留屏上原值，支持只更新某一项）：

| 字段 | 类型 | 约束 | 显示位置 |
|---|---|---|---|
| `temp` `feels` `hum` `uv` `tlow` `thigh` | 整数 | 摄氏度/百分比，不带单位 | 主区 |
| `desc` | 字符串 | **≤16 字符，纯 ASCII** | 温度下方 |
| `fc_date` | 字符串 | `MM-DD`，≤5 字符 | 底部预告 |
| `fc_desc` | 字符串 | ≤16 字符，纯 ASCII | 底部预告 |
| `fc_low` `fc_high` | 整数 | 摄氏度 | 底部预告 |

> ⚠️ **字库只有 ASCII**：`drawChar()` 用的是内置 5×7 英文字模，中文会渲染成方框。
> 所以天气描述请用 `Sunny / Cloudy / Rain / Snow / Fog / Storm` 这类英文短词。

### 3.2 查询状态

写：`{"cmd":"status"}`
收（TX notify）：
```json
{"ok":true,"last":"16:40","msg":"status","heap":123456}
```

成功刷屏后 ESP32 会主动 notify：`{"ok":true,"msg":"displayed"}`

---

## 四、怎么用（三条路，按难度排）

### 路线 A：先手动验证（5 分钟，验证硬件链路）

1. 烧录固件，串口应看到 `[BLE] 广播中，设备名: EPD-Weather`
2. 手机装 **nRF Connect**（安卓/iOS 都有）
3. 扫描 → 连 `EPD-Weather` → 展开 NUS Service
4. 往 RX 特征写：`{"temp":23,"desc":"Sunny"}`（**记得点 UTF-8 模式**，并加换行）
5. 墨水屏应立刻刷新

> 这一步能跑通，说明固件没问题，后面都只是"谁来取天气"的差别。

### 路线 B：网页版（零安装，推荐日常用）

见 `F:\AIchat\epd_ble\web\index.html`，配套说明在同目录 `蓝牙同步方案.md`。

### 路线 C：微信小程序 / 自写 App

见方案文档里的对比表。

---

## 五、需要注意的坑

| 现象 | 原因 | 处理 |
|---|---|---|
| **写入成功但屏幕不刷新** | （v3.0 的 bug，已修）`displayWeather()` 开头用 `ENABLE_WEATHER_SYNC` 做总开关，而该宏已改为 0 用于关闭 WiFi 取数，导致画屏函数直接 return | v3.1 起改为 `if (!ENABLE_WEATHER_SYNC && !ENABLE_BLE_SYNC) return;`，两条数据来源都能刷屏 |
| 中文显示成方框 | 字库仅 ASCII | 用英文短词 |
| 手机搜不到设备 | 已连接过且未断开；或该手机之前配对缓存 | 关掉蓝牙重开；或重启板子 |
| 写入没反应 | 漏了结尾 `\n` | 每条 JSON 必须以换行结束 |
| BLE 和 WiFi 同时开会变慢 | 共用 2.4G 射频 | 用蓝牙时不必连 WiFi |
| 刷屏慢/闪 | EPD 三色全刷本身要 15–20s | 正常，属硬件特性 |
| 左上角没有天气图标 | 手机没发 `code`，且 `desc` 也没法反推 | 固件已加 `code` 字段与 `descToCode()` 兜底 |
| 开机后天气是 `--` | 之前没同步过，NVS 里没缓存 | 同步一次后就会写入 NVS，之后开机直接显示上次结果 |

---

## 六、自动同步

ESP32 是 **BLE 外设**，不能主动上网，所以"自动"必须由手机端驱动。
双方配合做了三层机制：

| 机制 | 谁发起 | 说明 |
|---|---|---|
| **连上即同步** | 手机端 | `connectDevice` 成功后立刻 `syncWeather(true)` 推一次 |
| **定时同步** | 手机端 | 每 30 分钟推送一次，页面显示"下次自动同步"时间 |
| **设备请求同步** | ESP32 | 连上时 / 数据超过 30 分钟，主动 notify `{"cmd":"need_sync"}`，手机收到立即推 |
| **断线自动重连** | 手机端 | 断开后 5 秒重试，重连成功立刻补一次数据 |
| **回前台补同步** | 手机端 | 手机锁屏会节流定时器，切回页面时若数据过期就补一次 |

> ⚠️ 前提：**页面要保持在后台运行**（可以切到别的 App，但别关掉标签页）。
> 这是 Web Bluetooth 的固有限制——网页关了就没人推数据了。
> 如果要"完全无人值守"，需要改成微信小程序或原生 App。

新增字段：

```json
{"temp":23,"desc":"Sunny","code":116,"epoch":1730544000}
```

- `code`：天气代码（wttr.in 风格），驱动左上角图标；不发则由 `desc` 反推
- `epoch`：手机当前 Unix 时间戳；BLE 模式没有 NTP，用它校准设备时钟

断电重启后会从 NVS 恢复上次天气，开机直接显示，不会是一片 `--`。

---

## 七、编译与烧录

本工程**必须用这个分区方案**，否则固件 96% 会顶到上限：

```
FlashSize = 16MB
PartitionScheme = Huge APP (3MB No OTA/1MB SPIFFS)
PSRAM = Disabled（如需 PSRAM 改 OPI PSRAM，但要一并确认板卡型号）
```

命令行验证方式（等价于 IDE 里选同样的选项）：

```powershell
& 'F:\program\AIDE\Arduino IDE\resources\app\lib\backend\resources\arduino-cli.exe' `
  compile --fqbn "esp32:esp32:esp32s3:PSRAM=disabled,FlashSize=16M,PartitionScheme=huge_app" `
  'F:\industryworks\arduino\ESP32-S3-LCD-2-Demo\Arduino\examples\20_epd_weather_station'
```

---

## 八、后续可做

1. **定时同步**：加 RTC（PCF85063）或利用 EPD 掉电保留画面，做"每天早 7 点由手机自动推一次"。
2. **断连重连优化**：当前断开 300ms 后重新广播，可加白名单绑定固定手机。
3. **回传电量/信号**：TX 特征已通，可让手机端读到 ESP32 状态。
4. 若确实需要"完全脱离手机"，唯一替代是让 ESP32 自己连 2.4G WiFi——
   那就得确保路由器 2.4G 开启，或用旧手机开 2.4G 热点。
