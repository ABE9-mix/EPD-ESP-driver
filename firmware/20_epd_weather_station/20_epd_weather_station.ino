// ============================================================
// EPD 墨水屏天气站 - v3 (BLE 同步版)
//
// v3 变更：天气数据改为【蓝牙 BLE 同步】，不再依赖 2.4G WiFi 上网取天气。
//   原因：ESP32-S3 的 WiFi 仅支持 2.4GHz，而现代路由器多为 5GHz-only，
//         连不上 2.4G 热点就无法通过 wttr.in 取天气。
//   注意：ESP32-S3 【只有 BLE，没有蓝牙经典】→ 手机"蓝牙网络共享"(PAN) 不可行，
//         所以采用 BLE GATT 写入：手机取到天气后写入本机（见 eink_weather.cpp）。
//   WiFi 功能保留但降级为可选：仍可用于网页配置/图片上传（需能连 2.4G）。
//
// 屏幕：PDI 2.13" 三色墨水屏 (212x104)
// 显示逻辑：基于 19_epd_orient_test（连续位流寻址 + 0x12,0x00 刷新）
// ============================================================

// ========== 功能开关 ==========
#define ENABLE_BLE_SYNC        1   // ★ 蓝牙同步（主路径）
#define ENABLE_WIFI_CONFIG     1   // WiFi 网页配置/上传（可选，能连 2.4G 时才用）
#define ENABLE_WEATHER_SYNC    0   // 旧的 WiFi 取天气（wttr.in），默认关闭
#define ENABLE_IMAGE_UPLOAD    1

// ========== 头文件 ==========
#include <SPI.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ESPmDNS.h>
#include "web_pages.h"
#include "epd_layout.h"      // v4 双栏布局 + 方舟像素 12px 字库渲染
#if ENABLE_BLE_SYNC
#include "eink_weather.h"
#endif

// ========== 引脚定义 ==========
#define PIN_CS    10
#define PIN_DC    13
#define PIN_RST   14
#define PIN_BUSY  15
#define PIN_MOSI  11
#define PIN_SCLK  12

// ========== 屏幕参数 ==========
#define WIDTH   212
#define HEIGHT  104
#define FSIZE   (WIDTH * HEIGHT / 8)   // 2756

// ========== 全局帧缓冲 ==========
uint8_t bw[FSIZE];
uint8_t re[FSIZE];

// ========== NVS 命名空间 ==========
#define NVS_NS "epd"

// ========== 全局状态 ==========
Preferences prefs;
WebServer server(80);

// 配置参数
char cfg_ssid[32] = "";
char cfg_pass[64] = "";
char cfg_city[32] = "Beijing";
int cfg_refresh_min = 30;
bool cfg_weather = true;
bool cfg_image   = true;

// 运行状态
bool wifi_connected = false;
bool in_ap_mode = false;
unsigned long ap_mode_start = 0;
unsigned long last_weather_fetch = 0;
unsigned long last_ntp_sync = 0;
unsigned long weather_refresh_interval = 1800000; // 30分钟，由 cfg_refresh_min 控制
unsigned long image_display_time = 0;
bool showing_image = false;
int upload_result_code = 0;
char upload_result_msg[64] = "";

// 时间
int current_hour = 0, current_min = 0, current_sec = 0;
int current_day = 0, current_month = 0, current_year = 0;
bool time_synced = false;

// 天气数据
char weather_temp[8] = "--";
char weather_feels[8] = "--";
char weather_desc[32] = "--";
char weather_hum[8] = "--";
char weather_wind[24] = "--";
char weather_pressure[8] = "--";
char weather_uv[4] = "--";
char weather_thigh[8] = "--";
char weather_tlow[8] = "--";
int weather_code = 0;

// 3天预报
char weather_fc1_date[6] = "";
char weather_fc1_desc[16] = "";
char weather_fc1_high[8] = "";
char weather_fc1_low[8] = "";
char weather_fc2_date[6] = "";
char weather_fc2_desc[16] = "";
char weather_fc2_high[8] = "";
char weather_fc2_low[8] = "";

// 状态记录
char last_image_time[32] = "从未";
char last_weather_time[32] = "从未";

// ===== v4 双栏布局：中文建议（由手机写入，UTF-8）=====
// 三条建议各最多 10 个汉字（=120px，右栏宽 132px）
char advice_line1[40] = "";
char advice_line2[40] = "";
char advice_line3[40] = "";
char advice_forecast_label[16] = "";

// ============================================================
// 1. EPD 底层驱动（基于 19 的正确逻辑）
// ============================================================
void epd_cmd(uint8_t c) { digitalWrite(PIN_DC, LOW);  digitalWrite(PIN_CS, LOW); SPI.transfer(c); digitalWrite(PIN_CS, HIGH); }
void epd_dat(uint8_t d) { digitalWrite(PIN_DC, HIGH); digitalWrite(PIN_CS, LOW); SPI.transfer(d); digitalWrite(PIN_CS, HIGH); }
void epd_buf(const uint8_t *b, uint32_t l) { digitalWrite(PIN_DC, HIGH); digitalWrite(PIN_CS, LOW); for (uint32_t i=0; i<l; i++) SPI.transfer(b[i]); digitalWrite(PIN_CS, HIGH); }
void epd_wait() { while (digitalRead(PIN_BUSY) == LOW) delay(10); }

void epd_init() {
  digitalWrite(PIN_RST, LOW); delay(20); digitalWrite(PIN_RST, HIGH); delay(40); epd_wait();
  epd_cmd(0x00); epd_dat(0x0E); epd_wait();
  epd_cmd(0xE5); epd_dat(0x19);
  epd_cmd(0xE0); epd_dat(0x02);
  epd_cmd(0x00); epd_dat(0xCF);  // PSR = 0xCF（官方库标准）
}

void epd_refresh() {
  epd_cmd(0x10); epd_buf(bw, FSIZE);
  epd_cmd(0x13); epd_buf(re, FSIZE);
  epd_cmd(0x04); epd_dat(0x00); epd_wait();
  epd_cmd(0x12); epd_dat(0x00);  // 官方库刷新参数
  epd_wait();
  epd_cmd(0x02); epd_dat(0x00); epd_wait();
}

// ============================================================
// 2. 绘图函数（连续位流寻址）
// ============================================================
void clearAll() { memset(bw, 0, FSIZE); memset(re, 0, FSIZE); }
void fillScreen(int c) {
  if (c == 0) { memset(bw, 0, FSIZE); memset(re, 0, FSIZE); }
  else if (c == 1) { memset(bw, 0xFF, FSIZE); memset(re, 0, FSIZE); }
  else if (c == 2) { memset(bw, 0, FSIZE); memset(re, 0xFF, FSIZE); }
}

// 颜色: 0=白, 1=黑, 2=红
void setPx(int x, int y, int c) {
  if (x < 0 || x >= WIDTH || y < 0 || y >= HEIGHT) return;
  int i = (y * WIDTH + x) / 8;
  uint8_t m = 0x80 >> ((y * WIDTH + x) % 8);
  if (c == 1) { bw[i] |= m; re[i] &= ~m; }
  else if (c == 0) { bw[i] &= ~m; re[i] &= ~m; }
  else if (c == 2) { bw[i] &= ~m; re[i] |= m; }
}

void fillRect(int x, int y, int w, int h, int c) {
  for (int r = 0; r < h; r++)
    for (int cl = 0; cl < w; cl++)
      setPx(x + cl, y + r, c);
}

void drawRect(int x, int y, int w, int h, int c) {
  for (int i = 0; i < w; i++) { setPx(x + i, y, c); setPx(x + i, y + h - 1, c); }
  for (int i = 0; i < h; i++) { setPx(x, y + i, c); setPx(x + w - 1, y + i, c); }
}

void drawLine(int x1, int y1, int x2, int y2, int c) {
  if (y1 == y2) for (int x = min(x1,x2); x <= max(x1,x2); x++) setPx(x, y1, c);
  else if (x1 == x2) for (int y = min(y1,y2); y <= max(y1,y2); y++) setPx(x1, y, c);
}

void drawCircle(int cx, int cy, int r, int c, bool fill) {
  for (int y = -r; y <= r; y++)
    for (int x = -r; x <= r; x++) {
      int d = x*x + y*y;
      if (fill) { if (d <= r*r) setPx(cx+x, cy+y, c); }
      else { if (d > (r-1)*(r-1) && d <= r*r) setPx(cx+x, cy+y, c); }
    }
}

