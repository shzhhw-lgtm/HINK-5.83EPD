/*
 * SerialTest.ino - UC8179 串口交互式台架测试
 *
 * 串口命令（115200）：
 *   0 / 1    切换 DTM 策略：S0=影子帧差分（默认）/ S1=单平面（CDI=0x12，只发 DTM2）
 *   t        全屏文字测试页（多字号 + ASCII 全字符集 + 黑底白字）
 *   c        复杂图片测试页（同心环纹渐变 + 三档灰阶圆，Bayer 4x4 抖动）
 *   g        网格/线条测试页（1px 网格、棋盘、对角线——观察边界细线）
 *   n        依次切换 t -> c -> g 三个图案页
 *   k / w    刷全黑 / 全白（残影检查）
 *   r        DRF 重放：不重传图像，直接按 SRAM 当前内容再刷一次
 *   m        黑白维护清屏
 *   i        当前状态（策略 / 影子帧 / 电源）
 *   ?        帮助
 *
 * 每页右下角有 S? + 页名角标，屏幕内容与串口日志可交叉核对。
 *
 * 电源策略（台架模式）：连续供电刷新，不在每帧后 POF——手册 §16.8-F 实测
 * 每帧 POF 会产生掉电瞬态（白区黑一下），v6 因此改为连续供电。测试完按
 * 'p' 手动断电收尾（防止长时间带电造成 VCOM/边框直流偏置，§16.9.3）。
 * S1 刷新由库自动改用 DRF 触发（DSP 的 Data_Flag 需要双平面，§6.2）。
 *
 * 显示自己的照片（Pillow 管线生成的 38880 字节数组）：
 *   1. 在本文件顶部 #include "landscape_data.h"（数组名按你的头文件改）
 *   2. 在 loop() 的 switch 里加：
 *        case 'p': epd.loadImage(landscape_data); epd.display(); break;
 */

#include <UC8179.h>

UC8179 epd;

static const char* s_page = "TEXT";

static const uint8_t BAYER4[16] = {
    0,  8,  2, 10,
   12,  4, 14,  6,
    3, 11,  1,  9,
   15,  7, 13,  5,
};

// ------------------------------------------------------------------
// 角标
// ------------------------------------------------------------------

static void drawTag() {
    char tag[16];
    snprintf(tag, sizeof(tag), "S%d %s", epd.dtmStrategy(), s_page);
    epd.fillRect(540, 452, 104, 24, UC8179::INK_WHITE);
    epd.drawRect(540, 452, 104, 24, UC8179::INK_BLACK);
    epd.setTextSize(2);
    epd.setTextColor(UC8179::INK_BLACK);
    epd.setCursor(546, 456);
    epd.print(tag);
}

static void refreshPage() {
    drawTag();
    // 台架模式：连续供电（powerOffAfter=false），按 'p' 手动断电收尾
    if (!epd.display(/*powerOffAfter=*/false)) {
        Serial.print("display() failed: ");
        Serial.println(epd.errorText());
    }
}

// ------------------------------------------------------------------
// 测试页 1：全屏文字
// ------------------------------------------------------------------

static void pageText() {
    s_page = "TEXT";
    epd.fillScreen(UC8179::INK_WHITE);
    epd.setTextColor(UC8179::INK_BLACK);

    epd.setTextSize(4);
    epd.setCursor(20, 10);
    epd.print("UC8179 TEXT");

    epd.setTextSize(2);
    epd.setCursor(20, 52);
    epd.print("The quick brown fox jumps over the lazy dog");
    epd.setCursor(20, 74);
    epd.print("0123456789 !@#$%^&*()_+-=[]{};:,./<>?");

    // 5x7 小字（1px 笔画）全字符集，16 字符 x 6 行
    epd.setTextSize(1);
    epd.setCursor(20, 104);
    for (uint8_t c = 0x20; c <= 0x7E; c++) {
        epd.print((char)c);
        if (((c - 0x20) % 16) == 15) epd.print('\n');
    }

    // 长行自动换行测试
    epd.setCursor(20, 176);
    epd.print("Wrap test: ABCDEFGHIJKLMNOPQRSTUVWXYZ 0123456789 "
              "abcdefghijklmnopqrstuvwxyz 0123456789");

    // 黑底白字块
    epd.fillRect(20, 220, 608, 40, UC8179::INK_BLACK);
    epd.setTextSize(2);
    epd.setTextColor(UC8179::INK_WHITE);
    epd.setCursor(32, 232);
    epd.print("Inverted: black on white / white on black");
    epd.setTextColor(UC8179::INK_BLACK);

    // 多字号混排
    epd.setTextSize(3);
    epd.setCursor(20, 280);
    epd.print("Size 3");
    epd.setTextSize(2);
    epd.setCursor(20, 320);
    epd.print("Size 2");
    epd.setTextSize(1);
    epd.setCursor(20, 344);
    epd.print("Size 1");

    refreshPage();
}

