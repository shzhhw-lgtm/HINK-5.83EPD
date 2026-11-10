/*
 * Clock4_EPD v2 — 四屏时钟（单一时基 + 绝对边界 + 只刷变化片）
 * ==================================================================
 * 设计要点：
 *   1. 时间 = UTC 锚点 + esp_timer_get_time()，64 位单调，不用 millis 步进；
 *   2. SNTP 只在回调里重锚，主循环从不逐秒比对 NTP；
 *   3. 调度用绝对分钟边界：T-5s 预写，T 触发，错过最多补刷一次；
 *   4. 只刷新数字真正变化的屏，通常每分钟 1 片；
 *   5. 四片共享一块 38,880 字节帧缓存；
 *   6. 默认 TR7 质量刷，f 可切 TR4；刷完延迟 2s POF。
 *
 * 串口：T#### 设时间  W|SSID|密码 存 WiFi  c 深清  f 快刷  q 质量刷  d 状态  h 帮助
 */
#include <Arduino.h>
#include <UC8179_EPD.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <esp_sntp.h>
#include <esp_timer.h>
#include <time.h>
#include <sys/time.h>
#include "digit_font.h"

// ------------------------------------------------------------------
// 配置
// ------------------------------------------------------------------
#define TZ_OFFSET_SEC        (8 * 3600)
#define NTP_SERVER_1         "ntp.aliyun.com"
#define NTP_SERVER_2         "ntp.tencent.com"
#define SYNC_INTERVAL_MS     (86400UL * 1000)  // 24 小时校一次
#define PREP_LEAD_SEC        5                 // T-5s 预写
#define LATE_TOLERANCE_SEC   3                 // 超过 3s 视为错过边界
#define REFRESH_TIMEOUT_MS   12000
#define POF_DELAY_MS         2000              // 刷完延迟 2s POF；0=保持 PON
#define CLEAN_CYCLES         5
#define CONNECTED_PANELS     4
#define DEFAULT_QUALITY      1                 // 1=TR7, 0=TR4
#define USE_12H              0
#define BLANK_LEADING_ZERO   0                 // 0=24h 制固定显示 0；1=小时<10 时十位留白

// ------------------------------------------------------------------
// 引脚
// ------------------------------------------------------------------
#define PIN_SCK   6
#define PIN_SDA   7
#define PIN_DC    3
#define PIN_RST   4       // 共享 RST；若接板载 EN 改成 -1
#define PIN_MISO  PIN_SDA

UC8179_EPD panHT( 10, PIN_DC, PIN_RST,  2);
UC8179_EPD panHO(  0, PIN_DC, PIN_RST,  1);
UC8179_EPD panMT(  5, PIN_DC, PIN_RST,  8);
UC8179_EPD panMO( 18, PIN_DC, PIN_RST, 19);
UC8179_EPD* pans[4] = { &panHT, &panHO, &panMT, &panMO };
const char* panName[4] = { "时十", "时个", "分十", "分个" };
static const int8_t panelCs[4]   = { 10, 0, 5, 18 };
static const int8_t panelBusy[4] = { 2, 1, 8, 19 };
static bool beginOk[4] = { false, false, false, false };

// 四片共享一块帧缓存（省 116KB）
static uint8_t sharedFrame[UC8179_EPD::bufferSize()];

// ------------------------------------------------------------------
// 时间锚点
// ------------------------------------------------------------------
static int64_t  anchorUtc = 0;      // 锚点 UTC 秒
static int64_t  anchorUs  = 0;      // 锚点对应的 esp_timer 微秒
static volatile bool syncFlag = false;
static bool     timeValid = false;

static int64_t nowUtc() {
    return anchorUtc + (esp_timer_get_time() - anchorUs) / 1000000;
}

void onSntpSync(struct timeval *tv) {
    (void)tv;
    syncFlag = true;
}

// ------------------------------------------------------------------
// 显示状态
// ------------------------------------------------------------------
static uint8_t shown[4] = { 0xFF, 0xFF, 0xFF, 0xFF };
static uint8_t tgt[4]   = { 0xFF, 0xFF, 0xFF, 0xFF };
static uint8_t pendingMask = 0;
static bool    prepped = false;
static int64_t targetUtc = 0;
static uint32_t pofAt = 0;
static bool    qualityMode = DEFAULT_QUALITY;

// 网络
static Preferences prefs;
static WebServer apWeb(80);
static bool configuring = false;
static bool portalSaved = false;
static uint32_t lastWifiTry = 0;
static bool portalTried = false;

// 串口
static char s_line[96] = { 0 };
static uint8_t s_len = 0;