// ============================================================
// 3. 8x13 ASCII 字体（行主序，每字节一个行，MSB=左）
// ============================================================
static const uint8_t font8x13[] PROGMEM = {
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00, // space
    0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x00,0x00,0x18,0x18,0x00,0x00, // !
    0x36,0x36,0x36,0x36,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00, // "
    0x00,0x00,0x66,0x66,0xff,0x66,0x66,0xff,0x66,0x66,0x00,0x00,0x00, // #
    0x18,0x7e,0xff,0xd8,0xf8,0x7e,0x1f,0x1b,0xff,0x7e,0x18,0x00,0x00, // $
    0x70,0xd8,0xdb,0x76,0x0c,0x18,0x30,0x6e,0xdb,0x1b,0x0e,0x00,0x00, // %
    0x38,0x6c,0xcc,0xcc,0xd8,0x70,0x70,0xd8,0xcf,0xc6,0x7f,0x00,0x00, // &
    0x0e,0x0c,0x1c,0x18,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00, // '
    0x0c,0x18,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x18,0x0c,0x00,0x00, // (
    0x30,0x18,0x0c,0x0c,0x0c,0x0c,0x0c,0x0c,0x0c,0x18,0x30,0x00,0x00, // )
    0x00,0x00,0x99,0x5a,0x3c,0xff,0x3c,0x5a,0x99,0x00,0x00,0x00,0x00, // *
    0x00,0x00,0x18,0x18,0x18,0xff,0xff,0x18,0x18,0x18,0x00,0x00,0x00, // +
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x1c,0x1c,0x18,0x30,0x00,0x00, // ,
    0x00,0x00,0x00,0x00,0x00,0xff,0xff,0x00,0x00,0x00,0x00,0x00,0x00, // -
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x38,0x38,0x00,0x00,0x00, // .
    0x03,0x03,0x06,0x06,0x0c,0x0c,0x18,0x18,0x30,0x30,0x60,0x60,0x00, // /
    0x3c,0x66,0xc3,0xc7,0xcf,0xdb,0xf3,0xe3,0xc3,0x66,0x3c,0x00,0x00, // 0
    0x18,0x38,0x78,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x7e,0x00,0x00, // 1
    0x7e,0xe7,0x03,0x06,0x0c,0x18,0x30,0x60,0xc0,0xc0,0xff,0x00,0x00, // 2
    0x7e,0xe7,0x03,0x03,0x07,0x7e,0x07,0x03,0x03,0xe7,0x7e,0x00,0x00, // 3
    0x0c,0x1c,0x3c,0x6c,0xcc,0xff,0x0c,0x0c,0x0c,0x0c,0x0c,0x00,0x00, // 4
    0xff,0xc0,0xc0,0xc0,0xc0,0xfe,0x07,0x03,0x03,0xe7,0x7e,0x00,0x00, // 5
    0x7e,0xe7,0xc0,0xc0,0xc0,0xfe,0xc7,0xc3,0xc3,0xe7,0x7e,0x00,0x00, // 6
    0xff,0x03,0x03,0x03,0x06,0x0c,0x18,0x30,0x30,0x30,0x30,0x00,0x00, // 7
    0x7e,0xe7,0xc3,0xc3,0xe7,0x7e,0xe7,0xc3,0xc3,0xe7,0x7e,0x00,0x00, // 8
    0x7e,0xe7,0xc3,0xc3,0xe7,0x7f,0x03,0x03,0x03,0xe7,0x7e,0x00,0x00, // 9
    0x00,0x00,0x00,0x00,0x38,0x38,0x00,0x00,0x38,0x38,0x00,0x00,0x00, // :
    0x00,0x00,0x00,0x1c,0x1c,0x00,0x00,0x1c,0x1c,0x18,0x30,0x00,0x00, // ;
    0x06,0x0c,0x18,0x30,0x60,0xc0,0x60,0x30,0x18,0x0c,0x06,0x00,0x00, // <
    0x00,0x00,0x00,0x00,0xff,0xff,0x00,0xff,0xff,0x00,0x00,0x00,0x00, // =
    0x60,0x30,0x18,0x0c,0x06,0x03,0x06,0x0c,0x18,0x30,0x60,0x00,0x00, // >
    0x7e,0xc3,0xc3,0x03,0x06,0x0c,0x18,0x18,0x00,0x00,0x18,0x00,0x00, // ?
    0x00,0x00,0x00,0x7e,0xc3,0xdd,0xd3,0xdb,0xcf,0x60,0x3f,0x00,0x00, // @
    0x18,0x3c,0x66,0xc3,0xc3,0xc3,0xff,0xc3,0xc3,0xc3,0xc3,0x00,0x00, // A
    0xfe,0xc7,0xc3,0xc3,0xc7,0xfe,0xc7,0xc3,0xc3,0xc7,0xfe,0x00,0x00, // B
    0x7e,0xe7,0xc0,0xc0,0xc0,0xc0,0xc0,0xc0,0xc0,0xe7,0x7e,0x00,0x00, // C
    0xfc,0xce,0xc7,0xc3,0xc3,0xc3,0xc3,0xc3,0xc7,0xce,0xfc,0x00,0x00, // D
    0xff,0xc0,0xc0,0xc0,0xc0,0xfc,0xc0,0xc0,0xc0,0xc0,0xff,0x00,0x00, // E
    0xff,0xc0,0xc0,0xc0,0xfc,0xc0,0xc0,0xc0,0xc0,0xc0,0xc0,0x00,0x00, // F
    0x7e,0xe7,0xc0,0xc0,0xc0,0xc0,0xcf,0xc3,0xc3,0xe7,0x7e,0x00,0x00, // G
    0xc3,0xc3,0xc3,0xc3,0xc3,0xff,0xc3,0xc3,0xc3,0xc3,0xc3,0x00,0x00, // H
    0x7e,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x7e,0x00,0x00, // I
    0x06,0x06,0x06,0x06,0x06,0x06,0x06,0x06,0xc6,0xee,0x7c,0x00,0x00, // J
    0xc3,0xc6,0xcc,0xd8,0xf0,0xe0,0xf0,0xd8,0xcc,0xc6,0xc3,0x00,0x00, // K
    0xc0,0xc0,0xc0,0xc0,0xc0,0xc0,0xc0,0xc0,0xc0,0xc0,0xff,0x00,0x00, // L
    0xc3,0xe7,0xff,0xff,0xdb,0xc3,0xc3,0xc3,0xc3,0xc3,0xc3,0x00,0x00, // M
    0xe3,0xe3,0xf3,0xf3,0xfb,0xdb,0xdf,0xcf,0xcf,0xc7,0xc7,0x00,0x00, // N
    0x7e,0xe7,0xc3,0xc3,0xc3,0xc3,0xc3,0xc3,0xc3,0xe7,0x7e,0x00,0x00, // O
    0xfe,0xc7,0xc3,0xc3,0xc7,0xfe,0xc0,0xc0,0xc0,0xc0,0xc0,0x00,0x00, // P
    0x3c,0x66,0xc3,0xc3,0xc3,0xc3,0xc3,0xdb,0xdf,0x6e,0x3f,0x00,0x00, // Q
    0xfe,0xc7,0xc3,0xc3,0xc7,0xfe,0xf0,0xd8,0xcc,0xc6,0xc3,0x00,0x00, // R
    0x7e,0xe7,0xc0,0xc0,0xe0,0x7e,0x07,0x03,0x03,0xe7,0x7e,0x00,0x00, // S
    0xff,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x00,0x00, // T
    0xc3,0xc3,0xc3,0xc3,0xc3,0xc3,0xc3,0xc3,0xc3,0xe7,0x7e,0x00,0x00, // U
    0xc3,0xc3,0xc3,0xc3,0xc3,0xc3,0x66,0x66,0x3c,0x3c,0x18,0x00,0x00, // V
    0xc3,0xc3,0xc3,0xc3,0xc3,0xdb,0xdb,0xff,0xff,0xe7,0xc3,0x00,0x00, // W
    0xc3,0x66,0x66,0x3c,0x3c,0x18,0x3c,0x3c,0x66,0x66,0xc3,0x00,0x00, // X
    0xc3,0x66,0x66,0x3c,0x3c,0x18,0x18,0x18,0x18,0x18,0x18,0x00,0x00, // Y
    0xff,0x03,0x03,0x06,0x0c,0x7e,0x30,0x60,0xc0,0xc0,0xff,0x00,0x00, // Z
    0x3c,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x3c,0x00,0x00, // [
    0x60,0x60,0x30,0x30,0x18,0x18,0x0c,0x0c,0x06,0x06,0x03,0x03,0x00, // backslash
    0x3c,0x0c,0x0c,0x0c,0x0c,0x0c,0x0c,0x0c,0x0c,0x0c,0x3c,0x00,0x00, // ]
    0x18,0x3c,0x66,0xc3,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00, // ^
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xff,0xff, // _
    0x70,0x30,0x38,0x18,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00, // `
    0x00,0x00,0x00,0x00,0x7e,0xc3,0x03,0x7f,0xc3,0xc3,0x7f,0x00,0x00, // a
    0xc0,0xc0,0xc0,0xc0,0xc0,0xfe,0xc3,0xc3,0xc3,0xc3,0xfe,0x00,0x00, // b
    0x00,0x00,0x00,0x00,0x7e,0xc3,0xc0,0xc0,0xc0,0xc3,0x7e,0x00,0x00, // c
    0x03,0x03,0x03,0x03,0x03,0x7f,0xc3,0xc3,0xc3,0xc3,0x7f,0x00,0x00, // d
    0x00,0x00,0x00,0x00,0x7e,0xc3,0xc3,0xfe,0xc0,0xc0,0x7f,0x00,0x00, // e
    0x1e,0x33,0x30,0x30,0x30,0xfc,0x30,0x30,0x30,0x30,0x30,0x00,0x00, // f
    0x00,0x00,0x00,0x00,0x7e,0xc3,0xc3,0xc3,0x7f,0x03,0x03,0xc3,0x7e, // g
    0xc0,0xc0,0xc0,0xc0,0xfe,0xc3,0xc3,0xc3,0xc3,0xc3,0xc3,0x00,0x00, // h
    0x00,0x18,0x00,0x00,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x00,0x00, // i
    0x00,0x0c,0x00,0x00,0x0c,0x0c,0x0c,0x0c,0x0c,0x0c,0x0c,0x6c,0x38, // j
    0xc0,0xc0,0xc0,0xc0,0xc6,0xcc,0xd8,0xf0,0xf8,0xcc,0xc6,0x00,0x00, // k
    0x78,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x7e,0x00,0x00, // l
    0x00,0x00,0x00,0x00,0xfe,0xdb,0xdb,0xdb,0xdb,0xdb,0xdb,0x00,0x00, // m
    0x00,0x00,0x00,0x00,0xfc,0xc6,0xc6,0xc6,0xc6,0xc6,0xc6,0x00,0x00, // n
    0x00,0x00,0x00,0x00,0x7c,0xc6,0xc6,0xc6,0xc6,0xc6,0x7c,0x00,0x00, // o
    0x00,0x00,0x00,0x00,0xfe,0xc3,0xc3,0xc3,0xc3,0xfe,0xc0,0xc0,0xc0, // p
    0x00,0x00,0x00,0x00,0x7f,0xc3,0xc3,0xc3,0xc3,0x7f,0x03,0x03,0x03, // q
    0x00,0x00,0x00,0x00,0xfe,0xe0,0xc0,0xc0,0xc0,0xc0,0xc0,0x00,0x00, // r
    0x00,0x00,0x00,0x00,0x7f,0xc0,0xc0,0x7e,0x03,0x03,0xfe,0x00,0x00, // s
    0x00,0x30,0x30,0x30,0xfc,0x30,0x30,0x30,0x30,0x36,0x1c,0x00,0x00, // t
    0x00,0x00,0x00,0x00,0xc6,0xc6,0xc6,0xc6,0xc6,0xc6,0x7e,0x00,0x00, // u
    0x00,0x00,0x00,0x00,0xc3,0xc3,0x66,0x66,0x3c,0x3c,0x18,0x00,0x00, // v
    0x00,0x00,0x00,0x00,0xc3,0xc3,0xc3,0xdb,0xff,0xe7,0xc3,0x00,0x00, // w
    0x00,0x00,0x00,0x00,0xc3,0x66,0x3c,0x18,0x3c,0x66,0xc3,0x00,0x00, // x
    0x00,0x00,0x00,0x00,0xc3,0x66,0x66,0x3c,0x18,0x30,0x60,0x60,0xc0, // y
    0x00,0x00,0x00,0x00,0xff,0x06,0x0c,0x18,0x30,0x60,0xff,0x00,0x00, // z
    0x0f,0x18,0x18,0x18,0x38,0xf0,0x38,0x18,0x18,0x18,0x0f,0x00,0x00, // {
    0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18, // |
    0xf0,0x18,0x18,0x18,0x1c,0x0f,0x1c,0x18,0x18,0x18,0xf0,0x00,0x00, // }
    0x00,0x00,0x00,0x60,0xf1,0x8f,0x06,0x00,0x00,0x00,0x00,0x00,0x00  // ~
};

