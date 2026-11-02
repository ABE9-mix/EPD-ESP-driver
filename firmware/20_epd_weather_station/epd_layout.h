// ============================================================
// epd_layout.h / epd_layout.cpp —— 新版双栏天气界面
//
// 设计目标（用户 2026-11-01 提出）：
//   原来字号小、文字堆叠、阅读效率低。
//   改为「左侧天气数据 + 右侧中文单句建议」的双栏结构。
//
// 屏幕：212x104 逻辑坐标（rotate90 后横屏显示）
// 字体：方舟像素 12px（英文/数字与中文同字体，等宽 12px 格）
//
// 分栏依据：左栏宽度由内容决定，不是简单对半分。
//   实测"28 12"最宽约占 72px，加图标与边距 → 左栏 80px
//   右栏 = 212 - 80 = 132px，可放 11 个汉字（12px 字宽）
// ============================================================
#pragma once
#include <Arduino.h>

#define SCR_W 212
#define SCR_H 104

// 分栏
#define COL_SPLIT_X   80      // 左栏 0..79，右栏 80..211
#define LEFT_PAD      4
#define LEFT_CX       40      // 左栏水平中心（图标/UV/温度都据此居中）
#define RIGHT_PAD_X   84      // 右栏文字起始 x
#define RIGHT_MAX_CH  10      // 右栏单行最多汉字数（132/12 = 11，留 1 字余量）

// 绘制原语由主 .ino 提供
void setPx(int x, int y, int c);
void drawLine(int x1, int y1, int x2, int y2, int c);
void fillRect(int x, int y, int w, int h, int c);
void drawRect(int x, int y, int w, int h, int c);
void drawCircle(int cx, int cy, int r, int c, bool fill);

// 本模块对外接口
void layoutReset();                                  // 清屏（由主程序 clearAll 处理）
void drawZhChar(int x, int y, uint32_t cp, int color);      // 单个字符
void drawZhText(int x, int y, const char *utf8, int color); // UTF-8 字符串（按字宽推进）
void drawZhTextScaled(int x, int y, const char *utf8, int color, int scale); // 带倍率
int  zhTextWidth(const char *utf8);                          // 计算像素宽

// 圆角矩形填充（像素风 45° 切角，r 建议 2~3）
void fillRoundRect(int x, int y, int w, int h, int r, int color);

// 10px 时间数字（仅 0-9 与冒号），用于右上角"最新更新时间"
void drawTimeText(int x, int y, const char *s, int color);
int  timeTextWidth(const char *s);

void drawWeatherScene(const char *tempStr, int code,
                      const char *advice1, const char *advice2, const char *advice3,
                      const char *forecastStr,
                      const char *timeStr,
                      const char *tlowStr, const char *thighStr);
