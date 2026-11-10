/*
 * 03_BitmapDemo — 演示 drawBitmap() 直接读 PROGMEM 位图
 * ------------------------------------------------------------------
 * 你要用 make_digit_font_gui.py 生成的大数字字模，就是这个套路：
 *   行优先 / MSB 在左 / 每行 (宽+7)/8 字节 / bit=1 画黑
 *
 * 本例把一张 160x120 的位图放在 flash 里，居中绘制。
 * 换成你的 digit_font.h 时，只要把 DIGITS[digit] 和 DIGIT_W/DIGIT_H 传进来。
 */
#include <UC8179_EPD.h>

#include "demo_bitmap.h"

#define PIN_CS 10
#define PIN_DC 3
#define PIN_BUSY 2
#define PIN_SCK 6
#define PIN_MOSI 7
#define PIN_RST -1

UC8179_EPD epd(PIN_CS, PIN_DC, PIN_RST, PIN_BUSY);

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println(F("UC8179 drawBitmap 示例"));

  if (!epd.begin(PIN_SCK, -1, PIN_MOSI, 4000000)) {
    Serial.println(F("!! 初始化失败"));
    while (true) delay(1000);
  }

  epd.fillScreen(UC8179_WHITE);

  // 居中画位图
  int16_t x = (epd.width() - DEMO_BMP_W) / 2;
  int16_t y = (epd.height() - DEMO_BMP_H) / 2 - 40;
  epd.drawBitmap(x, y, DEMO_BMP, DEMO_BMP_W, DEMO_BMP_H, UC8179_BLACK);

  epd.drawString(x + 10, y + DEMO_BMP_H + 30, "drawBitmap()", UC8179_BLACK, 3);
  epd.drawStringf(x + 10, y + DEMO_BMP_H + 70, UC8179_BLACK, 1,
                  "%d x %d, 1bpp, PROGMEM", DEMO_BMP_W, DEMO_BMP_H);

  if (!epd.display()) {
    Serial.println(F("!! 刷新超时"));
  } else {
    Serial.println(F("完成"));
  }
}

void loop() { delay(1000); }
