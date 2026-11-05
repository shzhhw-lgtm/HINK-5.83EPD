/*
 * PanClean.ino - UC8179 面板电荷/粒子整理程序（纯 TR7 黑白交替清屏）
 *
 * 用途：对面板做深度整理——释放俘获电荷、消除极化、抹平残影印子。
 * 适用场景：经历过大量 TR4 快刷 / 每次刷新后 POF / 自定义波形实验的面板，
 *           投入正式使用（如时钟项目）前的恢复性清理。
 *
 * 原理：TR7（OTP TR7 波形）自带"砸黑归零 + VCOM 交流摆动 + 收尾静置"的
 *       完整重整结构（S1 砸黑49帧 -> 平衡闪32帧 -> 分类推32帧 -> 收尾22帧
 *       -> 静置），是这套 OTP 里唯一的重整波形。黑白交替多轮 = 让粒子在
 *       两个稳定态之间反复走完整波形，逐轮释放极化（实测 POF 瞬态随 TR7
 *       次数递减即为佐证）。
 *
 * 流程：N 轮 [全黑 TR7 -> 停顿 -> 全白 TR7 -> 停顿]，轮内以白结束，
 *       最终停在纯白屏；结束后 POF 断电收尾（粒子新鲜归位，瞬态最小）。
 *       整理期间全程保持供电（中途 POF 会把极化踢动一遍，前功尽弃）。
 *
 * 说明：
 *   - 单屏版：一次接一片，清理完换下一片（库同总线多实例需小改，此处回避）。
 *   - 每步都用库的 displayQuality(false) = S1 + TR7，保持供电不掉电。
 *   - 整理效果会随后续 TR4 使用重新变脏，属正常；装上时钟后如果走
 *     "每次变化都 TR7"的策略，面板会持续保持整理状态。
 *
 * 串口命令（115200）：
 *   s  开始整理（当前轮数，默认 10 轮，约 70 秒）
 *   + / -  调整轮数（1~30），5=快速 10=标准 20=深度
 *   b  单次全黑 TR7        w  单次全白 TR7
 *   z  POF 断电            i  状态查询   h  帮助
 *
 * 目标平台 ESP32-C3 + 库默认引脚（CS=10 DC=3 RST=4 BUSY=5 SCK=6 SDA=7）。
 */

#include <Arduino.h>
#include <UC8179.h>

static UC8179 epd;

static int s_cycles = 10;          // 黑白交替轮数（每轮 = 黑 + 白）
static const uint16_t SETTLE_MS = 500;  // 每步之间停顿，给泵和粒子喘息

static void printErr(const char* what) {
    Serial.printf("[FAIL] %s (err=%d %s)\n", what, epd.lastError(), epd.errorText());
}

// 单步：填充颜色 + TR7 刷新（保持供电），返回耗时
static uint32_t stepColor(uint8_t color) {
    epd.fillScreen(color);
    uint32_t t0 = millis();
    if (!epd.displayQuality(/*powerOffAfter=*/false)) {
        printErr("TR7 step");
        return 0;
    }
    return millis() - t0;
}

// 完整整理：N 轮黑白交替，终点白屏，收尾 POF
static void runClean() {
    Serial.printf("\n===== 面板整理开始：%d 轮黑白交替（每轮约 7 秒，全程约 %d 秒）=====\n",
                  s_cycles, s_cycles * 7);
    uint32_t t0 = millis();
    for (int i = 1; i <= s_cycles; i++) {
        uint32_t tb = stepColor(UC8179::INK_BLACK);
        if (tb == 0) return;
        delay(SETTLE_MS);
        uint32_t tw = stepColor(UC8179::INK_WHITE);
        if (tw == 0) return;
        Serial.printf("  轮 %2d/%d  黑 %lu ms | 白 %lu ms\n",
                      i, s_cycles, (unsigned long)tb, (unsigned long)tw);
        if (i < s_cycles) delay(SETTLE_MS);
    }
    uint32_t total = millis() - t0;
    Serial.printf("===== 整理完成：共 %lu ms，当前为纯白屏。按 z 断电收尾 =====\n",
                  (unsigned long)total);
}

static void printHelp() {
    Serial.println(F(
        "\n=== PanClean 面板电荷整理（纯 TR7） ===\n"
        "  s  开始整理（黑白交替 x N 轮，终点白屏，收尾不自动 POF）\n"
        "  + / -  调整轮数 1~30（当前值见提示）  5=快速 10=标准 20=深度\n"
        "  b  单次全黑 TR7     w  单次全白 TR7\n"
        "  z  POF 断电收尾     i  状态查询     h  帮助\n"
        "提示：整理期间保持供电；清完一片换下一片时，直接换线重跑 s 即可。\n"));
}

static void printStatus() {
    uint8_t flg = 0;
    bool ok = epd.readFLG(flg);
    Serial.printf("状态: 供电=%d 轮数=%d FLG=%s0x%02X\n",
                  epd.isPowered() ? 1 : 0, s_cycles,
                  ok ? "" : "不可读 ", ok ? flg : 0);
}

static void handleKey(char c) {
    switch (c) {
        case 's': runClean(); break;
        case '+':
            if (s_cycles < 30) s_cycles++;
            Serial.printf("轮数 = %d\n", s_cycles);
            break;
        case '-':
            if (s_cycles > 1) s_cycles--;
            Serial.printf("轮数 = %d\n", s_cycles);
            break;
        case 'b': {
            uint32_t dt = stepColor(UC8179::INK_BLACK);
            if (dt) Serial.printf("全黑 TR7 %lu ms\n", (unsigned long)dt);
            break;
        }
        case 'w': {
            uint32_t dt = stepColor(UC8179::INK_WHITE);
            if (dt) Serial.printf("全白 TR7 %lu ms\n", (unsigned long)dt);
            break;
        }
        case 'z':
            if (!epd.powerOff()) printErr("POF");
            else Serial.println("已 POF（粒子新鲜归位状态断电，瞬态最小）");
            break;
        case 'i': printStatus(); break;
        case 'h': printHelp(); break;
        default: break;
    }
}

void setup() {
    Serial.begin(115200);
    delay(400);
    Serial.println("\nPanClean — 面板电荷/粒子整理（纯 TR7 黑白交替）");

    epd.setDebug(&Serial);
    if (!epd.begin()) {
        printErr("begin");
        while (true) delay(1000);
    }

    // 开机先建立已知白底（TR7，保持供电）
    stepColor(UC8179::INK_WHITE);

    printHelp();
    Serial.printf("当前轮数 %d。接好一片屏后按 s 开始；清完换下一片再按 s。\n\n", s_cycles);
}

void loop() {
    while (Serial.available()) {
        char c = (char)Serial.read();
        if (c != '\r' && c != '\n') handleKey(c);
    }
    delay(2);
}
