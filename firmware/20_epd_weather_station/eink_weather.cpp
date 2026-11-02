// ============================================================
// eink_weather.cpp — BLE(GATT) 天气同步实现
// 依赖：ESP32 core 3.x 自带 BLE 库（Bluedroid），无需外装 ArduinoJson
// ============================================================
#include "eink_weather.h"

#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <stdio.h>

// ---------- Nordic UART Service ----------
#define NUS_SERVICE_UUID "6e400001-b5a3-f393-e0a9-e50e24dcca9e"
#define NUS_RX_UUID      "6e400002-b5a3-f393-e0a9-e50e24dcca9e"  // 手机 -> ESP32 (write)
#define NUS_TX_UUID      "6e400003-b5a3-f393-e0a9-e50e24dcca9e"  // ESP32 -> 手机 (notify)

#define BLE_DEVICE_NAME  "EPD-Weather"
#define RX_BUF_MAX       1024          // 单条 JSON 上限
#define LINK_TIMEOUT_MS  120000UL      // 2 分钟无交互视为链路空闲

// ---------- 内部状态 ----------
static BLEServer         *s_server   = nullptr;
static BLECharacteristic *s_txChar   = nullptr;
static volatile bool      s_connected = false;
static volatile bool s_needRestartAdv = false;
static unsigned long s_restartAt = 0;   // 0 表示未排程
static unsigned long s_lastAdvKick = 0; // 广播看门狗上次重试时刻

static char     s_rxBuf[RX_BUF_MAX];
static size_t   s_rxLen = 0;
static bool     s_haveLine = false;
static char     s_lineBuf[RX_BUF_MAX];

static volatile bool s_pending = false;   // 有一条已解析、待应用
static BleWeather    s_weather;
static char          s_lastSync[32] = "从未";
static volatile unsigned long s_lastActivity = 0;
static unsigned long s_epoch = 0;         // 手机写入的 epoch 秒
static unsigned long s_lastSyncMs = 0;    // 上次成功同步的 millis
static bool          s_syncOnConnect = true;   // 连上就请手机推一次
static unsigned long s_maxAgeMs = 1800000UL;   // 默认 30 分钟算过期

// 解析 "YYYY-MM-DD HH:MM:SS" / "YYYY-MM-DDTHH:MM:SSZ" 为 epoch（按本地时间算，够用）
static unsigned long parseTimeString(const char *s) {
    int Y = 0, M = 0, D = 0, h = 0, m = 0, sec = 0;
    if (sscanf(s, "%d-%d-%d%*c%d:%d:%d", &Y, &M, &D, &h, &m, &sec) < 5) return 0;
    if (Y < 2020) return 0;
    static const int mdays[12] = {31,28,31,30,31,30,31,31,30,31,30,31};
    unsigned long days = 0;
    for (int y = 1970; y < Y; y++) {
        days += 365;
        if ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0) days++;
    }
    bool leap = (Y % 4 == 0 && Y % 100 != 0) || (Y % 400 == 0);
    for (int i = 0; i < M - 1 && i < 12; i++) {
        days += mdays[i];
        if (i == 1 && leap) days++;
    }
    days += (D > 0 ? D - 1 : 0);
    return days * 86400UL + h * 3600UL + m * 60UL + sec;
}

// ---------- 工具 ----------
static const char *skipWs(const char *p) {
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    return p;
}

// 极简 JSON 取字段：找 "key" 后定位值，支持字符串与整数。
// 返回 true 表示找到。不递归、不处理嵌套，仅满足本协议。
static bool jsonGetString(const char *json, const char *key, char *out, size_t outSize) {
    char pat[40];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(json, pat);
    if (!p) return false;
    p += strlen(pat);
    p = skipWs(p);
    if (*p != ':') return false;
    p++;
    p = skipWs(p);
    if (*p != '"') return false;     // 不是字符串
    p++;
    size_t i = 0;
    while (*p && *p != '"' && i < outSize - 1) {
        // 简单转义：\" \\ 原样处理
        if (*p == '\\' && (*(p + 1) == '"' || *(p + 1) == '\\')) p++;
        out[i++] = *p++;
    }
    out[i] = 0;
    return true;
}

static bool jsonGetInt(const char *json, const char *key, int *out) {
    char pat[40];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(json, pat);
    if (!p) return false;
    p += strlen(pat);
    p = skipWs(p);
    if (*p != ':') return false;
    p++;
    p = skipWs(p);
    if (*p == '"') p++;              // 允许 "23" 这种带引号的数字
    if (*p != '-' && (*p < '0' || *p > '9')) return false;
    *out = atoi(p);
    return true;
}

