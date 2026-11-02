// ============================================================
// eink_weather.h — EPD 墨水屏天气站 · 蓝牙(BLE) 同步模块
// ------------------------------------------------------------
// 设计前提（务必先读）：
//   ESP32-S3 只有 Bluetooth LE（BLE 5.0），【没有蓝牙经典 BR/EDR】。
//   因此：
//     ✗ 蓝牙网络共享 / Bluetooth PAN（手机共享蓝牙网络）—— 不可行，PAN 走的是经典蓝牙 BNEP
//     ✗ BluetoothSerial（SPP 串口）—— S3 上不存在该协议栈
//     ✓ 唯一可行：ESP32 做 GATT Server（外设），手机做 Client 把天气写进来
//
// 采用 Nordic UART Service (NUS) UUID，好处是 nRF Connect / BLE 串口助手
// 等通用 App 无需改代码即可直接连；也便于自写 Web Bluetooth / 小程序。
//
// 载荷：UTF-8 JSON，以 '\n' 结尾（可分片）。
// ============================================================
#pragma once
#include <Arduino.h>

// 启动 BLE 广播（须在 setup 中、EPD 初始化之后、进入主循环之前调用一次）
void bleBegin();

// 主循环里轮询：处理新写入的数据、应用天气、发状态回包
void bleLoop();

// 是否有手机连上来（用于省电策略与状态显示）
bool bleConnected();

// 手机是否还"活着"（最近 N 秒内有任何交互）
bool bleLinkActive();

// 取走一条已经解析好、待应用到屏幕的天气指令；返回 true 表示有新数据
// 用完必须调用 bleConsumeReady() 释放
bool bleHasPendingWeather();
void bleConsumeReady();

// 供显示层读取的字段（与旧 weather_* 全局变量并行，互不冲突）
struct BleWeather {
    bool has_temp  = false;  int  temp  = 0;
    bool has_feels = false;  int  feels = 0;
    bool has_hum   = false;  int  hum   = 0;
    bool has_uv    = false;  int  uv    = 0;
    bool has_tlow  = false;  int  tlow  = 0;
    bool has_thigh = false;  int  thigh = 0;
    bool has_desc  = false;  char desc[20]    = "";
    bool has_fc_date = false; char fc_date[8]  = "";
    bool has_fc_desc = false; char fc_desc[20] = "";
    bool has_fc_low  = false; int  fc_low  = 0;
    bool has_fc_high = false; int  fc_high = 0;
    bool has_code    = false; int  code    = 0;   // 天气代码（天气图标用）

    // ---- 中文建议（UTF-8，每行 ≤10 个字符）----
    bool has_a1 = false; char a1[48] = "";
    bool has_a2 = false; char a2[48] = "";
    bool has_a3 = false; char a3[48] = "";
    bool has_fclabel = false; char fclabel[24] = "";
};
const BleWeather &bleWeather();

// 手机写进来的时间（epoch 秒）；0 表示本次没有
unsigned long bleTakeEpoch();

// 记录一次成功同步的时间（用于状态回包与界面）
void bleMarkSynced(const char *whenStr);

// 发送一条 notify 文本（手机端订阅 TX 特征后可收到）
void bleNotify(const char *msg);

// 天气是否需要刷新：手机连上后请求一次，或距上次同步超过 maxAgeMs
// 返回 true 时手机端应立刻重新取天气并写入
bool bleWantsSync(unsigned long maxAgeMs);

// 标记"手机已连上，请推一次天气"，由连接回调内部置位
void bleRequestSyncOnConnect(bool enable);
