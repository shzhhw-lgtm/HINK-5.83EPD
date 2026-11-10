/*
 * 02_FourPanelClock — 四片 HINK-E0583A51-A0 拼成 HH:MM 大时钟
 * ------------------------------------------------------------------
 * 这是 bigclock 的目标形态：4 片屏共享 SCK/SDA/DC，CS、BUSY 各自独立。
 *
 * 接线（对应 4 屏接线图）:
 *   共享: SCK=GPIO6  SDA=GPIO7  DC=GPIO3  RST=板载 RESET（固件不可控，传 -1）
 *   时十: CS=GPIO10  BUSY=GPIO2
 *   时个: CS=GPIO0   BUSY=GPIO1
 *   分十: CS=GPIO5   BUSY=GPIO8
 *   分个: CS=GPIO18  BUSY=GPIO19
 *
 * 关键点:
 *  1) 4 片共用一块帧缓存（38.8KB），画完一片立刻 startRefresh() 把数据推进去，
 *     再画下一片，RAM 占用只有 1/4。
 *  2) 4 片依次 startRefresh()，此时前一片已经在刷新，SPI 总线却空着，
 *     最后统一 waitRefresh()，4 片并行刷，总时间约 = 1 片的时间 + 数据搬运。
 *  3) 面板竖装时把 ROTATION 改成 1 或 3。
 */
#include <UC8179_EPD.h>

// ---------------- 引脚 ----------------
#define PIN_DC 3
#define PIN_SCK 6
#define PIN_MOSI 7
#define PIN_RST -1

#define CS_H10 10
#define CS_H1 0
#define CS_M10 5
#define CS_M1 18

#define BUSY_H10 2
#define BUSY_H1 1
#define BUSY_M10 8
#define BUSY_M1 19

// 0=横屏 648x480, 1=竖屏 480x648, 2/3=再转 180°
#define ROTATION 0

UC8179_EPD epdH10(CS_H10, PIN_DC, PIN_RST, BUSY_H10);
UC8179_EPD epdH1(CS_H1, PIN_DC, PIN_RST, BUSY_H1);
UC8179_EPD epdM10(CS_M10, PIN_DC, PIN_RST, BUSY_M10);
UC8179_EPD epdM1(CS_M1, PIN_DC, PIN_RST, BUSY_M1);

UC8179_EPD *const panels[4] = {&epdH10, &epdH1, &epdM10, &epdM1};

// 4 片共用这一块帧缓存（必须先 setBuffer 再 begin，避免 4 份 malloc）
static uint8_t frame[UC8179_EPD::bufferSize()];

// 七段码位定义: bit0=A ... bit6=G
static const uint8_t SEG_MASK[10] = {
    0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F};

// ---------------- 画一个七段数字 ----------------
static void drawDigit(UC8179_EPD &d, uint8_t digit) {
  if (digit > 9) digit = 0;
  const int16_t W = d.width();
  const int16_t H = d.height();

  const int16_t w = (int16_t)(W * 0.56f);
  const int16_t h = (int16_t)(H * 0.78f);
  const int16_t x = (W - w) / 2;
  const int16_t y = (H - h) / 2;
  const int16_t t = w / 6;                 // 笔画粗细
  const int16_t v = (h - 3 * t) / 2;       // 竖段长度
  const int16_t g = t / 4;                 // 段间留缝
  const uint8_t m = SEG_MASK[digit];

  if (m & 0x01)  // A 上
    d.fillRect(x + t, y, w - 2 * t, t, UC8179_BLACK);
  if (m & 0x02)  // B 右上
    d.fillRect(x + w - t, y + t + g, t, v - 2 * g, UC8179_BLACK);
  if (m & 0x04)  // C 右下
    d.fillRect(x + w - t, y + h / 2 + t / 2 + g, t, v - 2 * g, UC8179_BLACK);
  if (m & 0x08)  // D 下
    d.fillRect(x + t, y + h - t, w - 2 * t, t, UC8179_BLACK);
  if (m & 0x10)  // E 左下
    d.fillRect(x, y + h / 2 + t / 2 + g, t, v - 2 * g, UC8179_BLACK);
  if (m & 0x20)  // F 左上
    d.fillRect(x, y + t + g, t, v - 2 * g, UC8179_BLACK);
  if (m & 0x40)  // G 中
    d.fillRect(x + t, y + h / 2 - t / 2, w - 2 * t, t, UC8179_BLACK);
}

// ---------------- 显示 HH:MM ----------------
static void showTime(uint8_t hour, uint8_t minute) {
  uint8_t digits[4] = {(uint8_t)(hour / 10), (uint8_t)(hour % 10),
                       (uint8_t)(minute / 10), (uint8_t)(minute % 10)};

  uint32_t t0 = millis();
  for (uint8_t i = 0; i < 4; i++) {
    panels[i]->fillScreen(UC8179_WHITE);
    drawDigit(*panels[i], digits[i]);
    panels[i]->startRefresh();  // 数据已送入该片，缓存可马上复用
  }
  bool ok = true;
  for (uint8_t i = 0; i < 4; i++) {
    ok &= panels[i]->waitRefresh(30000UL);
  }

  Serial.printf("显示 %02u:%02u  用时 %lu ms  %s  剩余堆 %u\n", hour, minute,
                (unsigned long)(millis() - t0), ok ? "OK" : "TIMEOUT",
                (unsigned)ESP.getFreeHeap());
}

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println(F("\nUC8179 四屏时钟"));

  bool ok = true;
  for (uint8_t i = 0; i < 4; i++) {
    panels[i]->setBuffer(frame);  // 先绑定共享缓存
    // 只有第一片需要真正复位总线；这里 RST=-1，doReset 无影响
    ok &= panels[i]->begin(PIN_SCK, -1, PIN_MOSI, 4000000, i == 0);
    panels[i]->setRotation(ROTATION);
  }
  if (!ok) {
    Serial.println(F("!! 初始化失败"));
    while (true) delay(1000);
  }

  // 用编译时刻作为起始时间，之后每分钟 +1（实际项目请接 RTC/NTP）
  uint8_t hh = (__TIME__[0] - '0') * 10 + (__TIME__[1] - '0');
  uint8_t mm = (__TIME__[3] - '0') * 10 + (__TIME__[4] - '0');
  showTime(hh, mm);
}

void loop() {
  static uint32_t nextTick = millis() + 60000UL;
  static uint8_t hour = (__TIME__[0] - '0') * 10 + (__TIME__[1] - '0');
  static uint8_t minute = (__TIME__[3] - '0') * 10 + (__TIME__[4] - '0');

  if (millis() < nextTick) {
    delay(200);
    return;
  }
  nextTick += 60000UL;

  if (++minute >= 60) {
    minute = 0;
    if (++hour >= 24) hour = 0;
  }
  showTime(hour, minute);
}