// ---------- 解析一条完整 JSON ----------
static void applyJson(const char *json) {
    // 命令：{"cmd":"status"} 之类，只回状态不改天气
    char cmd[24] = "";
    if (jsonGetString(json, "cmd", cmd, sizeof(cmd)) && cmd[0]) {
        if (strcmp(cmd, "status") == 0) {
            char msg[200];
            snprintf(msg, sizeof(msg),
                     "{\"ok\":true,\"last\":\"%s\",\"msg\":\"status\",\"heap\":%u,\"needsync\":%s}\n",
                     s_lastSync, (unsigned)ESP.getFreeHeap(),
                     bleWantsSync(s_maxAgeMs) ? "true" : "false");
            bleNotify(msg);
        } else if (strcmp(cmd, "ping") == 0) {
            bleNotify("{\"ok\":true,\"msg\":\"pong\"}\n");
        }
        s_lastActivity = millis();
        return;
    }

    bool any = false;
    int v = 0;

    if (jsonGetInt(json, "temp", &v))  { s_weather.temp  = v; s_weather.has_temp  = true; any = true; }
    if (jsonGetInt(json, "feels", &v)) { s_weather.feels = v; s_weather.has_feels = true; any = true; }
    if (jsonGetInt(json, "hum", &v))   { s_weather.hum   = v; s_weather.has_hum   = true; any = true; }
    if (jsonGetInt(json, "uv", &v))    { s_weather.uv    = v; s_weather.has_uv    = true; any = true; }
    if (jsonGetInt(json, "tlow", &v))  { s_weather.tlow  = v; s_weather.has_tlow  = true; any = true; }
    if (jsonGetInt(json, "thigh", &v)) { s_weather.thigh = v; s_weather.has_thigh = true; any = true; }
    if (jsonGetInt(json, "fc_low", &v))  { s_weather.fc_low  = v; s_weather.has_fc_low  = true; any = true; }
    if (jsonGetInt(json, "fc_high", &v)) { s_weather.fc_high = v; s_weather.has_fc_high = true; any = true; }
    if (jsonGetInt(json, "code", &v))    { s_weather.code    = v; s_weather.has_code    = true; any = true; }

    // 可选：手机顺带把当前时间写进来（无 WiFi 时用于校准时钟）
    if (jsonGetInt(json, "epoch", &v) && v > 1600000000) {
        s_epoch = (unsigned long)v;
    } else {
        char ts[32];
        if (jsonGetString(json, "time", ts, sizeof(ts))) {
            s_epoch = parseTimeString(ts);
        }
    }

    if (jsonGetString(json, "desc", s_weather.desc, sizeof(s_weather.desc))) {
        s_weather.has_desc = true; any = true;
    }
    if (jsonGetString(json, "fc_date", s_weather.fc_date, sizeof(s_weather.fc_date))) {
        s_weather.has_fc_date = true; any = true;
    }
    if (jsonGetString(json, "fc_desc", s_weather.fc_desc, sizeof(s_weather.fc_desc))) {
        s_weather.has_fc_desc = true; any = true;
    }

    // ---- 中文建议（UTF-8 多字节，jsonGetString 按字节拷贝，不破坏编码）----
    if (jsonGetString(json, "a1", s_weather.a1, sizeof(s_weather.a1))) {
        s_weather.has_a1 = true; any = true;
    }
    if (jsonGetString(json, "a2", s_weather.a2, sizeof(s_weather.a2))) {
        s_weather.has_a2 = true; any = true;
    }
    if (jsonGetString(json, "a3", s_weather.a3, sizeof(s_weather.a3))) {
        s_weather.has_a3 = true; any = true;
    }
    if (jsonGetString(json, "fclabel", s_weather.fclabel, sizeof(s_weather.fclabel))) {
        s_weather.has_fclabel = true; any = true;
    }

    s_lastActivity = millis();

    if (!any) {
        bleNotify("{\"ok\":false,\"msg\":\"no known field\"}\n");
        return;
    }
    s_pending = true;
    Serial.printf("[BLE] 收到天气: temp=%d feel=%d desc=%s hum=%d\n",
                  s_weather.temp, s_weather.feels, s_weather.desc, s_weather.hum);
}

// ---------- 收数据回调 ----------
class RxCallbacks : public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic *c) override {
        // 注意：esp32 core 3.3.x 的 getValue() 返回 Arduino String（不是 std::string）
        String s = c->getValue();
        size_t n = s.length();
        if (n == 0) return;
        s_lastActivity = millis();

        // 追加到缓冲，按 '\n' 切分；若没换行且缓冲满，也当成一条处理
        for (size_t i = 0; i < n; i++) {
            char ch = s[i];
            if (ch == '\n' || ch == '\r') {
                if (s_rxLen > 0) {
                    s_rxBuf[s_rxLen] = 0;
                    strncpy(s_lineBuf, s_rxBuf, sizeof(s_lineBuf) - 1);
                    s_lineBuf[sizeof(s_lineBuf) - 1] = 0;
                    s_rxLen = 0;
                    s_haveLine = true;
                }
            } else {
                if (s_rxLen < RX_BUF_MAX - 1) s_rxBuf[s_rxLen++] = ch;
                else s_rxLen = 0;   // 溢出丢弃，避免半条 JSON
            }
        }
    }
};

