// ============================================================
// epd_layout.cpp —— 方舟像素 12px 双栏天气界面实现
// ============================================================
#include "epd_layout.h"
#include "font_zh12.h"

// ---------------- UTF-8 解码 ----------------
// 返回解码出的码位，并前移指针
static uint32_t utf8Next(const char *&p) {
    uint8_t c = (uint8_t)*p;
    if (c == 0) return 0;
    if (c < 0x80) { p++; return c; }

    uint32_t cp = 0;
    int extra = 0;
    if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; extra = 1; }
    else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; extra = 2; }
    else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; extra = 3; }
    else { p++; return '?'; }

    p++;
    for (int i = 0; i < extra; i++) {
        uint8_t cc = (uint8_t)*p;
        if ((cc & 0xC0) != 0x80) return '?';
        cp = (cp << 6) | (cc & 0x3F);
        p++;
    }
    return cp;
}

// ---------------- 字形查找 ----------------
// 线性查表：134 项，PROGMEM 顺序扫描。每次绘制最多几十个字，
// 对 EPD 刷新的秒级耗时来说完全可忽略，不值得引入二分查找复杂度。
static const ZhGlyph *findGlyph(uint32_t cp) {
    for (int i = 0; i < ZH_COUNT; i++) {
        if (pgm_read_dword(&ZH_GLYPHS[i].cp) == cp) {
            return &ZH_GLYPHS[i];
        }
    }
    return nullptr;
}

// ---------------- 单字符绘制 ----------------
void drawZhChar(int x, int y, uint32_t cp, int color) {
    const ZhGlyph *g = findGlyph(cp);
    if (!g) {
        // 缺字画一个空心方框，方便一眼看出是字库缺字而不是漏画
        drawRect(x + 1, y + 1, ZH_CELL_W - 2, ZH_CELL_H - 2, color);
        return;
    }
    for (int row = 0; row < ZH_CELL_H; row++) {
        // 每行 2 字节，高字节在前
        uint8_t hi = pgm_read_byte(&g->bits[row * 2]);
        uint8_t lo = pgm_read_byte(&g->bits[row * 2 + 1]);
        uint16_t bits = ((uint16_t)hi << 8) | lo;
        if (!bits) continue;
        for (int col = 0; col < ZH_CELL_W; col++) {
            if (bits & (1 << (ZH_CELL_W - 1 - col))) {
                setPx(x + col, y + row, color);
            }
        }
    }
}

// ---------------- 字符串绘制 ----------------
// 按字形实际宽度推进：ASCII 6px、汉字 12px。
// 否则英文字母会被拉成「U V 7」的松散观感。
void drawZhText(int x, int y, const char *utf8, int color) {
    if (!utf8) return;
    const char *p = utf8;
    int cx = x;
    while (*p) {
        uint32_t cp = utf8Next(p);
        if (cp == 0) break;
        if (cp == '\n') { cx = x; y += ZH_CELL_H + 2; continue; }
        drawZhChar(cx, y, cp, color);
        cx += zhGlyphWidth(cp);
    }
}

// 带倍率的字符串绘制（温度放大用）
void drawZhTextScaled(int x, int y, const char *utf8, int color, int scale) {
    if (!utf8) return;
    const char *p = utf8;
    int cx = x;
    while (*p) {
        uint32_t cp = utf8Next(p);
        if (!cp) break;
        const ZhGlyph *g = findGlyph(cp);
        if (g) {
            for (int row = 0; row < ZH_CELL_H; row++) {
                uint16_t bits = ((uint16_t)pgm_read_byte(&g->bits[row * 2]) << 8)
                              | pgm_read_byte(&g->bits[row * 2 + 1]);
                if (!bits) continue;
                for (int col = 0; col < ZH_CELL_W; col++) {
                    if (bits & (1 << (ZH_CELL_W - 1 - col))) {
                        fillRect(cx + col * scale, y + row * scale, scale, scale, color);
                    }
                }
            }
        }
        cx += zhGlyphWidth(cp) * scale;
    }
}

int zhTextWidth(const char *utf8) {
    if (!utf8) return 0;
    const char *p = utf8;
    int w = 0;
    while (*p) {
        uint32_t cp = utf8Next(p);
        if (cp) w += zhGlyphWidth(cp);
    }
    return w;
}