void drawChar(int x, int y, char c, int color, int scale) {
  if (c < 32 || c > 126) c = ' ';
  int idx = (c - 32) * 13;
  for (int row = 0; row < 13; row++) {
    uint8_t line = pgm_read_byte(&font8x13[idx + row]);
    for (int col = 0; col < 8; col++) {
      if (line & (0x80 >> col)) {
        if (scale == 1) setPx(x + col, y + row, color);
        else fillRect(x + col * scale, y + row * scale, scale, scale, color);
      }
    }
  }
}

void drawText(int x, int y, const char *s, int color, int scale) {
  while (*s) {
    if (*s == '\n') { y += 13 * scale + 2; x = 0; }
    else { drawChar(x, y, *s, color, scale); x += 9 * scale; }
    s++;
  }
}

// 紧凑版 drawText，每字符 8px 间距（不加间隙），适合窄行
void drawTextNarrow(int x, int y, const char *s, int color, int scale) {
  while (*s) {
    if (*s == '\n') { y += 13 * scale + 2; x = 0; }
    else { drawChar(x, y, *s, color, scale); x += 8 * scale; }
    s++;
  }
}

// ============================================================
// 4. 90度旋转（横屏显示）
// ============================================================
void rotate90() {
  uint8_t nb[FSIZE], nr[FSIZE];
  memset(nb, 0, FSIZE); memset(nr, 0, FSIZE);
  for (int y = 0; y < HEIGHT; y++)
    for (int x = 0; x < WIDTH; x++) {
      int nx = y, ny = WIDTH - 1 - x;
      int ni = (ny * HEIGHT + nx) / 8;
      uint8_t nm = 0x80 >> ((ny * HEIGHT + nx) % 8);
      int oi = (y * WIDTH + x) / 8;
      uint8_t om = 0x80 >> ((y * WIDTH + x) % 8);
      if (bw[oi] & om) nb[ni] |= nm;
      if (re[oi] & om) nr[ni] |= nm;
    }
  memcpy(bw, nb, FSIZE); memcpy(re, nr, FSIZE);
}

// ============================================================
// 5. 配置存储
// ============================================================
bool loadConfig() {
  prefs.begin(NVS_NS, true);
  bool has = prefs.isKey("ssid");
  if (has) {
    prefs.getString("ssid", cfg_ssid, sizeof(cfg_ssid));
    prefs.getString("pass", cfg_pass, sizeof(cfg_pass));
    prefs.getString("city", cfg_city, sizeof(cfg_city));
    cfg_refresh_min = prefs.getInt("refresh", 30);
    cfg_weather = prefs.getBool("weather", true);
    cfg_image   = prefs.getBool("image", true);
  }
  prefs.end();
  if (cfg_refresh_min < 5) cfg_refresh_min = 5;
  weather_refresh_interval = (unsigned long)cfg_refresh_min * 60000;
  return has;
}

void saveConfig(const char *ssid, const char *pass, const char *city, int refresh_min, bool weather, bool image) {
  prefs.begin(NVS_NS, false);
  prefs.putString("ssid", ssid);
  prefs.putString("pass", pass);
  prefs.putString("city", city);
  prefs.putInt("refresh", refresh_min);
  prefs.putBool("weather", weather);
  prefs.putBool("image", image);
  prefs.end();
  strlcpy(cfg_ssid, ssid, sizeof(cfg_ssid));
  strlcpy(cfg_pass, pass, sizeof(cfg_pass));
  strlcpy(cfg_city, city, sizeof(cfg_city));
  cfg_refresh_min = refresh_min;
  cfg_weather = weather;
  cfg_image = image;
  weather_refresh_interval = (unsigned long)refresh_min * 60000;
}

// ============================================================
// 6. NTP 时间同步
// ============================================================
bool syncNTP() {
  if (!wifi_connected) return false;
  configTime(8 * 3600, 0, "ntp.aliyun.com", "ntp.ntsc.ac.cn", "pool.ntp.org");
  time_t now = 0;
  int retries = 0;
  while (time(&now) < 100000 && retries < 20) { delay(500); retries++; }
  if (now >= 100000) {
    struct tm *ti = localtime(&now);
    current_hour = ti->tm_hour;
    current_min = ti->tm_min;
    current_sec = ti->tm_sec;
    current_year = ti->tm_year + 1900;
    current_month = ti->tm_mon + 1;
    current_day = ti->tm_mday;
    time_synced = true;
    Serial.printf("[NTP] %04d-%02d-%02d %02d:%02d:%02d\n",
      current_year, current_month, current_day, current_hour, current_min, current_sec);
    return true;
  }
  Serial.println("[NTP] 同步失败");
  return false;
}

void updateLocalTime() {
  if (!time_synced) return;
  time_t now;
  time(&now);
  struct tm *ti = localtime(&now);
  current_hour = ti->tm_hour;
  current_min = ti->tm_min;
  current_sec = ti->tm_sec;
  current_year = ti->tm_year + 1900;
  current_month = ti->tm_mon + 1;
  current_day = ti->tm_mday;
}

// BLE 模式下没有 WiFi、也就没有 NTP。此时用手机写进来的时间校准。
// 手机发 {"time":"2026-11-01 18:40:00"}，或 {"epoch":1730544000}。
void setTimeFromBle(unsigned long epochSec) {
  if (epochSec < 1600000000UL) return;   // 明显不是有效时间(早于2020)，忽略

  // 必须先设时区，否则 localtime() 返回 UTC，屏幕/状态里会差 8 小时。
  // ESP32 默认 TZ 为空（等价 UTC），这里固定为中国标准时间 UTC+8。
  setenv("TZ", "CST-8", 1);
  tzset();

  struct timeval tv;
  tv.tv_sec  = (time_t)epochSec;
  tv.tv_usec = 0;
  settimeofday(&tv, nullptr);
  time_synced = true;
  updateLocalTime();
  Serial.printf("[TIME] 已用手机时间校准: %04d-%02d-%02d %02d:%02d\n",
                current_year, current_month, current_day, current_hour, current_min);
}