// ------------------------------------------------------------------
// 时间与数字
// ------------------------------------------------------------------
void digitsFor(int64_t utc, uint8_t out[4]) {
    time_t t = (time_t)utc;
    struct tm lt;
    localtime_r(&t, &lt);
    uint8_t hh = lt.tm_hour, mm = lt.tm_min;
    if (USE_12H) hh = (hh % 12 == 0) ? 12 : hh % 12;
    if (BLANK_LEADING_ZERO && hh < 10) out[0] = 10;  // 10 = 空白
    else                               out[0] = hh / 10;
    out[1] = hh % 10;
    out[2] = mm / 10;
    out[3] = mm % 10;
}

void applyGear() {
    for (uint8_t i = 0; i < CONNECTED_PANELS; i++) {
        pans[i]->setWaveformGear(qualityMode ? 7 : 4);
    }
}

// 把字模按当前逻辑画布居中；放不下时转 90°
void renderDigit(uint8_t idx, uint8_t d) {
    UC8179_EPD* p = pans[idx];
    p->fillScreen(UC8179_WHITE);
    if (d > 9) return;  // 空白

    const int16_t bw = DIGIT_W, bh = DIGIT_H, rb = DIGIT_ROW_BYTES;
    const int16_t lw = p->width(), lh = p->height();
    bool rot90 = (lh < bh);
    int16_t dw = rot90 ? bh : bw;
    int16_t dh = rot90 ? bw : bh;
    int16_t x0 = (lw - dw) / 2, y0 = (lh - dh) / 2;

    for (int16_t by = 0; by < bh; by++) {
        const uint8_t* row = DIGITS[d] + (int32_t)by * rb;
        for (int16_t bx = 0; bx < bw; bx++) {
            if (!(row[bx >> 3] & (uint8_t)(0x80 >> (bx & 7)))) continue;
            int16_t x, y;
            if (rot90) { x = x0 + (bh - 1 - by); y = y0 + bx; }
            else       { x = x0 + bx;            y = y0 + by; }
            p->drawPixel(x, y, UC8179_BLACK);
        }
    }
}

void schedulePof() {
    pofAt = (POF_DELAY_MS > 0) ? (millis() + POF_DELAY_MS) : 0;
}

void servicePof() {
    if (!pofAt) return;
    if ((int32_t)(millis() - pofAt) < 0) return;
    pofAt = 0;
    for (uint8_t i = 0; i < CONNECTED_PANELS; i++) pans[i]->powerOff();
}

// ------------------------------------------------------------------
// 刷新：预写 / 触发
// ------------------------------------------------------------------
void prep(int64_t utc) {
    uint32_t t0 = millis();
    uint8_t d[4];
    digitsFor(utc, d);
    pendingMask = 0;
    for (uint8_t i = 0; i < CONNECTED_PANELS; i++) {
        tgt[i] = d[i];
        if (d[i] == shown[i]) continue;
        renderDigit(i, d[i]);
        pans[i]->writeFrame();          // PON + 送数，不 DRF
        pendingMask |= (uint8_t)(1 << i);
    }
    if (pendingMask) Serial.printf("[prep] mask=%X %lu ms\n", pendingMask,
                                   (unsigned long)(millis() - t0));
}

void fire() {
    if (!pendingMask) return;
    uint8_t mask = pendingMask;
    uint32_t t0 = millis();
    applyGear();
    for (uint8_t i = 0; i < CONNECTED_PANELS; i++) {
        if (pendingMask & (1 << i)) pans[i]->triggerRefresh();
    }
    for (uint8_t i = 0; i < CONNECTED_PANELS; i++) {
        if (!(pendingMask & (1 << i))) continue;
        if (!pans[i]->waitRefresh(REFRESH_TIMEOUT_MS)) {
            Serial.printf("[FAIL] %s 刷新超时\n", panName[i]);
            shown[i] = 0xFF;             // 下一分钟整片重绘
        } else {
            shown[i] = tgt[i];
        }
    }
    pendingMask = 0;
    Serial.printf("[fire] mask=%X %lu ms\n", mask, (unsigned long)(millis() - t0));
    schedulePof();
}

// 立即按当前分钟刷新（首次校时、时间跳变、错过边界时用）
void showNow() {
    int64_t now = nowUtc();
    prep(now);
    fire();
    targetUtc = (now / 60 + 1) * 60;
    prepped = false;
}

