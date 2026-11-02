# EPD 天气同步服务（电脑端）

让电脑开机自动把天气推给墨水屏，不用手动操作、不用开手机。

- 常驻内存 **约 5–45 MB**（其中 40 MB 是 Windows BLE/WinRT 协议栈，Python 本体仅 ~5 MB）
- 默认 **60 分钟**同步一次；天气没变化时**跳过写屏**（保护墨水屏寿命）
- 断网/找不到设备会自动退避重试（30s → 1m → 2m → 5m → 10m）

---

## 一、安装

```powershell
cd service

# 1. 独立虚拟环境（不污染系统 Python）
python -m venv .venv
.\.venv\Scripts\python.exe -m pip install bleak httpx `
  -i https://pypi.tuna.tsinghua.edu.cn/simple

# 2. 安装（会先试跑一次验证链路）
.\install_task.ps1 -City 杭州
```

脚本会依次：检查环境 → 写配置 → **试跑一次** → 注册计划任务。

装完就完事了，重启电脑会自动运行。

### 常用命令

```powershell
.\install_task.ps1 -Status                       # 状态 + 内存 + 最近日志
.\install_task.ps1 -Uninstall                    # 卸载
.\install_task.ps1 -City 上海 -Interval 30       # 改城市/间隔并重装
```

---

## 二、城市怎么设置（重点）

**电脑服务和手机网页的城市是两套，互不影响**——它们跑在不同设备上：

| 谁在推天气 | 城市存在哪 |
|---|---|
| **本服务**（电脑常驻） | `service/config.json` 的 `city` |
| 手机网页 | 浏览器 localStorage |

**设备端不存城市**，只接收最终结果。谁推就按谁的城市。

改本服务的城市：

```powershell
# ① 编辑配置（最简单）
notepad config.json          # 把 "city": "Hangzhou" 改成 "city": "杭州"
Stop-ScheduledTask  -TaskName EPD-Weather-Sync
Start-ScheduledTask -TaskName EPD-Weather-Sync

# ② 重装时指定
.\install_task.ps1 -City 杭州