// ============================================================
// 7. 天气获取（wttr.in）
// ============================================================
bool fetchWeather() {
  if (!wifi_connected) return false;

  String url = "https://wttr.in/";
  url += cfg_city;
  url += "?format=j1";

  HTTPClient http;
  WiFiClientSecure client;
  client.setInsecure();
  http.begin(client, url);
  http.setConnectTimeout(8000);
  http.setTimeout(10000);
  http.addHeader("Accept", "application/json");

  Serial.printf("[WEATHER] 请求 %s ... ", url.c_str());
  int code = http.GET();
  if (code != 200) {
    String err = http.errorToString(code);
    Serial.printf("HTTPS 失败 HTTP %d (%s)\n", code, err.c_str());
    http.end();

    // 回退到 HTTP
    url.replace("https://", "http://");
    Serial.printf("[WEATHER] 回退 HTTP %s ... ", url.c_str());
    http.begin(url);
    http.setConnectTimeout(8000);
    http.setTimeout(10000);
    http.addHeader("Accept", "application/json");
    code = http.GET();
    if (code != 200) {
      Serial.printf("HTTP 也失败 %d\n", code);
      http.end();
      return false;
    }
  }

  String body = http.getString();
  http.end();
  Serial.printf("OK (%d bytes)\n", body.length());

  // 调试：打印JSON前300字节确认格式
  Serial.println("[DEBUG] JSON 开头:");
  Serial.println(body.substring(0, 300));
  Serial.println("[DEBUG] ---");

  // 辅助 lambda：从 body 中找 key 并提取 value
  auto extractStr = [&](const char *key, char *buf, int maxlen) {
    String k = "\"";
    k += key;
    k += "\": \"";
    int p = body.indexOf(k);
    if (p >= 0) {
      p += k.length();
      int e = body.indexOf("\"", p);
      if (e > p) { int len = min(e - p, maxlen - 1); strncpy(buf, body.c_str() + p, len); buf[len] = 0; return true; }
    }
    Serial.printf("[DEBUG] 未找到字段: %s\n", key);
    return false;
  };

  // 当前天气
  extractStr("temp_C", weather_temp, sizeof(weather_temp));
  extractStr("FeelsLikeC", weather_feels, sizeof(weather_feels));
  extractStr("humidity", weather_hum, sizeof(weather_hum));
  extractStr("pressure", weather_pressure, sizeof(weather_pressure));
  extractStr("uvIndex", weather_uv, sizeof(weather_uv));

  // weatherCode 在 humidity 之后，用首次出现
  int wc1 = body.indexOf("\"weatherCode\": \"");
  if (wc1 >= 0) {
    wc1 += 16;
    int wc2 = body.indexOf("\"", wc1);
    if (wc2 > wc1) { char buf[4]; int len = min(wc2 - wc1, 3); strncpy(buf, body.c_str() + wc1, len); buf[len] = 0; weather_code = atoi(buf); }
    Serial.printf("[DEBUG] weatherCode=%d\n", weather_code);
  } else {
    Serial.println("[DEBUG] weatherCode 未找到");
  }

  // weatherDesc 使用首次出现（当前天气的描述）
  int d1 = body.indexOf("\"weatherDesc\":");
  if (d1 >= 0) {
    int v = body.indexOf("\"value\": \"", d1);
    if (v >= 0) {
      v += 10;
      int d2 = body.indexOf("\"", v);
      if (d2 > v) { int len = min(d2 - v, (int)sizeof(weather_desc) - 1); strncpy(weather_desc, body.c_str() + v, len); weather_desc[len] = 0; }
    }
    Serial.printf("[DEBUG] weatherDesc=%s\n", weather_desc);
  } else {
    Serial.println("[DEBUG] weatherDesc 未找到");
  }

  // 风向风速
  String wdir, wspd;
  if (extractStr("winddir16Point", weather_wind, sizeof(weather_wind))) {
    // 追加风速到 wind 字符串
    String tmp = weather_wind;
    tmp += " ";
    char spd[8];
    if (extractStr("windspeedKmph", spd, sizeof(spd))) { tmp += spd; tmp += "km/h"; }
    strncpy(weather_wind, tmp.c_str(), sizeof(weather_wind) - 1);
    weather_wind[sizeof(weather_wind) - 1] = 0;
  }

  // 解析 3 天预报
  // 先找 weather 数组的开始
  int wpos = body.indexOf("\"weather\": [");
  if (wpos >= 0) {
    String wsec = body.substring(wpos + 12);  // 从第一个 { 开始

    // 找第 1 天预报的数据（index 0，当天）
    // 找 maxtempC / mintempC 在 weather[0] 中的首次出现
    // 当天
    int mt1 = body.indexOf("\"maxtempC\": \"", wpos);
    if (mt1 >= 0) { mt1 += 13; int mt2 = body.indexOf("\"", mt1); if (mt2 > mt1) { int len = min(mt2 - mt1, 5); strncpy(weather_thigh, body.c_str() + mt1, len); weather_thigh[len] = 0; } }
    int mn1 = body.indexOf("\"mintempC\": \"", wpos);
    if (mn1 >= 0) { mn1 += 13; int mn2 = body.indexOf("\"", mn1); if (mn2 > mn1) { int len = min(mn2 - mn1, 5); strncpy(weather_tlow, body.c_str() + mn1, len); weather_tlow[len] = 0; } }

    // 找第 2 天（明天）：在第一个 "date": " 之后找第二个 "date": "
    int dpos1 = body.indexOf("\"date\": \"", wpos);
    int dpos2 = body.indexOf("\"date\": \"", dpos1 + 9);
    if (dpos2 > 0) {
      // 明天的 date（取 MM-DD，跳过 YYYY-）
      int de = body.indexOf("\"", dpos2 + 9);
      if (de > dpos2 + 9 + 5) { int len = min(de - dpos2 - 9 - 5, 5); strncpy(weather_fc1_date, body.c_str() + dpos2 + 9 + 5, len); weather_fc1_date[len] = 0; }
      // 明天的 maxtempC
      int mt2 = body.indexOf("\"maxtempC\": \"", dpos2);
      if (mt2 > 0) { mt2 += 13; int me = body.indexOf("\"", mt2); if (me > mt2) { int len = min(me - mt2, 5); strncpy(weather_fc1_high, body.c_str() + mt2, len); weather_fc1_high[len] = 0; } }
      int mn2 = body.indexOf("\"mintempC\": \"", dpos2);
      if (mn2 > 0) { mn2 += 13; int me = body.indexOf("\"", mn2); if (me > mn2) { int len = min(me - mn2, 5); strncpy(weather_fc1_low, body.c_str() + mn2, len); weather_fc1_low[len] = 0; } }
      // 明天的 weatherDesc（取第一个 hourly 的描述）
      int hpos = body.indexOf("\"hourly\": [", dpos2);
      if (hpos > 0) {
        int wd = body.indexOf("\"weatherDesc\":", hpos);
        if (wd > 0) {
          int v = body.indexOf("\"value\": \"", wd);
          if (v > 0) {
            v += 10; int we = body.indexOf("\"", v);
            if (we > v) { int len = min(we - v, 14); strncpy(weather_fc1_desc, body.c_str() + v, len); weather_fc1_desc[len] = 0; }
          }
        }
      }
    }

    // 第 3 天（后天）
    int dpos3 = body.indexOf("\"date\": \"", dpos2 + 9);
    if (dpos3 > 0) {
      // 后天的 date（取 MM-DD）
      int de = body.indexOf("\"", dpos3 + 9);
      if (de > dpos3 + 9 + 5) { int len = min(de - dpos3 - 9 - 5, 5); strncpy(weather_fc2_date, body.c_str() + dpos3 + 9 + 5, len); weather_fc2_date[len] = 0; }
      int mt3 = body.indexOf("\"maxtempC\": \"", dpos3);
      if (mt3 > 0) { mt3 += 13; int me = body.indexOf("\"", mt3); if (me > mt3) { int len = min(me - mt3, 5); strncpy(weather_fc2_high, body.c_str() + mt3, len); weather_fc2_high[len] = 0; } }
      int mn3 = body.indexOf("\"mintempC\": \"", dpos3);
      if (mn3 > 0) { mn3 += 13; int me = body.indexOf("\"", mn3); if (me > mn3) { int len = min(me - mn3, 5); strncpy(weather_fc2_low, body.c_str() + mn3, len); weather_fc2_low[len] = 0; } }
      int hpos2 = body.indexOf("\"hourly\": [", dpos3);
      if (hpos2 > 0) {
        int wd = body.indexOf("\"weatherDesc\":", hpos2);
        if (wd > 0) {
          int v = body.indexOf("\"value\": \"", wd);
          if (v > 0) {
            v += 10; int we = body.indexOf("\"", v);
            if (we > v) { int len = min(we - v, 14); strncpy(weather_fc2_desc, body.c_str() + v, len); weather_fc2_desc[len] = 0; }
          }
        }
      }
    }
  }

  updateLocalTime();
  snprintf(last_weather_time, sizeof(last_weather_time), "%02d:%02d", current_hour, current_min);

  Serial.printf("[WEATHER] %s %sC(体感%s) %s%% %s 风%s 气压%s 紫外线%s\n",
    cfg_city, weather_temp, weather_feels, weather_hum, weather_desc,
    weather_wind, weather_pressure, weather_uv);
  Serial.printf("[WEATHER] 今天↑%s↓%s  明天%s %s↑%s↓%s\n",
    weather_thigh, weather_tlow, weather_fc1_date, weather_fc1_desc, weather_fc1_high, weather_fc1_low);
  return true;
}

