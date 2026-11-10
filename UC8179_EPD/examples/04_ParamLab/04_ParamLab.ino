/*
 * 04_ParamLab — UC8179 参数对比实验台（按 TUNING.md 的思路）
 * ==================================================================
 * 三个阶段：
 *   1) P  可控性探测：逐项用“极端值”判断寄存器到底有没有用
 *         （OTP 波形可能接管 VCOM/VDH/帧率，先证明再调）
 *   2) S  规格内 TR 扫描：TR0~TR5（0~50°C 面板温区）
 *      Z  超温 TR 实验：TR6~TR7（80~127°C 波形，仅实验，不作生产）
 *   3) 单项微调：t/v/h/l/g/f/c/b/e/s + k 重画，逐项对比
 *
 * 接线同 01_HelloWorld：CS=10 DC=3 BUSY=2 SCK=6 SDA=7 RST=-1
 * 串口 115200，一行一条命令，回车执行。
 *
 * 命令（数字默认十进制，支持 0x 前缀）：
 *   ?            帮助
 *   P            启动可控性探测向导（y=有变化 n=没变化 s=跳过 q=退出）\n *   i            打印诊断（REV/温度/档位/VCOM/FLG/OTP/PBC）\n *   O            OTP 0x000~0x04F 十六进制转储\n *   R<pin>       换回读引脚并重新校准（例如 R5 / R7）
 *   p            打印当前参数 + 耗时
 *   l            输出一行 CSV 记录（便于抄表）
 *   t0..t7       直接选 TR0~TR7
 *   x / X        下一档 / 上一档 TR
 *   a            温度自动分档
 *   o-8..o7      温度偏移
 *   v<0..79>     设 VCOM_DC（0x12=-1.00V，每步 0.05V）
 *   + / -        VCOM ±1 码
 *   h<0..63>     VDH   d<0..63>  VDL   g0..7  VGH/VGL
 *   f0..15       帧率码（6=50Hz）
 *   c0..15       CDI 间隔   b0..3 边框 LUT   e<0..255> EVS
 *   s            单/双平面切换
 *   k 重画   w 全白   B 全黑   G 残影测试   r 恢复基线
 *
 * 建议：先跑 P，再 S，再固定档位用 +/- 微调 VCOM（如果探测证明它有效）。
 */
#include <UC8179_EPD.h>

#define PIN_CS 10
#define PIN_DC 3
#define PIN_BUSY 2
#define PIN_SCK 6
#define PIN_MOSI 7
#define PIN_RST -1
#define PIN_MISO PIN_MOSI  // SDA 单线回读；若另接 MISO 改成该引脚

UC8179_EPD epd(PIN_CS, PIN_DC, PIN_RST, PIN_BUSY);

static int8_t gear = -1;  // -1=AUTO，0..7=强制档

// ---------------------------------------------------------------- 测试图
static void checker(int16_t x, int16_t y, int16_t w, int16_t h, int16_t cell) {
  for (int16_t j = 0; j < h; j += cell) {
    for (int16_t i = 0; i < w; i += cell) {
      if (((i / cell) + (j / cell)) & 1) {
        epd.fillRect(x + i, y + j, cell, cell, UC8179_BLACK);
      }
    }
  }
}