# ③ 临时手动跑一次验证
.\.venv\Scripts\python.exe epd_sync.py --once --city 上海
```

> 城市名支持中文（`杭州`）和拼音（`Hangzhou`）。
> 查不到会报 `找不到城市`，换成更通用的名称。

### 配置项

`config.json`（可从 `config.example.json` 复制）：

| 字段 | 默认 | 说明 |
|---|---|---|
| `city` | `Hangzhou` | 城市名 |
| `interval_minutes` | `60` | 同步间隔（分钟） |
| `device_address` | `""` | 首次连上后自动填入；填了免扫描，更快 |
| `device_name` | `EPD-Weather` | 设备广播名 |
| `scan_timeout_s` | `12` | 扫描超时 |
| `only_when_changed` | `true` | 天气无变化时不写屏 |
| `retry_backoff_s` | `[30,60,120,300,600]` | 失败退避序列 |
| `startup_delay_s` | `8` | 开机后等多久再开始 |

---

## 三、为什么不是「Windows 服务」

你要的可能是注册成 Windows 服务（SYSTEM 账户），但**这套方案在蓝牙上跑不通**：

| 问题 | 说明 |
|---|---|
| BLE 走 WinRT | Windows 上操作 BLE 用 `Windows.Devices.Bluetooth`，属于**用户会话**的蓝牙运行时 |
| 服务在 Session 0 | 服务没有用户桌面会话，枚举不到蓝牙设备，扫描直接失败 |
| 绕过代价大 | 要写 WinRT 打包组件或 IPC 中转，脆弱且易被系统更新打断 |

所以用**「用户登录时启动的隐藏任务」**——效果一样（开机自动跑、没窗口、不用管），
只是挂在你的用户账户下而不是 SYSTEM。

> 这也意味着：**必须先登录 Windows 它才会启动**。
> 需要「开机未登录也运行」只能改走 WiFi 方案（ESP32 自己联网），功耗高很多。

---

## 四、自动同步机制

ESP32 是 BLE 外设，不能主动上网，所以「自动」由本服务驱动：

| 机制 | 说明 |
|---|---|
| **登录自启** | 开机登录后自动运行 |
| **定时同步** | 每 60 分钟一次（可配） |
| **设备请求** | ESP32 数据过期会 notify `need_sync`，服务立即推 |
| **失败退避** | 30s → 1m → 2m → 5m → 10m |
| **天气去重** | 天气无变化就不写屏，省墨水屏寿命 |
| **网络重试** | 取天气失败就地重试 3 次（1.5s/3s），不因单次抖动退避 |
| **任务自愈** | 每 30 分钟检查一次，进程挂了自动拉起 |

---

## 五、资源占用实测

| 项目 | 数值 |
|---|---|
| 稳定态内存 | **≈ 44 MB**（Python 本体 4.7 MB + BLE/WinRT 栈 39 MB） |
| 空闲 CPU | ≈ 0.00s（只在同步/扫描时短暂占用） |
| 网络 | 每小时 2 个请求，约几十 KB |
| 磁盘 | 日志 1MB×3 轮转，不会无限增长 |

> 那 ~40 MB 是 **Windows 自己加载的蓝牙运行时**，任何用 bleak 的程序都一样，
> 换 C#/Rust 也省不掉。

---

## 六、排查

日志：`logs/epd-sync.log`

| 日志表现 | 原因 | 处理 |
|---|---|---|
| `没找到 EPD-Weather` | 没上电 / 被手机连着 / 固件刚断连 | 确认上电、手机断开 |
| `BLE 错误：...` | 适配器被占用 | 关掉其它连 BLE 的程序 |
| `找不到城市` | 城市名不认识 | 换更通用的名称 |
| `天气无变化，跳过写屏` | **正常行为**，不是错误 | 无需处理 |
| 一直成功但屏幕不刷新 | 固件版本旧 | 重刷固件 |
| 服务没在跑 | 被停过 | `.\install_task.ps1 -Status` 然后 `Start-ScheduledTask EPD-Weather-Sync` |

手动跑一次看详细输出：

```powershell
.\.venv\Scripts\python.exe epd_sync.py --once
```

---

## 七、实现要点

- **bleak**（Windows 走 WinRT，无需装驱动/Zadig）
- BLE 跑在独立线程的 asyncio 事件循环里，主线程只做调度
- 按「设备名 → NUS 服务 UUID → 名字前缀」三级匹配找设备
- 整包写入失败时退化为 180 字节分片
- 成功连上后把 MAC 记进 `config.json`，之后免扫描直连
- 用 `pythonw.exe` 启动，无控制台窗口

### 文件

```
service/
├─ epd_sync.py           # 同步主程序
├─ advice.py             # 中文建议逻辑（也是字表的唯一真相来源）
├─ install_task.ps1      # 安装/卸载/状态
├─ config.example.json   # 配置模板
├─ config.json           # 实际配置（自动生成，不入库）
├─ state.json            # 上次天气缓存（自动生成，不入库）
└─ logs/                 # 轮转日志（自动生成，不入库）
```

---

## 八、手机端替代方案

不方便常开电脑时，可用仓库 **`EPD-BLE-SYNC`** 里的 Web Bluetooth 网页：
手机 Chrome 打开即用，无需安装。

| | 电脑服务 | 手机网页 |
|---|---|---|
| 自动同步 | ✅ 开机常驻 | ⚠️ 页面要保持后台 |
| iOS 支持 | — | ❌ Safari 未实现 Web Bluetooth |
| 免安装 | 需装 Python | ✅ 发个网址即可 |

两者可同时使用，互不冲突（同时只允许一个连接，谁先连谁用）。