// ============================================================
// 8. 天气图标（基于 wttr.in weatherCode）
// ============================================================
void drawWeatherIcon(int x, int y) {
  int cx = x + 20, cy = y + 15;

  // 113=晴, 116=多云, 119=阴, 122=非常阴
  // 143=雾, 248=雾, 260=大雾
  // 176=小雨, 179=小雪, 182=雨夹雪, 185=冻雨, 200=雷暴
  // 227=阵雪, 230=暴雪, 263=小雨, 266=细雨
  // 293=小雨, 296=小雨, 299=中雨, 302=中雨
  // 305=大雨, 308=大雨, 311=冻雨, 314=冻雨
  // 317=雨夹雪, 320=雨夹雪, 323=小雪, 326=小雪
  // 329=中雪, 332=中雪, 335=大雪, 338=大雪
  // 350=冰雹, 353=阵雨, 356=中阵雨, 359=大阵雨
  // 362=雨夹雪, 365=雨夹雪, 368=小阵雪, 371=大阵雪
  // 374=小冰雹, 377=冰雹, 386=雷阵雨, 389=雷阵雨
  // 392=雷阵雪, 395=雷阵雪

  if (weather_code == 113) {
    // 晴 - 太阳
    drawCircle(cx, cy, 12, 1, false);
    for (int a = 0; a < 360; a += 45) {
      float rad = a * 3.14159 / 180.0;
      int ox = cx + (int)(cos(rad) * 15);
      int oy = cy + (int)(sin(rad) * 15);
      drawRect(ox - 1, oy - 1, 2, 2, 1);
    }
  } else if (weather_code >= 116 && weather_code <= 122) {
    // 多云/阴
    drawCircle(cx + 5, cy, 8, 1, false);
    drawCircle(cx - 5, cy + 2, 7, 1, false);
    drawCircle(cx + 12, cy + 2, 6, 1, false);
    drawLine(cx - 13, cy + 2, cx + 18, cy + 2, 1);
    drawLine(cx - 12, cy + 3, cx + 17, cy + 3, 1);
  } else if (weather_code == 143 || weather_code == 248 || weather_code == 260) {
    // 雾
    for (int i = 0; i < 3; i++)
      drawLine(cx - 15, cy - 5 + i * 8, cx + 15, cy - 5 + i * 8, 1);
  } else if (weather_code >= 176 && weather_code <= 200) {
    // 雨/雷
    drawCircle(cx + 5, cy - 3, 10, 1, false);
    drawCircle(cx - 5, cy - 1, 8, 1, false);
    for (int i = 0; i < 4; i++) {
      int rx = x + 10 + i * 8, ry = y + 20 + i * 2;
      drawLine(rx, ry, rx - 2, ry + 5, 1);
    }
  } else if ((weather_code >= 263 && weather_code <= 266) ||
             (weather_code >= 293 && weather_code <= 308)) {
    // 雨
    drawCircle(cx + 5, cy - 3, 10, 1, false);
    for (int i = 0; i < 5; i++) {
      int rx = x + 8 + i * 6, ry = y + 18 + i;
      drawLine(rx, ry, rx - 2, ry + 6, 1);
    }
  } else if ((weather_code >= 323 && weather_code <= 338) ||
             (weather_code >= 368 && weather_code <= 371)) {
    // 雪
    drawCircle(cx + 5, cy - 3, 10, 1, false);
    for (int i = 0; i < 4; i++) {
      int sx = x + 10 + i * 7, sy = y + 20 + i * 2;
      drawCircle(sx, sy, 1, 1, true);
      drawCircle(sx, sy, 2, 1, false);
    }
  } else {
    // 默认 - 云
    drawCircle(cx + 5, cy, 8, 1, false);
    drawCircle(cx - 5, cy + 2, 7, 1, false);
    drawCircle(cx + 12, cy + 2, 6, 1, false);
    drawLine(cx - 13, cy + 2, cx + 18, cy + 2, 1);
  }
}

// ============================================================
// 9. 天气显示页面（在 212x104 空间绘制，旋转后横屏显示）
//    布局：顶部今日信息 | 中间分割线 | 底部一行预告(紧凑字体)
//    数据值用红色(2)，标签用黑色(1)
//    drawText 每字符 9px，drawTextNarrow 每字符 8px
// ============================================================
void displayWeather() {
  // 注意：这里的开关只管"是否画天气界面"，与"天气数据从哪来"无关。
  // 数据可能来自 WiFi(wttr.in) 或 BLE 写入，两条路都要能刷屏，
  // 所以不能再用 ENABLE_WEATHER_SYNC 拦（那是 WiFi 取数开关）。
  if (!ENABLE_WEATHER_SYNC && !ENABLE_BLE_SYNC) return;

  clearAll();
  updateLocalTime();

  // ===== v4 双栏布局（方舟像素 12px）=====
  // 左栏：天气图标 + UV + 大号温度
  // 右栏：中文单句建议 + 明日预告
  // 分栏位置由左栏内容宽度决定（COL_SPLIT_X=80），不是对半

  // 温度字符串：只保留数字，单位在布局里处理
  char tempBuf[12];
  snprintf(tempBuf, sizeof(tempBuf), "%s", weather_temp);

  // UV 字符串
  char uvBuf[8];
  snprintf(uvBuf, sizeof(uvBuf), "%s", weather_uv);

  // 右上角"最新更新时间"：24 小时制到分（仅数字与冒号，走 10px 字库）
  char timeBuf[8];
  timeBuf[0] = 0;
  if (time_synced) {
    snprintf(timeBuf, sizeof(timeBuf), "%02d:%02d", current_hour, current_min);
  }

  // 今日温度区间：低 / 高（布局层把"高"染红）
  char lowBuf[8], highBuf[8];
  lowBuf[0] = 0; highBuf[0] = 0;
  if (weather_tlow[0] && strcmp(weather_tlow, "--") != 0) {
    snprintf(lowBuf, sizeof(lowBuf), "%s", weather_tlow);
  }
  if (weather_thigh[0] && strcmp(weather_thigh, "--") != 0) {
    snprintf(highBuf, sizeof(highBuf), "%s", weather_thigh);
  }

  // 中文建议：由 advice 模块提前算好并存在全局里
  // （BLE 写入时已算过一次；这里直接用）
  // 明日预告：**整个字符串在这里拼好（含"明日"前缀）**，
  // 布局层不再加前缀，否则会变成"明日 明日 13~20"。
  // "明日 13~20°C" = 明(12)+日(12)+空格(6)+四项数字(6×4)+°(6)+C(6) = 72px，
  // 右栏可用 212-84=128px，放得下。
  char fcBuf[40];
  fcBuf[0] = 0;
  if (weather_fc1_low[0] && weather_fc1_high[0]) {
    snprintf(fcBuf, sizeof(fcBuf), "%s %s~%s°C",
             advice_forecast_label[0] ? advice_forecast_label : "明日",
             weather_fc1_low, weather_fc1_high);
  } else if (weather_fc1_desc[0]) {
    snprintf(fcBuf, sizeof(fcBuf), "%s %s",
             advice_forecast_label[0] ? advice_forecast_label : "明日",
             weather_fc1_desc);
  }

  drawWeatherScene(
      tempBuf,
      weather_code,
      uvBuf,
      advice_line1,
      advice_line2,
      advice_line3,
      fcBuf,
      timeBuf,
      lowBuf,
      highBuf);

  // 旋转并刷新
  rotate90();
  epd_refresh();
  showing_image = false;
}

// ============================================================
// 10. 启动画面
// ============================================================
void displaySplash(const char *line1, const char *line2) {
  clearAll();
  drawText(10, 30, line1, 1, 2);
  if (line2) drawText(10, 60, line2, 1, 1);
  rotate90();
  epd_refresh();
}

// ============================================================
// 11. WiFi + AP 模式
// ============================================================
#if ENABLE_WIFI_CONFIG

void handleRoot() { server.send(200, "text/html", PAGE_CONFIG); }

void handleScan() {
  String json = "[";
  int n = WiFi.scanComplete();
  if (n > 0) {
    bool first = true;
    for (int i = 0; i < n && i < 20; i++) {
      if (WiFi.SSID(i).length() > 0) {
        if (!first) json += ",";
        json += "\"" + WiFi.SSID(i) + "\"";
        first = false;
      }
    }
  }
  json += "]";
  WiFi.scanDelete();
  server.send(200, "application/json", json);
}