// 全白清屏（开机、无时间时）
void clearAll() {
    pans[0]->fillScreen(UC8179_WHITE);   // 共享缓存
    for (uint8_t i = 0; i < CONNECTED_PANELS; i++) pans[i]->writeFrame();
    applyGear();
    for (uint8_t i = 0; i < CONNECTED_PANELS; i++) pans[i]->triggerRefresh();
    for (uint8_t i = 0; i < CONNECTED_PANELS; i++) pans[i]->waitRefresh(REFRESH_TIMEOUT_MS);
    for (uint8_t i = 0; i < CONNECTED_PANELS; i++) shown[i] = 0xFF;
    schedulePof();
}

void cleanAllPanels() {
    Serial.printf("[CLEAN] %d 轮黑白交替\n", CLEAN_CYCLES);
    applyGear();
    for (uint8_t c = 0; c < CLEAN_CYCLES; c++) {
        pans[0]->fillScreen(UC8179_BLACK);
        for (uint8_t i = 0; i < CONNECTED_PANELS; i++) pans[i]->writeFrame();
        for (uint8_t i = 0; i < CONNECTED_PANELS; i++) pans[i]->triggerRefresh();
        for (uint8_t i = 0; i < CONNECTED_PANELS; i++) pans[i]->waitRefresh(REFRESH_TIMEOUT_MS);

        pans[0]->fillScreen(UC8179_WHITE);
        for (uint8_t i = 0; i < CONNECTED_PANELS; i++) pans[i]->writeFrame();
        for (uint8_t i = 0; i < CONNECTED_PANELS; i++) pans[i]->triggerRefresh();
        for (uint8_t i = 0; i < CONNECTED_PANELS; i++) pans[i]->waitRefresh(REFRESH_TIMEOUT_MS);
        Serial.printf("  轮 %u/%u\n", c + 1, CLEAN_CYCLES);
    }
    for (uint8_t i = 0; i < CONNECTED_PANELS; i++) shown[i] = 0xFF;
    schedulePof();
}

// ------------------------------------------------------------------
// 校时
// ------------------------------------------------------------------
void reAnchor() {
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    if (tv.tv_sec < 1700000000) return;   // 未同步

    int64_t oldMinute = timeValid ? (nowUtc() / 60) : -1;
    anchorUtc = (int64_t)tv.tv_sec;
    anchorUs  = esp_timer_get_time() - (int64_t)tv.tv_usec;
    timeValid = true;

    int64_t newMinute = anchorUtc / 60;
    targetUtc = (newMinute + 1) * 60;
    prepped = false;

    if (oldMinute != newMinute) {
        time_t t = (time_t)anchorUtc;
        struct tm lt;
        localtime_r(&t, &lt);
        Serial.printf("[SYNC] %02d:%02d\n", lt.tm_hour, lt.tm_min);
        showNow();                         // 只补一次
    }
}

void setManualTime(uint8_t hh, uint8_t mm) {
    time_t now = time(nullptr);
    struct tm lt;
    if (now > 1700000000) {
        localtime_r(&now, &lt);
    } else {
        memset(&lt, 0, sizeof(lt));
        lt.tm_year = 126;  // 2026
        lt.tm_mon = 0;
        lt.tm_mday = 1;
    }
    lt.tm_hour = hh;
    lt.tm_min = mm;
    lt.tm_sec = 0;
    lt.tm_isdst = -1;
    time_t local = mktime(&lt);
    if (local == (time_t)-1) return;

    anchorUtc = (int64_t)local;
    anchorUs = esp_timer_get_time();
    timeValid = true;
    targetUtc = (anchorUtc / 60 + 1) * 60;
    prepped = false;
    showNow();
    Serial.printf("[TIME] 手动 %02d:%02d\n", hh, mm);
}

// ------------------------------------------------------------------
// WiFi / 配网
// ------------------------------------------------------------------
void portalStopAndConnect() {
    portalSaved = false;
    apWeb.stop();
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_STA);
    prefs.begin("clock4", true);
    String ssid = prefs.getString("ssid", "");
    String pass = prefs.getString("pass", "");
    prefs.end();
    if (ssid.length()) WiFi.begin(ssid.c_str(), pass.c_str());
    configuring = false;
    Serial.printf("[WIFI] 连接 \"%s\"\n", ssid.c_str());
}