// ------------------------------------------------------------------
// 测试页 2：复杂图片（同心环纹 + 灰阶圆，全部整数运算 + Bayer 抖动）
// ------------------------------------------------------------------

static inline bool ditherWhite(uint8_t gray, int16_t x, int16_t y) {
    uint8_t b = BAYER4[(y & 3) * 4 + (x & 3)];
    return ((uint16_t)gray * 16) > ((uint16_t)b * 255 + 128);
}

static void fillCircleGray(int16_t x0, int16_t y0, int16_t r, uint8_t gray) {
    for (int16_t dy = -r; dy <= r; dy++) {
        int16_t dx = r;
        while ((int32_t)dx * dx + (int32_t)dy * dy > (int32_t)r * r) dx--;
        int16_t y = y0 + dy;
        for (int16_t x = x0 - dx; x <= x0 + dx; x++)
            epd.drawPixel(x, y, ditherWhite(gray, x, y) ? UC8179::INK_WHITE
                                                        : UC8179::INK_BLACK);
    }
}

static void pageComplex() {
    s_page = "IMG";
    epd.fillScreen(UC8179::INK_WHITE);

    // 同心环纹：每个环内是 8bit 平滑灰度渐变，抖动后接近照片的中间调表现
    const int32_t cx = 430, cy = 170;
    for (int16_t y = 0; y < (int16_t)UC8179::HEIGHT; y++) {
        int32_t dy = y - cy;
        int32_t dy2 = dy * dy;
        for (int16_t x = 0; x < (int16_t)UC8179::WIDTH; x++) {
            int32_t dx = x - cx;
            uint8_t gray = (uint8_t)(((dx * dx + dy2) / 26) & 0xFF);
            epd.drawPixel(x, y, ditherWhite(gray, x, y) ? UC8179::INK_WHITE
                                                        : UC8179::INK_BLACK);
        }
    }

    // 三档灰阶圆：残影/发暗最敏感的中间调
    fillCircleGray(140, 340, 90, 64);    // 深灰
    fillCircleGray(320, 340, 90, 128);   // 中灰
    fillCircleGray(500, 340, 90, 200);   // 浅灰

    epd.setTextSize(2);
    epd.setTextColor(UC8179::INK_WHITE);
    epd.setCursor(98, 332);
    epd.print("GRAY 64");
    epd.setTextColor(UC8179::INK_BLACK);
    epd.setCursor(278, 332);
    epd.print("GRAY 128");
    epd.setCursor(458, 332);
    epd.print("GRAY 200");

    refreshPage();
}

// ------------------------------------------------------------------
// 测试页 3：网格 / 线条（边界细线观察）
// ------------------------------------------------------------------

static void pageGrid() {
    s_page = "GRID";
    epd.fillScreen(UC8179::INK_WHITE);

    // 16px 1px 网格
    for (int16_t x = 0; x < (int16_t)UC8179::WIDTH; x += 16)
        epd.drawFastVLine(x, 0, UC8179::HEIGHT, UC8179::INK_BLACK);
    for (int16_t y = 0; y < (int16_t)UC8179::HEIGHT; y += 16)
        epd.drawFastHLine(0, y, UC8179::WIDTH, UC8179::INK_BLACK);

    // 双边框
    epd.drawRect(0, 0, UC8179::WIDTH, UC8179::HEIGHT, UC8179::INK_BLACK);
    epd.drawRect(3, 3, UC8179::WIDTH - 6, UC8179::HEIGHT - 6, UC8179::INK_BLACK);

    // 对角线
    epd.drawLine(0, 0, UC8179::WIDTH - 1, UC8179::HEIGHT - 1, UC8179::INK_BLACK);
    epd.drawLine(UC8179::WIDTH - 1, 0, 0, UC8179::HEIGHT - 1, UC8179::INK_BLACK);

    // 1px 棋盘块（100x100，两块：一块固定位置约定，便于 A/B 切换观察）
    for (int16_t y = 60; y < 160; y++)
        for (int16_t x = 60; x < 160; x++)
            if (((x ^ y) & 1) == 0) epd.drawPixel(x, y, UC8179::INK_BLACK);
    for (int16_t y = 60; y < 160; y++)
        for (int16_t x = 480; x < 580; x++)
            if (((x ^ y) & 1) == 0) epd.drawPixel(x, y, UC8179::INK_BLACK);

    // 圆
    epd.drawCircle(320, 110, 70, UC8179::INK_BLACK);
    epd.fillCircle(320, 370, 70, UC8179::INK_BLACK);
    epd.drawCircle(320, 370, 74, UC8179::INK_BLACK);

    refreshPage();
}