static void drawPattern(const char *title) {
  const int16_t W = epd.width();
  const int16_t H = epd.height();

  epd.fillScreen(UC8179_WHITE);
  epd.drawRect(0, 0, W, H, UC8179_BLACK);  // 1px 边框

  epd.drawString(10, 10, title, UC8179_BLACK, 2);

  // 纯黑 / 纯白块
  epd.fillRect(10, 45, 110, 95, UC8179_BLACK);
  epd.drawRect(130, 45, 110, 95, UC8179_BLACK);

  // 1px / 2px / 4px 棋盘
  checker(255, 45, 115, 95, 1);
  checker(380, 45, 115, 95, 2);
  checker(505, 45, 130, 95, 4);

  // 1px 横线 / 竖线
  for (int16_t y = 160; y < 300; y += 4) {
    epd.drawLine(10, y, 310, y, UC8179_BLACK);
  }
  for (int16_t x = 330; x < 630; x += 4) {
    epd.drawLine(x, 160, x, 300, UC8179_BLACK);
  }

  // 文字
  epd.drawString(10, 315, "THE QUICK BROWN FOX 0123456789", UC8179_BLACK, 2);
  epd.drawString(10, 345, "abcdefghijklmnopqrstuvwxyz", UC8179_BLACK, 2);

  // 参数行
  char trLabel[20];
  if (epd.params().tsfix) {
    snprintf(trLabel, sizeof(trLabel), "TR%d FIX", gear);
  } else {
    snprintf(trLabel, sizeof(trLabel), "AUTO act=TR%d", epd.currentTrGear());
  }
  epd.drawStringf(10, H - 60, UC8179_BLACK, 1,
                  "%-14s VCOM=0x%02X VDH=%02X VDL=%02X VG=%u", trLabel,
                  epd.params().vdcs, epd.params().vdh, epd.params().vdl,
                  epd.params().vg);
  epd.drawStringf(10, H - 44, UC8179_BLACK, 1,
                  "FRS=%u CDI=%u TCON=%02X EVS=%02X BDV=%u %s",
                  epd.params().frs, epd.params().cdi2, epd.params().tcon,
                  epd.params().evs, (epd.params().cdi1 >> 4) & 3,
                  epd.singlePlane() ? "S1" : "S0");
  epd.drawStringf(10, H - 28, UC8179_BLACK, 1,
                  "T=%dC GEAR=TR%d READ=%s ref=%lu/%lu ms", epd.readTemperature(),
                  epd.currentTrGear(), epd.readEnabled() ? "ok" : "off",
                  (unsigned long)epd.lastRefreshMs(),
                  (unsigned long)epd.lastDisplayMs());
}

static bool redraw(const char *title) {
  drawPattern(title);
  bool ok = epd.display();
  Serial.printf("  refresh %lu ms, display %lu ms %s\n",
                (unsigned long)epd.lastRefreshMs(),
                (unsigned long)epd.lastDisplayMs(), ok ? "OK" : "TIMEOUT");
  return ok;
}

// ---------------------------------------------------------------- 恢复基线
// 恢复基线：不再软复位（软复位后命令可能被忽略，导致 PON 卡死），
// 直接把所有寄存器重写一遍；VDCS 由 resetParams() 显式写回 0x00。
static void restoreBaseline() {
  epd.resetParams();
  gear = -1;
}

// ---------------------------------------------------------------- 探测向导
static const uint8_t PROBE_COUNT = 6;
static const char *const PROBE_NAME[PROBE_COUNT] = {
    "VCOM -0.10V", "VDH/VDL 2.4V", "FRS 10Hz", "CDI 2hsync", "TCON 0x00",
    "dual plane"};
static const char *const PROBE_HINT[PROBE_COUNT] = {
    "对比度应明显变淡", "画面应明显发白", "刷新应慢约 5 倍（约 12 秒）",
    "边缘/拖影应有变化", "细线串扰应有变化", "display 时间应增加（数据翻倍）"};

static bool probeActive = false;
static uint8_t probeIdx = 0;
static uint8_t probeResult[PROBE_COUNT];  // 0=未测 1=有变化 2=无变化 3=跳过
static uint32_t probeMs[PROBE_COUNT];
static uint32_t probeDisp[PROBE_COUNT];
static uint32_t probeBaseMs = 0;
static uint32_t probeBaseDisp = 0;

static void applyProbe(uint8_t i) {
  switch (i) {
    case 0: epd.setVcomDc(0x00); break;
    case 1: epd.setSourceVoltage(0x00, 0x00); break;
    case 2: epd.setFrameRateCode(0x01); break;  // 10Hz，避免超时
    case 3: epd.setCdiInterval(0x00); break;
    case 4: epd.setTcon(0x00); break;
    case 5: epd.setSinglePlane(false); break;
  }
}

