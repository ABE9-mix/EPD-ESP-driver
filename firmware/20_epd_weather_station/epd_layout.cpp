// ============================================================
// epd_layout.cpp —— 方舟像素 12px 双栏天气界面实现
// ============================================================
#include "epd_layout.h"
#include "font_zh12.h"
#include "font_time10.h"
#include "icons.h"

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
// 天气图标 —— 双色面性点阵（由 tools/gen_icons.py 从 SVG 生成）
//
// 源文件在 assets/icons/*.svg，改完重跑 gen_icons.py 即可。
// 每个图标 30x30，分「黑 / 红」两层位图；红色是强调色，
// 用来画太阳、雨滴、雪点、闪电。
// ============================================================

// 天气代码 → 图标 id（与 gen_icons.py 的分组保持一致）
// 雨分两档：小雨(4) / 大雨(5)，按降水量级区分
static uint8_t iconIdFor(int code) {
    switch (code) {
        case 113: return 0;                                  // 晴
        case 116: return 1;                                  // 局部多云
        case 119: case 122: return 2;                        // 阴 / 非常阴
        case 143: case 248: case 260: return 3;              // 雾 / 大雾
        // 雷暴（含雷阵雨、雷阵雪）
        case 200: case 386: case 389: case 392: case 395: return 7;
        // 各类雪
        case 179: case 227: case 230:
        case 323: case 326: case 329: case 332: case 335: case 338:
        case 368: case 371: return 6;
        // 大雨 / 中雨 / 大阵雨 / 冰雹
        case 299: case 302: case 305: case 308:
        case 350: case 356: case 359: case 374: case 377: return 5;
        // 小雨 / 细雨 / 冻雨 / 雨夹雪 / 阵雨
        case 176: case 182: case 185:
        case 263: case 266: case 293: case 296:
        case 311: case 314: case 317: case 320:
        case 353: case 362: case 365: return 4;
        default: return 4;                                   // 其余按小雨
    }
}

// 图标下方显示的当前天气文字（顺序与 icons.h 的 id 一一对应）
// 注：fog 用「灰」——「雾」缺字，「霾」笔画密度 51% 在 12px 下糊成一团，
//     实测「灰」(27%) 最清晰，且雾霾天视觉上就是灰蒙蒙的。
static const char *const ICON_LABEL[ICON_COUNT] = {
    "晴", "多云", "阴", "灰", "小雨", "大雨", "雪", "雷阵雨"
};

static void drawIcon(int cx, int cy, int code, int color) {
    (void)color;   // 双色图标自带配色，不再需要外部颜色
    uint8_t id = iconIdFor(code);

    const WeatherIcon *ic = nullptr;
    for (int i = 0; i < ICON_COUNT; i++) {
        if (pgm_read_byte(&ICONS[i].id) == id) { ic = &ICONS[i]; break; }
    }
    if (!ic) return;

    int x0 = cx - ICON_W / 2;
    int y0 = cy - ICON_H / 2;
    for (int row = 0; row < ICON_H; row++) {
        uint32_t bl = pgm_read_dword(&ic->black[row]);
        uint32_t rd = pgm_read_dword(&ic->red[row]);
        if (!bl && !rd) continue;
        for (int col = 0; col < ICON_W; col++) {
            uint32_t bit = 1u << (ICON_W - 1 - col);
            if (rd & bit)      setPx(x0 + col, y0 + row, 2);   // 红
            else if (bl & bit) setPx(x0 + col, y0 + row, 1);   // 黑
        }
    }
}

// ============================================================
// 圆角矩形填充
// 像素风做法：四个角按 45° 逐行收进，r=2/3 时视觉上就是干净的圆角，
// 不必做真圆计算（在 2~3px 尺度上没有区别，还省 flash）。
// ============================================================
void fillRoundRect(int x, int y, int w, int h, int r, int color) {
    if (w <= 0 || h <= 0) return;
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;
    for (int row = 0; row < h; row++) {
        int dtop = row;
        int dbot = h - 1 - row;
        int d = (dtop < dbot) ? dtop : dbot;
        int inset = (d < r) ? (r - d) : 0;
        int span = w - 2 * inset;
        if (span > 0) fillRect(x + inset, y + row, span, 1, color);
    }
}

// ============================================================
// 10px 时间数字（0-9 与冒号）
// ============================================================
void drawTimeText(int x, int y, const char *s, int color) {
    if (!s) return;
    int cx = x;
    while (*s) {
        char c = *s++;
        const TimeGlyph *g = nullptr;
        for (int i = 0; i < TIME_COUNT; i++) {
            if ((char)pgm_read_byte(&TIME_GLYPHS[i].cp) == c) {
                g = &TIME_GLYPHS[i];
                break;
            }
        }
        if (g) {
            for (int row = 0; row < TIME_CELL_H; row++) {
                uint16_t bits = ((uint16_t)pgm_read_byte(&g->bits[row * 2]) << 8)
                              | pgm_read_byte(&g->bits[row * 2 + 1]);
                if (!bits) continue;
                for (int col = 0; col < TIME_CELL_W; col++) {
                    if (bits & (1 << (TIME_CELL_W - 1 - col))) {
                        setPx(cx + col, y + row, color);
                    }
                }
            }
        }
        cx += 5;   // 10px 字体 ASCII 步进 = 5px
    }
}