// ============================================================
// 天气图标（左侧，约 36x36 区域）
// 直接画基本几何图形，不依赖字库
// ============================================================
static void drawIcon(int cx, int cy, int code, int color) {
    // 晴
    if (code == 113) {
        drawCircle(cx, cy, 9, color, false);
        for (int a = 0; a < 360; a += 45) {
            float rad = a * 3.14159f / 180.0f;
            int ox = cx + (int)(cosf(rad) * 13);
            int oy = cy + (int)(sinf(rad) * 13);
            drawLine(cx + (int)(cosf(rad) * 11), cy + (int)(sinf(rad) * 11), ox, oy, color);
        }
        return;
    }
    // 雾
    if (code == 143 || code == 248 || code == 260) {
        for (int i = 0; i < 4; i++)
            drawLine(cx - 14, cy - 7 + i * 5, cx + 14, cy - 7 + i * 5, color);
        return;
    }
    // 雪
    bool snow = (code >= 323 && code <= 338) || (code >= 368 && code <= 371);
    // 雨/雷/雪：云 + 下坠
    bool rain = (code >= 176 && code <= 200) || (code >= 263 && code <= 308) ||
                (code >= 353 && code <= 359);
    if (rain || snow) {
        // 云
        drawCircle(cx - 6, cy - 4, 7, color, false);
        drawCircle(cx + 4, cy - 6, 8, color, false);
        drawCircle(cx + 11, cy - 2, 6, color, false);
        drawLine(cx - 13, cy + 1, cx + 15, cy + 1, color);
        // 下落物
        for (int i = 0; i < 4; i++) {
            int rx = cx - 9 + i * 6;
            if (snow) drawCircle(rx, cy + 8, 1, color, true);
            else      drawLine(rx, cy + 6, rx - 1, cy + 11, color);
        }
        return;
    }
    // 多云 / 阴 / 默认
    drawCircle(cx - 6, cy - 2, 7, color, false);
    drawCircle(cx + 4, cy - 5, 8, color, false);
    drawCircle(cx + 11, cy, 6, color, false);
    drawLine(cx - 13, cy + 4, cx + 15, cy + 4, color);
}

// ============================================================
// 主绘制：双栏
//
// 左栏（x 0..79）：
//     图标 (cx=22, cy=26)      UV 数值（右上角）
//     温度大字（下方居中）
// 右栏（x 84..211）：
//     3 行中文建议 + 1 行明日预告
// 分隔：竖直虚线，不占满全高，留呼吸感
// ============================================================
// 垂直节奏（共 104px 高）：
//   y=22  天气图标中心
//   y=46  紫外线
//   y=72  温度（24px，即 12px×2）
//   y=84  右栏明日预告（与温度底部视觉对齐）
void drawWeatherScene(const char *tempStr, int code,
                      const char *uvStr,
                      const char *advice1, const char *advice2, const char *advice3,
                      const char *forecastStr) {
    // ---------- 分隔线（点状，y=6..98）----------
    for (int y = 6; y < 98; y += 4) {
        setPx(COL_SPLIT_X - 1, y, 1);
        setPx(COL_SPLIT_X - 1, y + 1, 1);
    }

    // ---------- 左栏：图标（水平居中于左栏）----------
    drawIcon(LEFT_CX, 22, code, 1);

    // ---------- 左栏：紫外线（图标下方，居中）----------
    {
        char uvBuf[12];
        snprintf(uvBuf, sizeof(uvBuf), "UV%s", (uvStr && uvStr[0]) ? uvStr : "-");
        int w = zhTextWidth(uvBuf);
        drawZhText(LEFT_CX - w / 2, 46, uvBuf, 1);
    }

    // ---------- 左栏：温度 24px（12px × 2，含 °C，居中）----------
    if (tempStr && tempStr[0]) {
        char tBuf[16];
        // 只在还没带单位时补 °C，避免重复
        if (strstr(tempStr, "°")) {
            snprintf(tBuf, sizeof(tBuf), "%s", tempStr);
        } else {
            snprintf(tBuf, sizeof(tBuf), "%s°C", tempStr);
        }
        int w = zhTextWidth(tBuf) * 2;      // 放大 2 倍后的宽
        int tx = LEFT_CX - w / 2;
        if (tx < 2) tx = 2;
        drawZhTextScaled(tx, 72, tBuf, 1, 2);
    }

    // ---------- 右栏：建议文字（2 行）----------
    int ry = 12;
    const char *lines[2] = { advice1, advice2 };
    for (int i = 0; i < 2; i++) {
        if (lines[i] && lines[i][0]) {
            drawZhText(RIGHT_PAD_X, ry, lines[i], 1);
            ry += ZH_CELL_H + 10;
        }
    }

    // ---------- 右栏底部：明日预告 ----------
    // forecastStr 由调用方拼好（含"明日"前缀），这里不再加，否则会重复两次
    if (forecastStr && forecastStr[0]) {
        drawLine(RIGHT_PAD_X, 68, RIGHT_PAD_X + 112, 68, 1);
        drawZhText(RIGHT_PAD_X, 84, forecastStr, 1);
    }
}