// ------------------------------------------------------------------
// 实色 / 状态 / 帮助
// ------------------------------------------------------------------

static void pageSolid(uint8_t color) {
    s_page = (color == UC8179::INK_BLACK) ? "BLACK" : "WHITE";
    epd.fillScreen(color);
    refreshPage();
}

static void printStatus() {
    Serial.print("strategy: S");
    Serial.print(epd.dtmStrategy());
    Serial.print(epd.dtmStrategy() == UC8179::DTM_S0_SHADOW
                     ? " (shadow-frame diff)"
                     : " (single-plane, DTM2 only)");
    Serial.print(" | shadowValid: ");
    Serial.print(epd.isShadowValid() ? "yes" : "no");
    Serial.print(" | powered: ");
    Serial.print(epd.isPowered() ? "yes" : "no");
    Serial.print(" | page: ");
    Serial.println(s_page);
}

static void printHelp() {
    Serial.println("=== UC8179 SerialTest ===");
    Serial.println("0/1  strategy S0(shadow) / S1(single-plane)");
    Serial.println("t    full-screen text page");
    Serial.println("c    complex image page (rings + gray circles, dithered)");
    Serial.println("g    grid / lines page");
    Serial.println("n    next page (t -> c -> g)");
    Serial.println("k/w  solid black / white (ghost check)");
    Serial.println("r    DRF replay (re-refresh SRAM, no re-upload)");
    Serial.println("m    maintenance clear (black -> white)");
    Serial.println("p    power off now (POF) — do this when bench is idle/done");
    Serial.println("i    status   ?  help");
    Serial.println("(bench mode: refreshes keep power on; POF only via 'p')");
}

// ------------------------------------------------------------------

void setup() {
    Serial.begin(115200);
    delay(500);
    epd.setDebug(&Serial);

    if (!epd.begin()) {
        Serial.print("epd.begin() failed: ");
        Serial.println(epd.errorText());
        while (true) delay(1000);
    }
    epd.maintenanceClear();

    printHelp();
    printStatus();
    pageText();
}

void loop() {
    if (!Serial.available()) return;
    char c = Serial.read();
    switch (c) {
        case '0':
            epd.setDtmStrategy(UC8179::DTM_S0_SHADOW);
            printStatus();
            break;
        case '1':
            epd.setDtmStrategy(UC8179::DTM_S1_SINGLE);
            printStatus();
            break;
        case 't': pageText();    break;
        case 'c': pageComplex(); break;
        case 'g': pageGrid();    break;
        case 'n': {              // t -> c -> g 循环
            if      (strcmp(s_page, "TEXT") == 0) pageComplex();
            else if (strcmp(s_page, "IMG")  == 0) pageGrid();
            else                                  pageText();
            break;
        }
        case 'k': pageSolid(UC8179::INK_BLACK); break;
        case 'w': pageSolid(UC8179::INK_WHITE); break;
        case 'r':
            Serial.println("-- DRF replay --");
            if (!epd.refreshByDRF(/*powerOffAfter=*/false)) {
                Serial.print("refreshByDRF failed: ");
                Serial.println(epd.errorText());
            }
            break;
        case 'p':
            Serial.println("-- manual POF (idle finish) --");
            if (!epd.powerOff()) {
                Serial.print("powerOff failed: ");
                Serial.println(epd.errorText());
            }
            break;
        case 'm':
            Serial.println("-- maintenance clear --");
            epd.maintenanceClear();
            break;
        case 'i': printStatus(); break;
        case '?': printHelp();   break;
        default: break;
    }
}