static void askProbe() {
  Serial.printf(
      "\n[%u/%u] %s\n  期望：%s\n  当前 refresh=%lu ms / display=%lu ms"
      "（基线 %lu / %lu ms）\n",
      (unsigned)(probeIdx + 1), (unsigned)PROBE_COUNT, PROBE_NAME[probeIdx],
      PROBE_HINT[probeIdx], (unsigned long)epd.lastRefreshMs(),
      (unsigned long)epd.lastDisplayMs(), (unsigned long)probeBaseMs,
      (unsigned long)probeBaseDisp);
  Serial.println(F("  看到变化按 y，没变化按 n，跳过 s，退出 q"));
}

static void startProbe() {
  probeActive = true;
  probeIdx = 0;
  for (uint8_t i = 0; i < PROBE_COUNT; i++) {
    probeResult[i] = 0;
    probeMs[i] = 0;
    probeDisp[i] = 0;
  }

  Serial.println(F("\n== 可控性探测 =="));
  Serial.println(F("先测基线，再逐项设极端值。只看画面/耗时，不做主观调参。"));
  Serial.printf("基线: T=%dC, 自动档=TR%d, 读回=%s\n", epd.readTemperature(),
                epd.currentTrGear(), epd.readEnabled() ? "ok" : "off");
  restoreBaseline();
  redraw("BASELINE");
  probeBaseMs = epd.lastRefreshMs();
  probeBaseDisp = epd.lastDisplayMs();

  applyProbe(0);
  redraw(PROBE_NAME[0]);
  askProbe();
}

static void finishProbe() {
  probeActive = false;
  Serial.println(F("\n== 探测结果 =="));
  for (uint8_t i = 0; i < PROBE_COUNT; i++) {
    const char *r = probeResult[i] == 1   ? "有变化 -> 主机可控"
                    : probeResult[i] == 2 ? "无变化 -> 被 OTP 接管"
                    : probeResult[i] == 3 ? "跳过"
                                          : "未测";
    Serial.printf("  %-14s %-22s ref %+ld ms, disp %+ld ms", PROBE_NAME[i], r,
                  (long)(probeMs[i] - probeBaseMs),
                  (long)(probeDisp[i] - probeBaseDisp));
    if (i == 2 && (probeMs[i] - probeBaseMs) < 100) {
      Serial.print(F("  <- 帧率被 TR 覆盖"));
    }
    if (i == 5 && (probeDisp[i] - probeBaseDisp) > 40) {
      Serial.print(F("  <- 双平面生效（仅数据时间）"));
    }
    Serial.println();
  }
  Serial.println(F("提示：VCOM/VDH 无变化说明 OTP TR 接管，后续别在这上面调；"));
  Serial.println(F("      FRS 无变化说明 TR 帧率接管；CDI/TCON 无变化说明时序被忽略。"));
  restoreBaseline();
  redraw("BASELINE");
}

static void probeKey(char c) {
  if (c == 'q' || c == 'Q') {
    Serial.println(F("探测已退出"));
    finishProbe();
    return;
  }
  if (c != 'y' && c != 'Y' && c != 'n' && c != 'N' && c != 's' && c != 'S') {
    return;
  }
  probeMs[probeIdx] = epd.lastRefreshMs();
  probeDisp[probeIdx] = epd.lastDisplayMs();
  if (c == 'y' || c == 'Y') {
    probeResult[probeIdx] = 1;
  } else if (c == 'n' || c == 'N') {
    probeResult[probeIdx] = 2;
  } else {
    probeResult[probeIdx] = 3;
  }

  probeIdx++;
  if (probeIdx >= PROBE_COUNT) {
    finishProbe();
    return;
  }
  restoreBaseline();
  applyProbe(probeIdx);
  redraw(PROBE_NAME[probeIdx]);
  askProbe();
}