int timeTextWidth(const char *s) {
    return s ? (int)strlen(s) * 5 : 0;
}

// ============================================================
// 主绘制：双栏
//
// 左栏（x 0..79）：图标 → 紫外线 → 温度24px → 温度区间12px
// 右栏（x 84..211）：右上角更新时间 → 2 行中文建议 → 明日预告
//
// 垂直节奏（共 104px 高）：
//     y=2    右上角时间胶囊（黑底圆角，12px 高）
//     y=18   天气图标中心
//     y=20   建议第 1 行
//     y=38   紫外线
//     y=38   建议第 2 行
//     y=54   温度（24px，即 12px×2）
//     y=66   右栏分隔线
//     y=82   温度区间（左）/ 明日预告（右）—— 底行左右对齐
// ============================================================
void drawWeatherScene(const char *tempStr, int code,
                      const char *advice1, const char *advice2, const char *advice3,
                      const char *forecastStr,
                      const char *timeStr,
                      const char *tlowStr, const char *thighStr) {
    (void)advice3;   // 当前布局只用 2 行建议，保留参数以便扩展
    // ---------- 分隔线（点状，y=6..98）----------
    for (int y = 6; y < 98; y += 4) {
        setPx(COL_SPLIT_X - 1, y, 1);
        setPx(COL_SPLIT_X - 1, y + 1, 1);
    }

    // ---------- 右上角：最新更新时间（黑底圆角 + 阴刻白字）----------
    // "阴刻"= 黑底挖白字：先铺黑矩形，再用 color=0（擦除=白）写数字
    if (timeStr && timeStr[0]) {
        const int padX = 3;
        const int boxH = 12;
        int tw = timeTextWidth(timeStr);
        int boxW = tw + padX * 2;
        int bx = SCR_W - boxW - 3;
        int by = 2;
        if (bx < COL_SPLIT_X + 2) bx = COL_SPLIT_X + 2;   // 别压到分隔线
        fillRoundRect(bx, by, boxW, boxH, 3, 1);           // 黑底
        // 10px 字形实际落在格的第 2~8 行 → 往下 +1 后视觉居中
        drawTimeText(bx + padX, by + 1, timeStr, 0);       // 白字
    }

    // ---------- 左栏：图标（水平居中）----------
    drawIcon(LEFT_CX, 18, code, 1);

    // ---------- 左栏：当前天气文字（替代原来的 UV 数字）----------
    // UV 等级不再单独显示，已由右栏建议体现（紫外线强 / 注意防晒）
    {
        uint8_t iid = iconIdFor(code);
        const char *label = (iid < ICON_COUNT) ? ICON_LABEL[iid] : "";
        int lw = zhTextWidth(label);
        drawZhText(LEFT_CX - lw / 2, 38, label, 1);
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
        drawZhTextScaled(tx, 54, tBuf, 1, 2);
    }

    // ---------- 左栏：今日温度区间（低~高，最高温用红色强调）----------
    // 形如 13~20°C。三段分别绘制，以便把"高"单独染红。
    if (tlowStr && tlowStr[0] && thighStr && thighStr[0]) {
        char lowBuf[10];
        snprintf(lowBuf, sizeof(lowBuf), "%s~", tlowStr);

        int wLow  = zhTextWidth(lowBuf);
        int wHigh = zhTextWidth(thighStr);
        int wUnit = zhTextWidth("°C");
        int total = wLow + wHigh + wUnit;

        int rx = LEFT_CX - total / 2;
        if (rx < 2) rx = 2;

        drawZhText(rx,                    82, lowBuf,    1);   // 13~  黑
        drawZhText(rx + wLow,             82, thighStr,  2);   // 20   红
        drawZhText(rx + wLow + wHigh,     82, "°C",      1);   // °C   黑
    }

    // ---------- 右栏：建议文字（2 行）----------
    int ry = 20;
    const char *lines[2] = { advice1, advice2 };
    for (int i = 0; i < 2; i++) {
        if (lines[i] && lines[i][0]) {
            drawZhText(RIGHT_PAD_X, ry, lines[i], 1);
            ry += 18;      // 行距 18px（12px 字 + 6px 间隔）
        }
    }

    // ---------- 右栏底部：明日预告 ----------
    // forecastStr 由调用方拼好（含"明日"前缀），这里不再加，否则会重复两次
    if (forecastStr && forecastStr[0]) {
        drawLine(RIGHT_PAD_X, 66, RIGHT_PAD_X + 112, 66, 1);
        drawZhText(RIGHT_PAD_X, 82, forecastStr, 1);
    }
}