void handleSave() {
  String ssid = server.arg("ssid");
  String pass = server.arg("pass");
  String city = server.arg("city");
  int refresh = server.arg("refresh").toInt();
  bool weather = server.arg("weather") == "1";
  bool image = server.arg("image") == "1";

  if (ssid.length() == 0) { server.send(400, "text/plain; charset=utf-8", "SSID 不能为空"); return; }
  if (city.length() == 0) city = "Beijing";
  if (refresh < 5) refresh = 5;

  saveConfig(ssid.c_str(), pass.c_str(), city.c_str(), refresh, weather, image);
  server.send(200, "text/plain; charset=utf-8", "配置已保存！设备重启中...");
  delay(500);
  ESP.restart();
}

void handleWeatherSave() {
  String city = server.arg("city");
  int refresh = server.arg("refresh").toInt();
  if (city.length() == 0) city = "Beijing";
  if (refresh < 5) refresh = 5;
  strlcpy(cfg_city, city.c_str(), sizeof(cfg_city));
  cfg_refresh_min = refresh;
  weather_refresh_interval = (unsigned long)refresh * 60000;

  prefs.begin(NVS_NS, false);
  prefs.putString("city", cfg_city);
  prefs.putInt("refresh", cfg_refresh_min);
  prefs.end();

  Serial.printf("[CFG] 天气更新: city=%s refresh=%dmin\n", cfg_city, cfg_refresh_min);
  server.send(200, "text/plain; charset=utf-8", "天气配置已更新");
}

void handleReset() {
  prefs.begin(NVS_NS, false);
  prefs.clear();
  prefs.end();
  server.send(200, "text/plain; charset=utf-8", "已重置所有配置，设备重启...");
  delay(500);
  ESP.restart();
}

void startAPMode() {
  in_ap_mode = true;
  ap_mode_start = millis();
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP("EPD-Config", NULL);
  delay(200);
  Serial.printf("[AP] 热点: EPD-Config, IP: %s\n", WiFi.softAPIP().toString().c_str());

  displaySplash("AP Mode", "Connect to EPD-Config");
  delay(2000);
  displaySplash("AP: EPD-Config", "IP: 192.168.4.1");

  WiFi.scanNetworks(true);
  server.on("/", handleRoot);
  server.on("/scan", handleScan);
  server.on("/save", HTTP_POST, handleSave);
  server.begin();
}

void loopAPMode() {
  server.handleClient();
  if (millis() - ap_mode_start > 900000) { // 15分钟超时
    if (cfg_ssid[0]) {
      in_ap_mode = false;
      WiFi.mode(WIFI_STA);
      WiFi.begin(cfg_ssid, strlen(cfg_pass) ? cfg_pass : NULL);
    } else {
      ESP.restart();
    }
  }
}

#endif // ENABLE_WIFI_CONFIG

// ============================================================
// 12. 正常模式 Web 服务器
// ============================================================
#if ENABLE_IMAGE_UPLOAD || ENABLE_WEATHER_SYNC

void handleStatus() {
  String json = "{";
  json += "\"ip\":\"" + WiFi.localIP().toString() + "\"";
  json += ",\"ssid\":\"" + String(cfg_ssid) + "\"";
  json += ",\"city\":\"" + String(cfg_city) + "\"";
  json += ",\"refresh\":" + String(cfg_refresh_min);
  json += ",\"weather_time\":\"" + String(last_weather_time) + "\"";
  json += ",\"image_time\":\"" + String(last_image_time) + "\"";
  json += ",\"temp\":\"" + String(weather_temp) + "\"";
  json += ",\"desc\":\"" + String(weather_desc) + "\"";
  json += "}";
  server.send(200, "application/json", json);
}

void handleStatusPage() { server.send(200, "text/html", PAGE_STATUS); }

void handleRefresh() {
  if (showing_image) {
    // 缓冲已处于旋转状态，直接刷新
    epd_refresh();
  } else {
    displayWeather();
  }
  server.send(200, "text/plain; charset=utf-8", "屏幕已刷新");
}

void handleClear() {
  clearAll();
  rotate90();
  epd_refresh();
  server.send(200, "text/plain; charset=utf-8", "屏幕已清空");
}

#endif

// ============================================================
// 13. BMP 图片上传与显示
// ============================================================
#if ENABLE_IMAGE_UPLOAD

int rgbToEpaperColor(uint8_t r, uint8_t g, uint8_t b) {
  if (r > 150 && r > g * 1.5 && r > b * 1.5) return 2;  // 红色
  if (r < 80 && g < 80 && b < 80) return 1;              // 黑色
  if (r > 180 && g > 180 && b > 180) return 0;           // 白色
  uint16_t brightness = (r * 77 + g * 150 + b * 29) >> 8;
  return (brightness < 128) ? 1 : 0;
}

bool processBMP(const uint8_t *data, size_t len) {
  if (len < 54 || data[0] != 'B' || data[1] != 'M') {
    Serial.println("[BMP] 无效文件头"); return false;
  }

  uint32_t bmpWidth = *(uint32_t*)(data + 18);
  uint32_t bmpHeight = *(uint32_t*)(data + 22);
  uint16_t bpp = *(uint16_t*)(data + 28);
  uint32_t dataOffset = *(uint32_t*)(data + 10);

  Serial.printf("[BMP] %dx%d, %d bpp, offset=%d\n", bmpWidth, bmpHeight, bpp, dataOffset);

  if (bpp != 24 && bpp != 32) { Serial.printf("[BMP] 不支持的位深度: %d\n", bpp); return false; }
  if (dataOffset >= len) { Serial.println("[BMP] 数据偏移越界"); return false; }

  clearAll();

  float scaleX = (float)bmpWidth / WIDTH;
  float scaleY = (float)bmpHeight / HEIGHT;
  float scale = max(scaleX, scaleY);

  int srcW = (int)(WIDTH * scale);
  int srcH = (int)(HEIGHT * scale);
  int srcX = (bmpWidth - srcW) / 2;
  int srcY = (bmpHeight - srcH) / 2;
  if (srcX < 0) srcX = 0;
  if (srcY < 0) srcY = 0;
  if (srcX + srcW > (int)bmpWidth) srcW = bmpWidth - srcX;
  if (srcY + srcH > (int)bmpHeight) srcH = bmpHeight - srcY;

  int bytesPerPixel = (bpp == 32) ? 4 : 3;
  int rowSize = ((bmpWidth * bpp + 31) / 32) * 4;

  // 使用连续位流寻址公式
  for (int ey = 0; ey < HEIGHT; ey++) {
    for (int ex = 0; ex < WIDTH; ex++) {
      int sx = srcX + (int)(ex * scale);
      int sy = srcY + (int)(ey * scale);
      if (sx >= (int)bmpWidth) sx = bmpWidth - 1;
      if (sy >= (int)bmpHeight) sy = bmpHeight - 1;

      int row = bmpHeight - 1 - sy;
      uint32_t pixelOffset = dataOffset + row * rowSize + sx * bytesPerPixel;
      if (pixelOffset + 2 >= len) continue;

      uint8_t b = data[pixelOffset];
      uint8_t g = data[pixelOffset + 1];
      uint8_t r = data[pixelOffset + 2];

      int color = rgbToEpaperColor(r, g, b);
      if (color == 1) {
        int i = (ey * WIDTH + ex) / 8;
        uint8_t m = 0x80 >> ((ey * WIDTH + ex) % 8);
        bw[i] |= m;
      } else if (color == 2) {
        int i = (ey * WIDTH + ex) / 8;
        uint8_t m = 0x80 >> ((ey * WIDTH + ex) % 8);
        re[i] |= m;
      }
    }
  }

  return true;
}

// 直接处理客户端预处理的原始 EPD 数据（5512 字节）
// 格式: [BW 位面 2756 字节] [RE 位面 2756 字节]
bool processRawEPD(const uint8_t *data, size_t len) {
  if (len != FSIZE * 2) {
    Serial.printf("[RAW] 数据长度错误: %d (期望 %d)\n", len, FSIZE * 2);
    return false;
  }
  memcpy(bw, data, FSIZE);
  memcpy(re, data + FSIZE, FSIZE);
  Serial.printf("[RAW] 原始 EPD 数据加载成功 (%d 字节)\n", len);
  return true;
}