// ---------------------------------------------------------------- TR 扫描
static void sweepTR(uint8_t first, uint8_t last, const char *tag) {
  Serial.printf("== sweep %s TR%u..TR%u ==\n", tag, first, last);
  for (uint8_t g = first; g <= last; g++) {
    gear = g;
    epd.setWaveformGear(g);
    drawPattern(tag);
    bool ok = epd.display();
    Serial.printf("TR%u: refresh %lu ms, display %lu ms %s\n", g,
                  (unsigned long)epd.lastRefreshMs(),
                  (unsigned long)epd.lastDisplayMs(), ok ? "OK" : "TIMEOUT");
  }
  epd.setTemperatureAuto();
  gear = -1;
  Serial.println(F("== sweep done, back to AUTO =="));
}

// ---------------------------------------------------------------- 其它
static void ghostTest() {
  Serial.println(F("ghost test: black/white x2"));
  for (uint8_t i = 0; i < 2; i++) {
    epd.fillScreen(UC8179_BLACK);
    epd.display();
    delay(400);
    epd.fillScreen(UC8179_WHITE);
    epd.display();
    delay(400);
  }
  redraw("AFTER GHOST");
}

static void printCsv() {
  Serial.printf(
      "LOG,%d,%d,0x%02X,0x%02X,0x%02X,%u,%u,0x%02X,0x%02X,%u,%s,%d,%lu,%lu\n",
      epd.params().tsfix ? gear : -1, epd.currentTrGear(), epd.params().vdcs,
      epd.params().vdh, epd.params().vdl, epd.params().frs,
      epd.params().cdi2, epd.params().tcon, epd.params().evs,
      (epd.params().cdi1 >> 4) & 3, epd.singlePlane() ? "S1" : "S0",
      epd.readTemperature(), (unsigned long)epd.lastRefreshMs(),
      (unsigned long)epd.lastDisplayMs());
}

static void dumpOtp() {
  uint8_t buf[0x50];
  if (!epd.readOtp(0x0000, buf, sizeof(buf))) {
    Serial.println(F("OTP read failed: read=off or wiring?"));
    return;
  }
  for (uint8_t off = 0; off < sizeof(buf); off += 16) {
    Serial.printf("%04X:", off);
    for (uint8_t i = 0; i < 16 && off + i < sizeof(buf); i++) {
      Serial.printf(" %02X", buf[off + i]);
    }
    Serial.println();
  }
}

static void help() {
  Serial.println(F("i 诊断  O OTP转储  P 探测  p 参数  l CSV  S/Z TR扫描"));
  Serial.println(F("t0..t7 TR  x/X 上下档  a 自动  o<-8..7> 温度偏移"));
  Serial.println(F("v<code> VCOM  +/- 微调  h<vdh> d<vdl> g<0..7> VG"));
  Serial.println(F("f<0..15> 帧率  c<0..15> CDI  b<0..3> 边框  e<evs> EVS"));
  Serial.println(F("s 平面 V 厂商全刷(自动) F 厂商快刷(TR6) U 本库S1 K POF开关"));
  Serial.println(F("H 影子帧开关（真差分，需 38.8KB）"));
  Serial.println(F("k 重画  w 白  B 黑  G 残影  r 基线"));
  Serial.println(F("R<pin> 换回读引脚  z 边框Hi-Z开关"));
}