// ---------- 连接状态回调 ----------
class ServerCallbacks : public BLEServerCallbacks {
    void onConnect(BLEServer *srv) override {
        s_connected = true;
        s_lastActivity = millis();
        Serial.println("[BLE] 手机已连接");
        bleNotify("{\"ok\":true,\"msg\":\"connected\"}\n");
        // 主动告诉手机"我这边数据过期了，请推一次"，实现无人值守自动同步
        if (s_syncOnConnect) {
            bleNotify("{\"cmd\":\"need_sync\",\"reason\":\"connected\"}\n");
        } else if (millis() - s_lastSyncMs > s_maxAgeMs) {
            bleNotify("{\"cmd\":\"need_sync\",\"reason\":\"stale\"}\n");
        }
    }
    void onDisconnect(BLEServer *srv) override {
        s_connected = false;
        s_needRestartAdv = true;
        Serial.println("[BLE] 手机断开，重新广播");
    }
};

// ---------- 对外接口 ----------
void bleBegin() {
    BLEDevice::init(BLE_DEVICE_NAME);
    BLEDevice::setMTU(500);          // 争取更大 MTU，减少分片

    s_server = BLEDevice::createServer();
    s_server->setCallbacks(new ServerCallbacks());

    BLEService *svc = s_server->createService(NUS_SERVICE_UUID);

    s_txChar = svc->createCharacteristic(
        NUS_TX_UUID,
        BLECharacteristic::PROPERTY_NOTIFY | BLECharacteristic::PROPERTY_READ);
    s_txChar->addDescriptor(new BLE2902());   // 使能被订阅(notify)

    BLECharacteristic *rxChar = svc->createCharacteristic(
        NUS_RX_UUID,
        BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR);
    rxChar->setCallbacks(new RxCallbacks());

    svc->start();

    BLEAdvertising *adv = BLEDevice::getAdvertising();
    adv->addServiceUUID(NUS_SERVICE_UUID);
    adv->setScanResponse(true);
    adv->setMinPreferred(0x06);   // 低值优先：连接间隔更短，手机端更跟手
    BLEDevice::startAdvertising();

    s_lastActivity = millis();
    s_lastAdvKick = millis();
    Serial.println("[BLE] 广播中，设备名: EPD-Weather");
}

void bleLoop() {
    // 处理凑满的一整行 JSON
    if (s_haveLine) {
        s_haveLine = false;
        // 直接在主循环（非 BLE 回调任务）解析并置位，避免跨任务改动显示状态
        applyJson(s_lineBuf);
    }
    // 断开后重启广播（放在 loop 里而不是回调里，避免在协议栈任务里做重活）
    // 用时间戳做延时，不要 delay()：loop 里还有网页服务要跑
    if (s_needRestartAdv) {
        if (s_restartAt == 0) {
            s_restartAt = millis();
        } else if (millis() - s_restartAt > 300) {
            s_restartAt = 0;
            s_needRestartAdv = false;
            BLEDevice::startAdvertising();
            s_lastAdvKick = millis();
            Serial.println("[BLE] 已重新广播");
        }
    }

    // ---------- 广播看门狗 ----------
    // arduino-esp32 3.3.x 里 startAdvertising() 在断开连接后偶发“静默失败”：
    // 函数返回了但设备其实不再广播，结果主机永远搜不到设备（表现就是“连不上”）。
    // 这里每 5 秒无条件补一次广播请求；已经在广播时该调用是幂等的，无副作用。
    if (!s_connected && millis() - s_lastAdvKick > 5000UL) {
        s_lastAdvKick = millis();
        BLEDevice::startAdvertising();
    }
}

bool bleConnected() { return s_connected; }

bool bleLinkActive() {
    return s_connected && (millis() - s_lastActivity < LINK_TIMEOUT_MS);
}

bool bleHasPendingWeather() { return s_pending; }

void bleConsumeReady() {
    s_pending = false;
    // 只清"已应用"标记，字段值保留（下次只发改动字段也能正确合并）
    s_weather = BleWeather();
    s_weather.has_temp = false;
}

const BleWeather &bleWeather() { return s_weather; }

unsigned long bleTakeEpoch() {
    unsigned long e = s_epoch;
    s_epoch = 0;
    return e;
}

void bleMarkSynced(const char *whenStr) {
    if (whenStr && whenStr[0]) {
        strncpy(s_lastSync, whenStr, sizeof(s_lastSync) - 1);
        s_lastSync[sizeof(s_lastSync) - 1] = 0;
    }
    s_lastSyncMs = millis();
    s_syncOnConnect = false;    // 这次连上已经同步过了
}

bool bleWantsSync(unsigned long maxAgeMs) {
    s_maxAgeMs = maxAgeMs;
    if (!s_connected) return false;
    return s_syncOnConnect || (millis() - s_lastSyncMs > maxAgeMs);
}

void bleRequestSyncOnConnect(bool enable) {
    s_syncOnConnect = enable;
}

void bleNotify(const char *msg) {
    if (!s_txChar || !msg) return;
    size_t len = strlen(msg);
    if (len > 500) len = 500;
    s_txChar->setValue((uint8_t *)msg, len);
    s_txChar->notify();
}