void handleUpload() {
  HTTPUpload &upload = server.upload();
  static uint8_t *uploadBuf = NULL;
  static size_t uploadLen = 0;
  static size_t uploadTotal = 0;

  if (upload.status == UPLOAD_FILE_START) {
    if (uploadBuf) free(uploadBuf);
    uploadBuf = NULL; uploadLen = 0;
    upload_result_code = 0;
    upload_result_msg[0] = 0;
    uploadTotal = upload.totalSize;
    if (uploadTotal > 0 && uploadTotal < 200000) {
      uploadBuf = (uint8_t*)malloc(uploadTotal + 4);
      if (!uploadBuf) Serial.println("[UPLOAD] 内存不足");
    }
    Serial.printf("[UPLOAD] 开始: %s (%d bytes)\n", upload.filename.c_str(), uploadTotal);
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (uploadBuf && uploadLen + upload.currentSize <= uploadTotal) {
      memcpy(uploadBuf + uploadLen, upload.buf, upload.currentSize);
      uploadLen += upload.currentSize;
    }
  } else if (upload.status == UPLOAD_FILE_END) {
    Serial.printf("[UPLOAD] 完成: %d bytes\n", uploadLen);
    bool ok = false;
    if (uploadBuf && uploadLen > 0) {
      // 检测格式：如果正好是 5512 字节 = 原始 EPD 数据
      if (uploadLen == FSIZE * 2) {
        ok = processRawEPD(uploadBuf, uploadLen);
        if (ok) Serial.println("[UPLOAD] 原始 EPD 数据格式识别");
      }
      // 否则尝试 BMP 解析
      if (!ok && uploadLen > 54 && uploadBuf[0] == 'B' && uploadBuf[1] == 'M') {
        ok = processBMP(uploadBuf, uploadLen);
        if (ok) Serial.println("[UPLOAD] BMP 格式识别");
      }
      // 如果不是 BMP 也不是原始 EPD，尝试强制 BMP 解析（兼容）
      if (!ok) {
        ok = processBMP(uploadBuf, uploadLen);
      }
      if (ok) {
        rotate90();
        epd_refresh();
        showing_image = true;
        image_display_time = millis();
        updateLocalTime();
        snprintf(last_image_time, sizeof(last_image_time), "%02d:%02d", current_hour, current_min);
        upload_result_code = 200;
        strlcpy(upload_result_msg, "图片已显示！", sizeof(upload_result_msg));
        Serial.println("[UPLOAD] 图片显示成功");
      } else {
        upload_result_code = 400;
        strlcpy(upload_result_msg, "图片解析失败，请使用客户端预处理上传", sizeof(upload_result_msg));
        Serial.println("[UPLOAD] 图片解析失败");
      }
    } else {
      upload_result_code = 400;
      strlcpy(upload_result_msg, "上传失败或无数据", sizeof(upload_result_msg));
    }
    if (uploadBuf) { free(uploadBuf); uploadBuf = NULL; }
    uploadLen = 0;
  }
}

void handleUploadDone() {
  if (upload_result_code == 200) {
    server.send(200, "text/plain; charset=utf-8", upload_result_msg);
  } else if (upload_result_code == 400) {
    server.send(400, "text/plain; charset=utf-8", upload_result_msg);
  } else {
    server.send(500, "text/plain; charset=utf-8", "上传处理中，请重试");
  }
  upload_result_code = 0;
}

#endif // ENABLE_IMAGE_UPLOAD

// ============================================================
// 14. 正常模式处理
// ============================================================
void startNormalMode() {
  in_ap_mode = false;

  // 只显示一个启动画面，后台拉取数据（不额外刷新屏幕）
  displaySplash("Connected!", WiFi.localIP().toString().c_str());

  if (ENABLE_WEATHER_SYNC) {
    syncNTP();
    fetchWeather();
  }

  // 启动 mDNS
  if (MDNS.begin("epd-weather")) {
    Serial.println("[mDNS] http://epd-weather.local");
  } else {
    Serial.println("[mDNS] 启动失败");
  }

  server.on("/", handleStatusPage);
  server.on("/status", handleStatus);
  server.on("/refresh", handleRefresh);
  server.on("/clear", handleClear);
  server.on("/weather_save", HTTP_POST, handleWeatherSave);
  server.on("/reset", HTTP_POST, handleReset);

#if ENABLE_IMAGE_UPLOAD
  server.on("/upload", HTTP_POST, handleUploadDone, handleUpload);
#endif

  server.begin();
  Serial.printf("[HTTP] 服务器已启动: http://%s/  (mDNS: http://epd-weather.local)\n", WiFi.localIP().toString().c_str());

  if (ENABLE_WEATHER_SYNC) {
    displayWeather();
  } else {
    displaySplash("Ready!", WiFi.localIP().toString().c_str());
  }
}

void loopNormalMode() {
  unsigned long now = millis();
  server.handleClient();

  if (ENABLE_WEATHER_SYNC && wifi_connected) {
    // 每30分钟刷新天气
    if (now - last_weather_fetch > weather_refresh_interval) {
      last_weather_fetch = now;

      if (now - last_ntp_sync > 3600000) { syncNTP(); last_ntp_sync = now; }
      else { updateLocalTime(); }

      if (!showing_image) {
        fetchWeather();
        displayWeather();
      } else {
        fetchWeather(); // 只获取数据，不显示
        if (now - image_display_time > 300000) { // 5分钟切回天气
          showing_image = false;
          displayWeather();
        }
      }
    }

    // 每分钟检查是否需要切回天气（图片显示5分钟后自动切回）
    if (showing_image && now - image_display_time > 300000) {
      showing_image = false;
      fetchWeather();
      displayWeather();
    }
  }

#if ENABLE_BLE_SYNC
  // ---------- 蓝牙同步（主路径，不依赖 WiFi）----------
  bleLoop();
  if (bleHasPendingWeather()) {
    applyBleWeather();      // 把 BLE 收到的字段搬到 weather_* 全局变量
    displayWeather();       // 立即刷屏（EPD 只在刷新时耗电）
    bleConsumeReady();
    bleNotify("{\"ok\":true,\"msg\":\"displayed\"}\n");
  }

  // ---------- 自动同步 ----------
  // ESP32 是 GATT 外设，不能主动上网；"自动"靠两个机制配合：
  //   ① 连上即请手机推一次（onConnect 里 notify need_sync）
  //   ② 手机端订阅回执后，按 keep-alive 定时推（网页端实现）
  // 这里额外做一件事：数据过期时主动 notify，提醒手机端刷新。
  static unsigned long last_nag = 0;
  if (bleConnected() && bleWantsSync(30UL * 60UL * 1000UL)
      && (millis() - last_nag > 60000UL)) {
    last_nag = millis();
    bleNotify("{\"cmd\":\"need_sync\",\"reason\":\"stale\"}\n");
  }
#endif
}

#if ENABLE_BLE_SYNC
// 把 BLE 收到的字段应用到显示用全局变量（缺省字段保留原值，可增量更新）
void applyBleWeather() {
  const BleWeather &w = bleWeather();
  char buf[24];

  if (w.has_temp)  { snprintf(buf, sizeof(buf), "%d", w.temp);  strncpy(weather_temp,  buf, sizeof(weather_temp)  - 1);  }
  if (w.has_feels) { snprintf(buf, sizeof(buf), "%d", w.feels); strncpy(weather_feels, buf, sizeof(weather_feels) - 1); }
  if (w.has_hum)   { snprintf(buf, sizeof(buf), "%d", w.hum);   strncpy(weather_hum,   buf, sizeof(weather_hum)   - 1); }
  if (w.has_uv)    { snprintf(buf, sizeof(buf), "%d", w.uv);    strncpy(weather_uv,    buf, sizeof(weather_uv)    - 1); }
  if (w.has_tlow)  { snprintf(buf, sizeof(buf), "%d", w.tlow);  strncpy(weather_tlow,  buf, sizeof(weather_tlow)  - 1); }
  if (w.has_thigh) { snprintf(buf, sizeof(buf), "%d", w.thigh); strncpy(weather_thigh, buf, sizeof(weather_thigh) - 1); }

  if (w.has_desc)    { strncpy(weather_desc, w.desc, sizeof(weather_desc) - 1);         weather_desc[sizeof(weather_desc) - 1] = 0; }
  if (w.has_fc_date) { strncpy(weather_fc1_date, w.fc_date, sizeof(weather_fc1_date) - 1); weather_fc1_date[sizeof(weather_fc1_date) - 1] = 0; }
  if (w.has_fc_desc) { strncpy(weather_fc1_desc, w.fc_desc, sizeof(weather_fc1_desc) - 1); weather_fc1_desc[sizeof(weather_fc1_desc) - 1] = 0; }
  if (w.has_fc_low)  { snprintf(buf, sizeof(buf), "%d", w.fc_low);  strncpy(weather_fc1_low,  buf, sizeof(weather_fc1_low)  - 1); }
  if (w.has_fc_high) { snprintf(buf, sizeof(buf), "%d", w.fc_high); strncpy(weather_fc1_high, buf, sizeof(weather_fc1_high) - 1); }

  // 天气代码：驱动左上角的天气图标。手机没发就用 desc 反推一个。
  if (w.has_code) {
    weather_code = w.code;
  } else if (w.has_desc) {
    weather_code = descToCode(w.desc);
  }

  // 手机若有写时间，就校准时钟（BLE 模式没有 NTP）
  unsigned long ep = bleTakeEpoch();
  if (ep) setTimeFromBle(ep);

  // 记录同步时间（只用于状态显示/回包）
  updateLocalTime();
  char when[32];
  if (time_synced) snprintf(when, sizeof(when), "%02d:%02d", current_hour, current_min);
  else             snprintf(when, sizeof(when), "synced");
  bleMarkSynced(when);
  strncpy(last_weather_time, when, sizeof(last_weather_time) - 1);
  // ---- 中文建议（v4 双栏右栏）----
  if (w.has_a1) { strncpy(advice_line1, w.a1, sizeof(advice_line1) - 1); advice_line1[sizeof(advice_line1)-1] = 0; }
  if (w.has_a2) { strncpy(advice_line2, w.a2, sizeof(advice_line2) - 1); advice_line2[sizeof(advice_line2)-1] = 0; }
  if (w.has_a3) { strncpy(advice_line3, w.a3, sizeof(advice_line3) - 1); advice_line3[sizeof(advice_line3)-1] = 0; }
  if (w.has_fclabel) { strncpy(advice_forecast_label, w.fclabel, sizeof(advice_forecast_label) - 1); advice_forecast_label[sizeof(advice_forecast_label)-1] = 0; }

  last_weather_time[sizeof(last_weather_time) - 1] = 0;

  Serial.printf("[BLE] 已应用: %s C / %s / code=%d\n", weather_temp, weather_desc, weather_code);
  saveWeatherCache();   // 存进 NVS，断电/断连后还能继续显示
}