static void handleLine(char *line) {
  while (*line == ' ') line++;
  if (!*line) return;
  char cmd = line[0];
  long arg = strtol(line + 1, nullptr, 0);

  switch (cmd) {
    case '?': help(); break;

    case 'i':
      epd.printDiagnostics(Serial);
      epd.printOtpSummary(Serial);
      break;

    case 'O':
      dumpOtp();
      break;

    case 'V':  // 厂商全刷: CDI=0x29 双平面, OLD=NEW, 自动温度
      epd.invalidateShadow();
      epd.setOldFrameSame(true);
      epd.setSinglePlane(false);
      epd.setCdiRegister(0x29, 0x07);
      epd.setTemperatureAuto();
      gear = -1;
      Serial.println(F("vendor full: CDI=29/07, OLD=NEW, AUTO"));
      redraw("VENDOR FULL");
      break;

    case 'F':  // GxEPD2 GDEQ0583T31 快刷: CDI=0x29 + TSSET=0x5A, OLD=NEW
      epd.invalidateShadow();
      epd.setOldFrameSame(true);
      epd.setSinglePlane(false);
      epd.setCdiRegister(0x29, 0x07);
      epd.setTemperatureForce(0x5A);
      gear = 6;
      Serial.println(F("vendor fast: CDI=29/07, TSSET=5A (TR6)"));
      redraw("VENDOR FAST");
      break;

    case 'U':  // 回到本库默认: CDI=0x12 单平面
      epd.invalidateShadow();
      epd.setOldFrameSame(false);
      epd.setCdiRegister(0x12, 0x07);
      epd.setTemperatureAuto();
      gear = -1;
      Serial.println(F("library S1: CDI=12/07, AUTO"));
      redraw("LIB S1");
      break;

    case 'K': {
      bool keep = !epd.powerOffAfterRefresh();
      epd.setPowerOffAfterRefresh(keep);
      Serial.printf("power off after refresh: %s\n", keep ? "no (keep PON)" : "yes");
      break;
    }

    case 'H': {
      bool on = !epd.shadowEnabled();
      bool ok = epd.enableShadow(on);
      Serial.printf("shadow frame: %s\n", ok && on ? "on" : "off");
      redraw("SHADOW");
      break;
    }

    case 'z': {
      static bool borderHz = false;
      borderHz = !borderHz;
      epd.setBorderHiZ(borderHz);
      Serial.printf("border Hi-Z: %s\n", borderHz ? "on" : "off");
      redraw("BORDER");
      break;
    }

    case 'R': {
      if (arg >= 0 && arg <= 48) {
        bool ok = epd.setReadPin((int8_t)arg);
        Serial.printf("read pin=%ld -> %s\n", arg, ok ? "ok" : "fail");
        if (ok) epd.printOtpSummary(Serial);
      } else {
        Serial.printf("read pin=%d read=%s\n", epd.readPin(),
                      epd.readEnabled() ? "ok" : "off");
      }
      redraw("READPIN");
      break;
    }

    case 'P':
      startProbe();
      break;

    case 'p':
      epd.printConfig(Serial);
      Serial.printf("gear=%d refresh=%lu ms display=%lu ms\n", gear,
                    (unsigned long)epd.lastRefreshMs(),
                    (unsigned long)epd.lastDisplayMs());
      break;

    case 'l':
      printCsv();
      break;

    case 't':
      if (arg >= 0 && arg <= 7) {
        gear = (int8_t)arg;
        epd.setWaveformGear(gear);
        Serial.printf("TR%u\n", gear);
        redraw("TR");
      }
      break;

    case 'x':
      gear = (gear + 1) & 7;
      epd.setWaveformGear(gear);
      Serial.printf("TR%u\n", gear);
      redraw("TR");
      break;

    case 'X':
      if (gear < 0) gear = 0;
      gear = (gear + 7) & 7;
      epd.setWaveformGear(gear);
      Serial.printf("TR%u\n", gear);
      redraw("TR");
      break;

    case 'a':
      epd.setTemperatureAuto();
      Serial.println(F("temperature: AUTO"));
      redraw("AUTO");
      break;

    case 'o':
      epd.setTemperatureOffset((int8_t)arg);
      Serial.printf("temp offset: %ld\n", arg);
      redraw("OFFSET");
      break;

    case 'v':
      epd.setVcomDc((uint8_t)arg);
      Serial.printf("VCOM code 0x%02lX (%.2f V)\n", arg & 0x7F,
                    -(0.10 + 0.05 * (arg & 0x7F)));
      redraw("VCOM");
      break;

    case '+':
      epd.setVcomDc((uint8_t)((epd.params().vdcs + 1) & 0x7F));
      Serial.printf("VCOM code 0x%02X\n", epd.params().vdcs);
      redraw("VCOM+");
      break;

    case '-':
      epd.setVcomDc((uint8_t)((epd.params().vdcs + 127) & 0x7F));
      Serial.printf("VCOM code 0x%02X\n", epd.params().vdcs);
      redraw("VCOM-");
      break;

    case 'h':
      epd.setSourceVoltage((uint8_t)arg, epd.params().vdl);
      Serial.printf("VDH=0x%02lX (%.1f V)\n", arg & 0x3F,
                    2.4 + 0.2 * (arg & 0x3F));
      redraw("VDH");
      break;

    case 'd':
      epd.setSourceVoltage(epd.params().vdh, (uint8_t)arg);
      Serial.printf("VDL=0x%02lX (%.1f V)\n", arg & 0x3F,
                    -(2.4 + 0.2 * (arg & 0x3F)));
      redraw("VDL");
      break;

    case 'g':
      epd.setGateVoltage((uint8_t)arg);
      Serial.printf("VG=%ld\n", arg);
      redraw("VG");
      break;

    case 'f':
      epd.setFrameRateCode((uint8_t)arg);
      Serial.printf("FRS=%ld\n", arg);
      redraw("FRS");
      break;

    case 'c':
      epd.setCdiInterval((uint8_t)arg);
      Serial.printf("CDI=%ld\n", arg);
      redraw("CDI");
      break;

    case 'b':
      epd.setBorderLut((uint8_t)arg);
      Serial.printf("BDV=%ld\n", arg);
      redraw("BDV");
      break;

    case 'e':
      epd.setBorderEnd((uint8_t)arg);
      Serial.printf("EVS=0x%02lX\n", arg & 0xFF);
      redraw("EVS");
      break;

    case 's':
      epd.setSinglePlane(!epd.singlePlane());
      Serial.printf("plane: %s\n", epd.singlePlane() ? "S1 single" : "S0 dual");
      redraw("PLANE");
      break;

    case 'k': redraw("TEST"); break;

    case 'w':
      epd.fillScreen(UC8179_WHITE);
      epd.display();
      break;

    case 'B':
      epd.fillScreen(UC8179_BLACK);
      epd.display();
      break;

    case 'S': sweepTR(0, 5, "IN-SPEC"); break;
    case 'Z': sweepTR(6, 7, "OVER-TEMP"); break;

    case 'G': ghostTest(); break;

    case 'r':
      restoreBaseline();
      Serial.println(F("baseline restored (S1 + AUTO)"));
      redraw("BASELINE");
      break;

    default: help(); break;
  }
}

