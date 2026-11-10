/*
 * 01_HelloWorld — 单块 HINK-E0583A51-A0 (UC8179, 648x480) 点亮 + 调试面板
 * ==================================================================
 * 接线（按 4 屏接线图的共享信号，单屏只接一片）:
 *   EPD CS   -> GPIO10
 *   EPD DC   -> GPIO3
 *   EPD BUSY -> GPIO2       (高电平=空闲)
 *   EPD SCK  -> GPIO6
 *   EPD SDA  -> GPIO7 (MOSI)
 *   EPD RST  -> 板载 RESET/EN 时传 -1（固件控制不了，只能软复位）
 *   EPD VCC  -> 3.3V   GND -> GND（必须共地）
 *
 * 串口 115200，按单键实时调参，调好后把参数抄进正式程序：
 *   r 旋转90°   x 左右镜像   y 上下镜像   i 黑白反色
 *   s 单/双平面  f 快速(TR4)  q 高质量(TR7)  a 自动波形
 *   b 全黑      w 全白       c 清屏       d 重画       h 帮助
 *
 * 判断是否调对：左上角应显示 "TOP LEFT"，中间大 F 的横杠在右侧；
 * 四角 L 形块是左上最大、右下其次。
 */
#include <UC8179_EPD.h>

#define PIN_CS 10
#define PIN_DC 3
#define PIN_BUSY 2
#define PIN_SCK 6
#define PIN_MOSI 7
#define PIN_RST -1       // 接板载 RESET/EN 时保持 -1
#define PIN_MISO PIN_MOSI  // SDA 单线回读；若另接 MISO 改成该引脚

UC8179_EPD epd(PIN_CS, PIN_DC, PIN_RST, PIN_BUSY);

uint8_t rotation = 0;
bool mirrorX = false;
bool mirrorY = false;
bool inverted = false;

// ------------------------------------------------------------------ 测试卡
static void drawTestCard() {
  const int16_t w = epd.width();
  const int16_t h = epd.height();

  epd.fillScreen(UC8179_WHITE);

  // 双线边框（顺便检查末行末列是否完整）
  epd.drawRect(2, 2, w - 4, h - 4, UC8179_BLACK);
  epd.drawRect(8, 8, w - 16, h - 16, UC8179_BLACK);

  // 四角 L 形块：左上最大、右下其次，镜像一眼可见
  epd.fillRect(14, 14, 120, 34, UC8179_BLACK);
  epd.fillRect(14, 14, 34, 90, UC8179_BLACK);
  epd.fillRect(w - 74, 14, 60, 24, UC8179_BLACK);
  epd.fillRect(w - 38, 14, 24, 60, UC8179_BLACK);
  epd.fillRect(14, h - 38, 24, 24, UC8179_BLACK);
  epd.fillRect(w - 104, h - 48, 90, 34, UC8179_BLACK);

  // 方向文字
  epd.drawString(150, 40, "TOP LEFT", UC8179_BLACK, 3);
  epd.drawString(w - 330, h - 80, "BOTTOM RIGHT", UC8179_BLACK, 2);
  epd.drawString(40, h / 2 - 16, "LEFT", UC8179_BLACK, 2);
  epd.drawString(w - 100, h / 2 - 16, "RIGHT", UC8179_BLACK, 2);

  // 中心十字 + 圆
  epd.drawLine(w / 2, h / 2 - 60, w / 2, h / 2 + 60, UC8179_BLACK);
  epd.drawLine(w / 2 - 60, h / 2, w / 2 + 60, h / 2, UC8179_BLACK);
  epd.drawCircle(w / 2, h / 2, 40, UC8179_BLACK);

  // 非对称的大 F
  epd.drawString(w / 2 - 26, 150, "F", UC8179_BLACK, 12);

  // 底部状态
  epd.drawStringf(30, h - 34, UC8179_BLACK, 1,
                  "%s  %dx%d  R%d%s%s%s", UC8179_PANEL_NAME, w, h, rotation,
                  mirrorX ? " MX" : "", mirrorY ? " MY" : "",
                  inverted ? " INV" : "");
}

static void applySettings() {
  epd.setRotation(rotation);
  epd.setMirror(mirrorX, mirrorY);
  epd.setInverted(inverted);
}

static void refresh() {
  applySettings();
  drawTestCard();
  Serial.println(F("刷新中..."));
  if (!epd.display()) {
    Serial.println(F("!! 刷新超时：检查 BUSY 接线/上拉，或先把 RST 接对"));
  } else {
    Serial.println(F("完成"));
  }
}

static void help() {
  Serial.println(F("r=旋转 x=X镜像 y=Y镜像 i=反色 s=单/双平面"));
  Serial.println(F("f=快速TR4 q=高质量TR7 a=自动波形 b=全黑 w=全白 c=清屏 d=重画"));
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println();
  Serial.println(F("UC8179 E-Paper 调试面板"));
  Serial.println(F("默认: 单平面 + 自动波形 + 1=黑/0=白"));
  help();

  if (!epd.begin(PIN_SCK, PIN_MISO, PIN_MOSI, 4000000)) {
    Serial.println(F("!! 初始化失败：帧缓存分配不出来（内存不足）"));
    while (true) delay(1000);
  }
  refresh();
  epd.printDiagnostics(Serial);
  epd.printOtpSummary(Serial);
}

void loop() {
  if (!Serial.available()) {
    delay(20);
    return;
  }
  char c = (char)Serial.read();
  switch (c) {
    case 'r':
      rotation = (rotation + 1) & 0x03;
      refresh();
      break;
    case 'x':
      mirrorX = !mirrorX;
      refresh();
      break;
    case 'y':
      mirrorY = !mirrorY;
      refresh();
      break;
    case 'i':
      inverted = !inverted;
      Serial.print(F("inverted = "));
      Serial.println(inverted ? 1 : 0);
      refresh();
      break;
    case 's':
      epd.setSinglePlane(!epd.singlePlane());
      Serial.print(F("singlePlane = "));
      Serial.println(epd.singlePlane() ? 1 : 0);
      refresh();
      break;
    case 'f':
      epd.setRefreshMode(UC8179_EPD::REFRESH_FAST);
      Serial.println(F("波形: 强制 TR4（快）"));
      refresh();
      break;
    case 'q':
      epd.setRefreshMode(UC8179_EPD::REFRESH_QUALITY);
      Serial.println(F("波形: 强制 TR7（高质量）"));
      refresh();
      break;
    case 'a':
      epd.setRefreshMode(UC8179_EPD::REFRESH_AUTO);
      Serial.println(F("波形: OTP 自动分档"));
      refresh();
      break;
    case 'b':
      epd.fillScreen(UC8179_BLACK);
      epd.display();
      break;
    case 'w':
      epd.fillScreen(UC8179_WHITE);
      epd.display();
      break;
    case 'c':
      epd.fillScreen(UC8179_WHITE);
      epd.display();
      break;
    case 'd':
      refresh();
      break;
    case 'h':
      help();
      break;
    default:
      break;
  }
}