// 把当前天气写入 NVS，供开机时先显示上次结果
void saveWeatherCache() {
  prefs.begin(NVS_NS, false);
  prefs.putString("w_temp",  weather_temp);
  prefs.putString("w_feels", weather_feels);
  prefs.putString("w_desc",  weather_desc);
  prefs.putString("w_hum",   weather_hum);
  prefs.putString("w_uv",    weather_uv);
  prefs.putString("w_tlow",  weather_tlow);
  prefs.putString("w_thigh", weather_thigh);
  prefs.putString("w_fcd",   weather_fc1_date);
  prefs.putString("w_fcs",   weather_fc1_desc);
  prefs.putString("w_fcl",   weather_fc1_low);
  prefs.putString("w_fch",   weather_fc1_high);
  prefs.putInt("w_code",     weather_code);
  prefs.putString("w_a1",    advice_line1);
  prefs.putString("w_a2",    advice_line2);
  prefs.putString("w_a3",    advice_line3);
  prefs.putString("w_fclab", advice_forecast_label);  // 不能复用 w_fcl（已给 fc1_low）
  prefs.end();
}

// 开机时恢复上次天气（没有就保持 "--"）
void loadWeatherCache() {
  prefs.begin(NVS_NS, true);
  bool has = prefs.isKey("w_temp");
  if (has) {
    prefs.getString("w_temp",  weather_temp,  sizeof(weather_temp));
    prefs.getString("w_feels", weather_feels, sizeof(weather_feels));
    prefs.getString("w_desc",  weather_desc,  sizeof(weather_desc));
    prefs.getString("w_hum",   weather_hum,   sizeof(weather_hum));
    prefs.getString("w_uv",    weather_uv,    sizeof(weather_uv));
    prefs.getString("w_tlow",  weather_tlow,  sizeof(weather_tlow));
    prefs.getString("w_thigh", weather_thigh, sizeof(weather_thigh));
    prefs.getString("w_fcd",   weather_fc1_date, sizeof(weather_fc1_date));
    prefs.getString("w_fcs",   weather_fc1_desc, sizeof(weather_fc1_desc));
    prefs.getString("w_fcl",   weather_fc1_low,  sizeof(weather_fc1_low));
    prefs.getString("w_fch",   weather_fc1_high, sizeof(weather_fc1_high));
    weather_code = prefs.getInt("w_code", 113);
    prefs.getString("w_a1",    advice_line1, sizeof(advice_line1));
    prefs.getString("w_a2",    advice_line2, sizeof(advice_line2));
    prefs.getString("w_a3",    advice_line3, sizeof(advice_line3));
    prefs.getString("w_fclab", advice_forecast_label, sizeof(advice_forecast_label));
  }
  prefs.end();
  Serial.printf("[CACHE] 上次天气: %s (has=%d)\n", has ? weather_temp : "无", has);
}

// 用英文描述反推 wttr.in 风格的天气代码，只为让图标有线可画。
// 匹配顺序从"最坏"到"最好"，避免 "Partly cloudy" 被晴天规则抢先命中。
int descToCode(const char *d) {
  if (!d || !d[0]) return 113;
  char s[24];
  size_t i = 0;
  for (; d[i] && i < sizeof(s) - 1; i++) s[i] = tolower((unsigned char)d[i]);
  s[i] = 0;

  if (strstr(s, "thunder") || strstr(s, "storm"))            return 200;
  if (strstr(s, "blizzard") || strstr(s, "heavy snow"))      return 230;
  if (strstr(s, "snow") || strstr(s, "sleet"))               return 227;
  if (strstr(s, "freez") || strstr(s, "ice"))                return 311;
  if (strstr(s, "heavy rain") || strstr(s, "downpour"))      return 308;
  if (strstr(s, "rain") || strstr(s, "drizzle")
      || strstr(s, "shower"))                                return 296;
  if (strstr(s, "fog") || strstr(s, "mist")
      || strstr(s, "haze"))                                  return 143;
  if (strstr(s, "overcast"))                                 return 122;
  if (strstr(s, "cloud"))                                    return 116;  // partly/cloudy
  return 113;                                                            // sunny/clear
}
#endif

// ============================================================
// 15. WiFi 连接
// ============================================================
bool connectWiFi(unsigned long timeout_ms) {
  Serial.printf("[WiFi] 连接 %s ... ", cfg_ssid);
  WiFi.mode(WIFI_STA);
  WiFi.begin(cfg_ssid, strlen(cfg_pass) ? cfg_pass : NULL);

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED) {
    if (millis() - start > timeout_ms) { Serial.println("失败"); return false; }
    delay(200); Serial.print(".");
  }
  Serial.println();
  Serial.printf("[WiFi] 已连接, IP: %s\n", WiFi.localIP().toString().c_str());
  wifi_connected = true;
  return true;
}

// ============================================================
// 16. setup / loop
// ============================================================
void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n\n=== EPD 天气站 v3 (BLE) ===");

  // 初始化引脚
  pinMode(PIN_CS, OUTPUT);   digitalWrite(PIN_CS, HIGH);
  pinMode(PIN_DC, OUTPUT);
  pinMode(PIN_RST, OUTPUT);
  pinMode(PIN_BUSY, INPUT);
  SPI.begin(PIN_SCLK, -1, PIN_MOSI, PIN_CS);
  SPI.beginTransaction(SPISettings(4000000, MSBFIRST, SPI_MODE0));

  // 初始化 EPD
  Serial.println("[EPD] 初始化...");
  epd_init();
  displaySplash("Starting...", "");

  // 加载配置
  bool hasConfig = loadConfig();
  Serial.printf("[CFG] 已配置: %s\n", hasConfig ? "是" : "否");
  if (hasConfig) {
    Serial.printf("  SSID: %s, City: %s\n", cfg_ssid, cfg_city);
    Serial.printf("  Weather: %s, Image: %s\n", cfg_weather ? "开" : "关", cfg_image ? "开" : "关");
  }

  loadWeatherCache();   // 恢复上次天气，开机即可显示

#if ENABLE_BLE_SYNC
  // ★ 先起 BLE：这是主路径，不依赖 WiFi 是否能连上。
  //   BLE 与 WiFi 共用 2.4G 射频，同时开启会降低吞吐，所以 WiFi 连不上时
  //   直接留在 BLE-only 模式，不强行进 AP 热点（那样会和 BLE 抢射频）。
  bleBegin();
#endif

#if ENABLE_WIFI_CONFIG
  if (hasConfig) {
    displaySplash("WiFi+BLE...", cfg_ssid);
    if (connectWiFi(15000)) {
      startNormalMode();
      return;
    }
#if ENABLE_BLE_SYNC
    // WiFi 连不上是常态（5GHz-only 路由器）——不再进 AP 模式，直接走 BLE
    Serial.println("[WiFi] 连不上，保持 BLE-only 模式（这是预期行为）");
    displaySplash("BLE Only", "EPD-Weather");
    delay(1500);
    displayWeather();
    return;
#else
    Serial.println("[WiFi] 连接失败，进入 AP 模式");
    displaySplash("WiFi Failed", "AP: EPD-Config");
    delay(2000);
#endif
  }
#if !ENABLE_BLE_SYNC
  startAPMode();
#else
  // 没配置过 WiFi 也不影响用蓝牙
  Serial.println("[CFG] 无 WiFi 配置，直接走 BLE");
  displayWeather();
  return;
#endif
#else
  if (hasConfig) connectWiFi(15000);
  wifi_connected = (WiFi.status() == WL_CONNECTED);
  if (wifi_connected) startNormalMode();
  else displaySplash("No WiFi", "Enable WiFi config");
#endif
}

void loop() {
#if ENABLE_WIFI_CONFIG
  if (in_ap_mode) {
    loopAPMode();
#if ENABLE_BLE_SYNC
    // AP 模式下 BLE 也必须继续跑，否则蓝牙同步会整段失效
    bleLoop();
    if (bleHasPendingWeather()) {
      applyBleWeather();
      displayWeather();
      bleConsumeReady();
      bleNotify("{\"ok\":true,\"msg\":\"displayed\"}\n");
    }
#endif
    delay(10);
    return;
  }
#endif
  loopNormalMode();
  delay(5);
}