void portalStart() {
    if (configuring) return;
    portalTried = true;
    WiFi.mode(WIFI_AP);
    WiFi.softAP("BigClock-Setup");
    apWeb.on("/", []() {
        apWeb.send(200, "text/html; charset=utf-8",
            "<meta charset='utf-8'><title>Clock4 配网</title>"
            "<div style='font-size:20px;margin:12px'>Clock4 WiFi 配网</div>"
            "<form action='/save' method='get' style='font-size:18px'>"
            "WiFi名称(SSID):<br><input name='ssid' style='width:95%;height:28px;font-size:16px'><br><br>"
            "密码:<br><input name='pass' type='password' style='width:95%;height:28px;font-size:16px'><br><br>"
            "<input type='submit' value='保存并连接' style='font-size:18px;padding:8px 20px'>"
            "</form>");
    });
    apWeb.on("/save", []() {
        String ssid = apWeb.arg("ssid");
        String pass = apWeb.arg("pass");
        if (!ssid.length()) {
            apWeb.send(200, "text/html; charset=utf-8",
                "<meta charset='utf-8'>SSID 不能为空，<a href='/'>返回</a>");
            return;
        }
        prefs.begin("clock4", false);
        prefs.putString("ssid", ssid);
        prefs.putString("pass", pass);
        prefs.end();
        portalSaved = true;
        apWeb.send(200, "text/html; charset=utf-8",
            "<meta charset='utf-8'>已保存，正在连接 WiFi。<a href='/'>返回</a>");
    });
    apWeb.onNotFound([]() {
        apWeb.sendHeader("Location", "/");
        apWeb.send(302, "text/plain", "");
    });
    apWeb.begin();
    configuring = true;
    Serial.println("[WIFI] 热点 BigClock-Setup：手机连接后打开 192.168.4.1");
}

void wifiLoadAndConnect() {
    prefs.begin("clock4", true);
    String ssid = prefs.getString("ssid", "");
    String pass = prefs.getString("pass", "");
    prefs.end();

    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    if (ssid.length()) {
        WiFi.begin(ssid.c_str(), pass.c_str());
        Serial.printf("[WIFI] 连接 %s\n", ssid.c_str());
    }

    sntp_set_time_sync_notification_cb(onSntpSync);
    configTime(TZ_OFFSET_SEC, 0, NTP_SERVER_1, NTP_SERVER_2);
    sntp_set_sync_mode(SNTP_SYNC_MODE_IMMED);
    sntp_set_sync_interval(SYNC_INTERVAL_MS);

    if (!ssid.length()) portalStart();
}

void serviceWifi() {
    if (configuring) {
        apWeb.handleClient();
        if (portalSaved) portalStopAndConnect();
        return;
    }
    if (WiFi.status() == WL_CONNECTED) return;
    if (millis() - lastWifiTry < 10000) return;
    lastWifiTry = millis();
    prefs.begin("clock4", true);
    String ssid = prefs.getString("ssid", "");
    String pass = prefs.getString("pass", "");
    prefs.end();
    if (ssid.length()) WiFi.begin(ssid.c_str(), pass.c_str());
    else if (!portalTried) portalStart();
}

// ------------------------------------------------------------------
// 状态 / 串口
// ------------------------------------------------------------------
void printStatus() {
    time_t t = (time_t)nowUtc();
    struct tm lt;
    localtime_r(&t, &lt);
    Serial.printf("时间 %02d:%02d:%02d (%s) target=%lld prepped=%d pending=%X 波形=%s\n",
                  lt.tm_hour, lt.tm_min, lt.tm_sec, timeValid ? "有效" : "未同步",
                  (long long)targetUtc, prepped ? 1 : 0, pendingMask,
                  qualityMode ? "TR7" : "TR4");
    Serial.printf("显示 ");
    for (uint8_t i = 0; i < CONNECTED_PANELS; i++) {
        Serial.printf("%c", shown[i] > 9 ? ' ' : ('0' + shown[i]));
    }
    Serial.printf("  WiFi=%s\n", WiFi.status() == WL_CONNECTED ? "已连" : "未连");
    for (uint8_t i = 0; i < CONNECTED_PANELS; i++) {
        Serial.printf("  %s CS=%d BUSY=%d level=%d begin=%d 供电=%d\n",
                      panName[i], panelCs[i], panelBusy[i],
                      digitalRead(panelBusy[i]), beginOk[i] ? 1 : 0,
                      pans[i]->isPowered() ? 1 : 0);
    }
}

void printHelp() {
    Serial.println(F(
        "\n=== Clock4 EPD v2 ===\n"
        "  T#### 设时间   W|SSID|密码 存 WiFi\n"
        "  c 深清   f 快刷(TR4)/质量刷(TR7)   q 质量刷\n"
        "  d 状态   b BUSY 诊断   h 帮助\n"
        "调度：:55 预写变化片 -> :00 齐刷 -> +2s POF\n"));
}