// ---------------------------------------------------------------- 串口
static char lineBuf[64];
static uint8_t lineLen = 0;

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println();
  Serial.println(F("UC8179 ParamLab v1.4.2"));
  help();

  if (!epd.begin(PIN_SCK, PIN_MISO, PIN_MOSI, 4000000)) {
    Serial.println(F("!! init failed"));
    while (true) delay(1000);
  }
  Serial.println(F("CSV 表头: LOG,TR_REQ,GEAR_ACT,VCOM,VDH,VDL,FRS,CDI,TCON,EVS,BDV,PLANE,TEMP,REF_MS,DISP_MS"));
  Serial.printf("read pin=%d, read=%s\n", PIN_MISO,
                epd.readEnabled() ? "ok" : "off (check PIN_MISO/wiring)");
  epd.printDiagnostics(Serial);
  epd.printOtpSummary(Serial);
  redraw("HELLO");
}

void loop() {
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\r') continue;
    if (probeActive) {
      if (c != '\n') probeKey(c);
      continue;
    }
    if (c == '\n') {
      lineBuf[lineLen] = 0;
      handleLine(lineBuf);
      lineLen = 0;
      continue;
    }
    if (lineLen < sizeof(lineBuf) - 1) {
      lineBuf[lineLen++] = c;
    }
  }
  delay(10);
}
















