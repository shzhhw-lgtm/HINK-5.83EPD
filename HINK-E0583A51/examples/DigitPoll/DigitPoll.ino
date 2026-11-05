/*
 * DigitPoll.ino - 轮询显示数字 0-9（使用 make_digit_font_gui 生成的 digit_font.h）
 *
 * 每 DIGIT_INTERVAL_MS 轮询切换显示 0-9。刷新用 displayQuality(false)：
 * 每次都走 TR7 画质波形，且保持供电不掉电。
 *
 * 重要: 会话期间每次刷新后不要 POF —— displayQuality() 默认参数就是每次
 * POF，而开发指南实测"每次 POF 都有掉电瞬态(白色闪暗)"，表现就是显示完
 * 数字后全屏发暗。POF 只留给收尾/长时间不用（串口 'p' 命令）。
 * 若想用 2 快 1 质的自动节奏（更快、TR7 自动清残影），换回
 * displayManaged(false)，二者只要不 POF 就都不会发暗。
 *
 * 首次运行先执行 maintenanceClear()：全黑->全白恢复刷，洗掉旧固件留下的
 * 暗渍/残影。屏幕已是纯白时这一步无副作用。
 *
 * 串口命令 (115200):
 *   '0'-'9'  立即显示该数字（并从它继续轮询）
 *   's'      暂停 / 继续轮询
 *   'q'      立刻做一次 TR7 质量刷新（清残影，不 POF）
 *   'p'      TR7 + POF 收尾停电（长时间不用时；下次刷新自动恢复供电）
 *
 * 字库说明（digit_font.h）:
 *   DIGITS[10]，每个 DIGIT_W x DIGIT_H（336x596），正立（竖屏视窗）方向存储，
 *   bit=1=笔画，MSB first，行主序。方向不合适改 DIGIT_ROT 宏即可，
 *   点阵数据本身不用重新生成。
 */

#include <UC8179.h>
#include "digit_font.h"

// ---- 可调参数 ----
static const uint32_t DIGIT_INTERVAL_MS = 6000;  // 轮询间隔（刷新本身阻塞约0.5~3s，实际周期略长）
static const int8_t   DIGIT_ROT = 90;            // 绘制旋转：0=不转 90=顺时针90° 180=倒立 270=逆时针90°
static const bool     KEEP_POWERED = true;       // 会话期间保持供电（README 推荐，避免每次刷新后 POF 白闪）

UC8179 epd;

static int      currentDigit = 0;
static bool     polling      = true;
static uint32_t lastSwitch   = 0;

// 把正立存储的数字点阵按 DIGIT_ROT 旋转后居中画进帧缓冲。
// 源图 (bx,by) -> 帧缓冲坐标；越界像素由 epd.drawPixel 内部忽略。
static void drawDigitRotated(const uint8_t* bmp, uint8_t color) {
    int16_t x0, y0;  // 旋转后外接框左上角（居中）
    if (DIGIT_ROT == 90 || DIGIT_ROT == 270) {
        x0 = ((int16_t)UC8179::WIDTH  - (int16_t)DIGIT_H) / 2;  // 旋转后宽=DIGIT_H
        y0 = ((int16_t)UC8179::HEIGHT - (int16_t)DIGIT_W) / 2;  // 旋转后高=DIGIT_W
    } else {
        x0 = ((int16_t)UC8179::WIDTH  - (int16_t)DIGIT_W) / 2;
        y0 = ((int16_t)UC8179::HEIGHT - (int16_t)DIGIT_H) / 2;
    }
    for (int16_t by = 0; by < (int16_t)DIGIT_H; by++) {
        const uint32_t rowBase = (uint32_t)by * DIGIT_ROW_BYTES;
        for (int16_t bx = 0; bx < (int16_t)DIGIT_W; bx++) {
            const uint8_t v = pgm_read_byte(bmp + rowBase + (bx >> 3));
            if (!(v & (0x80 >> (bx & 7)))) continue;  // 只画笔画位
            int16_t x, y;
            switch (DIGIT_ROT) {
                case 90:   x = x0 + (DIGIT_H - 1 - by); y = y0 + bx;                break;
                case 270:  x = x0 + by;                 y = y0 + (DIGIT_W - 1 - bx); break;
                case 180:  x = x0 + (DIGIT_W - 1 - bx); y = y0 + (DIGIT_H - 1 - by); break;
                default:   x = x0 + bx;                 y = y0 + by;                break;
            }
            epd.drawPixel(x, y, color);
        }
    }
}

static void showDigit(int d) {
    if (d < 0 || d > 9) return;
    currentDigit = d;
    // DIGIT_INVERT: 0=白底黑字 1=黑底白字（点阵数据不变，固件决定笔画/背景各画什么颜色）
    const uint8_t bg     = DIGIT_INVERT ? UC8179::INK_BLACK : UC8179::INK_WHITE;
    const uint8_t stroke = DIGIT_INVERT ? UC8179::INK_WHITE : UC8179::INK_BLACK;
    epd.fillScreen(bg);
    drawDigitRotated(DIGITS[d], stroke);
    // epd.displayManaged();
    epd.displayQuality();  // TR7 画质刷; false=不掉电(每次 POF 实测有"白闪暗"掉电瞬态)
    // epd.displayManaged(false);  // 备选: 2 快 1 质自动循环, 同样不掉电
    Serial.print("已显示: ");
    Serial.print(d);
    Serial.print("  托管刷新计数: ");
    Serial.println(epd.fastRefreshCount());  // 1,2=TR4 快刷, 0=刚做过 TR7 质量刷
}

static void handleSerial() {
    while (Serial.available()) {
        const int c = Serial.read();
        if (c >= '0' && c <= '9') {
            polling = true;
            lastSwitch = millis();
            showDigit(c - '0');
        } else if (c == 's') {
            polling = !polling;
            Serial.println(polling ? "轮询: 继续" : "轮询: 暂停");
        } else if (c == 'q') {
            Serial.println("TR7 质量刷新（清残影）");
            epd.displayQuality();
        } else if (c == 'p') {
            Serial.println("TR7 + POF 收尾停电");
            epd.displayQuality();  // powerOffAfter=true
        } else if (c == '\n' || c == '\r' || c == ' ') {
            continue;
        } else {
            Serial.println("命令: 0-9=显示数字  s=暂停/继续  q=质量刷  p=收尾停电");
        }
    }
}

void setup() {
    Serial.begin(115200);
    delay(500);
    epd.setDebug(&Serial);

    if (KEEP_POWERED) epd.setFastPowerPolicy(UC8179::FAST_KEEP_POWERED);

    if (!epd.begin()) {
        Serial.print("epd.begin() 失败: ");
        Serial.println(epd.errorText());
        while (true) delay(1000);
    }

    Serial.println("maintenanceClear: 恢复刷（全黑->全白），清除旧固件残留暗渍...");
    epd.maintenanceClear();

    Serial.println("DigitPoll 启动: 轮询 0-9；串口命令 0-9 / s / q / p");
    showDigit(currentDigit);
    lastSwitch = millis();
}

void loop() {
    handleSerial();

    if (polling && millis() - lastSwitch >= DIGIT_INTERVAL_MS) {
        lastSwitch = millis();
        showDigit((currentDigit + 1) % 10);
    }
}