void handleLine() {
    if (s_len >= 3 && s_line[0] == 'W' && s_line[1] == '|') {
        char* sep = strchr(s_line + 2, '|');
        if (!sep) { Serial.println("[WIFI] 格式：W|SSID|密码"); return; }
        *sep = 0;
        String ssid = String(s_line + 2);
        String pass = String(sep + 1);
        if (!ssid.length()) { Serial.println("[WIFI] SSID 不能为空"); return; }
        prefs.begin("clock4", false);
        prefs.putString("ssid", ssid);
        prefs.putString("pass", pass);
        prefs.end();
        WiFi.begin(ssid.c_str(), pass.c_str());
        Serial.printf("[WIFI] 已保存并连接 \"%s\"\n", ssid.c_str());
        return;
    }
    if (s_len == 5 && (s_line[0] == 'T' || s_line[0] == 't')) {
        uint8_t hh = 0, mm = 0, ok = 1;
        for (uint8_t k = 0; k < 4; k++) {
            char c = s_line[1 + k];
            if (c < '0' || c > '9') { ok = 0; break; }
            if (k < 2) hh = hh * 10 + (c - '0');
            else       mm = mm * 10 + (c - '0');
        }
        if (ok && hh < 24 && mm < 60) { setManualTime(hh, mm); return; }
        Serial.println("[TIME] 格式：T####");
        return;
    }
    if (s_len == 1) {
        switch (s_line[0]) {
            case 'c': cleanAllPanels(); showNow(); break;
            case 'w': portalStart(); break;
            case 'f':
                qualityMode = !qualityMode;
                applyGear();
                Serial.printf("[MODE] %s\n", qualityMode ? "TR7" : "TR4");
                break;
            case 'q':
                qualityMode = true;
                applyGear();
                Serial.println("[MODE] TR7");
                break;
            case 'b': {
                for (uint8_t i = 0; i < CONNECTED_PANELS; i++) {
                    Serial.printf("%s CS=%d BUSY=%d idle_level=%d begin=%d\n",
                                  panName[i], panelCs[i], panelBusy[i],
                                  digitalRead(panelBusy[i]), beginOk[i] ? 1 : 0);
                }
                break;
            }
            case 'd': printStatus(); break;
            case 'h': printHelp(); break;
        }
    }
}

void serviceSerial() {
    while (Serial.available()) {
        char c = (char)Serial.read();
        if (c == '\r' || c == '\n') {
            s_line[s_len] = 0;
            if (s_len) handleLine();
            s_len = 0;
            continue;
        }
        if (s_len < 94) s_line[s_len++] = c;
        else s_len = 0;
    }
}

// ------------------------------------------------------------------
// setup / loop
// ------------------------------------------------------------------
void setup() {
    Serial.begin(115200);
    delay(300);
    Serial.println("\nClock4 EPD v2");

    setenv("TZ", "CST-8", 1);
    tzset();

    for (uint8_t i = 0; i < CONNECTED_PANELS; i++) {
        pans[i]->setBuffer(sharedFrame);       // 四片共用
        pans[i]->setRotation(i < 2 ? 0 : 1);   // 分十/分个 = 旧 ROTATION_270
        pans[i]->setPowerOffAfterRefresh(false);
        beginOk[i] = pans[i]->begin(PIN_SCK, PIN_MISO, PIN_SDA, 4000000,
                                    i == 0 && PIN_RST >= 0);
        if (!beginOk[i]) Serial.printf("[FAIL] %s begin\n", panName[i]);
    }
    applyGear();

    wifiLoadAndConnect();
    clearAll();                                // 未校时前保持空白
    printHelp();
}

void loop() {
    servicePof();
    serviceSerial();
    serviceWifi();

    if (syncFlag) {
        syncFlag = false;
        reAnchor();
    }

    if (!timeValid) {
        // 回调可能早于注册：开机阶段单次轮询，不是逐秒比对
        static uint32_t lastCheck = 0;
        if (millis() - lastCheck > 1000) {
            lastCheck = millis();
            if (time(nullptr) > 1700000000) reAnchor();
        }
        delay(2);
        return;
    }

    int64_t now = nowUtc();
    if (!targetUtc) targetUtc = (now / 60 + 1) * 60;

    // 只在边界之前预写；已经错过边界就交给 showNow()，避免先写旧数字
    if (!prepped && now >= targetUtc - PREP_LEAD_SEC && now < targetUtc) {
        prep(targetUtc);
        prepped = true;
    }

    if (now >= targetUtc) {
        if (!prepped || now - targetUtc > LATE_TOLERANCE_SEC) {
            showNow();                         // 错过边界：只补一次
        } else {
            fire();
        }
        prepped = false;
        targetUtc = (now / 60 + 1) * 60;
    }

    delay(2);
}




