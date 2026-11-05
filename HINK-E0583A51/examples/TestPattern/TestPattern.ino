/*
 * TestPattern.ino - UC8179 (HINK-E0583A51-A0) comprehensive bench test
 *
 * - Prints REV / FLG diagnostics to serial
 * - Runs the black->white maintenance clear (guide §14.5)
 * - Alternates a detailed test pattern and its inverse every 15 s
 * - Demonstrates DRF replay of the same SRAM frame (guide §14.7)
 *
 * Wiring (guide §3.1): SCK=6, SDA=7, CS=10, DC=3, RST=4, BUSY=5, BS=GND
 */

#include <UC8179.h>

UC8179 epd;
bool dark = false;
uint32_t lastFlip = 0;

static void printDiag() {
    uint8_t rev[7];
    if (epd.readREV(rev)) {
        Serial.print("REV:");
        for (uint8_t i = 0; i < 7; i++) { Serial.print(' '); Serial.print(rev[i], HEX); }
        Serial.println();   // last byte expected 0x0C = chip revision (guide §14.1)
    } else {
        Serial.println("REV read failed (writes still work)");
    }
    uint8_t flg;
    if (epd.readFLG(flg)) {
        Serial.print("FLG: 0x"); Serial.println(flg, HEX);
        Serial.print("  PON=");  Serial.print((flg & UC8179::FLG_PON)  ? 1 : 0);
        Serial.print(" POF=");   Serial.print((flg & UC8179::FLG_POF)  ? 1 : 0);
        Serial.print(" BUSY=");  Serial.print((flg & UC8179::FLG_BUSY) ? 1 : 0);
        Serial.print(" DATA=");  Serial.println((flg & UC8179::FLG_DATA) ? 1 : 0);
    }
}

static void drawPattern(bool darkMode) {
    const uint8_t fg = darkMode ? UC8179::INK_WHITE : UC8179::INK_BLACK;
    epd.fillScreen(darkMode ? UC8179::INK_BLACK : UC8179::INK_WHITE);

    // double frame
    epd.drawRect(0, 0, UC8179::WIDTH, UC8179::HEIGHT, fg);
    epd.drawRect(2, 2, UC8179::WIDTH - 4, UC8179::HEIGHT - 4, fg);

    // title
    epd.setTextColor(fg);
    epd.setTextSize(3);
    epd.setCursor(24, 20);
    epd.print(darkMode ? "UC8179 INVERSE" : "UC8179 TEST");
    epd.setTextSize(2);
    epd.setCursor(24, 58);
    epd.print("648x480 KW  4-wire SPI");

    // vertical stripes (left)
    for (int16_t x = 24; x < 300; x += 8) epd.drawFastVLine(x, 100, 130, fg);

    // checkerboard (right)
    for (int16_t y = 100; y < 230; y += 16)
        for (int16_t x = 320; x < 624; x += 16)
            if ((((x / 16) + (y / 16)) & 1) == 0) epd.fillRect(x, y, 16, 16, fg);

    // circles
    epd.drawCircle(120, 320, 70, fg);
    epd.fillCircle(260, 320, 70, fg);
    epd.drawCircle(400, 320, 45, fg);

    // diagonals
    epd.drawLine(480, 260, 620, 380, fg);
    epd.drawLine(620, 260, 480, 380, fg);

    // 50% dither block
    for (int16_t y = 400; y < 460; y++)
        for (int16_t x = 24; x < 300; x++)
            epd.drawPixel(x, y, ((x ^ y) & 2) ? fg : (uint8_t)(fg ^ 1));

    // running seconds
    epd.setTextSize(2);
    epd.setCursor(320, 410);
    epd.print("uptime s = ");
    epd.print(millis() / 1000);
}

void setup() {
    Serial.begin(115200);
    delay(500);
    epd.setDebug(&Serial);

    if (!epd.begin()) {
        Serial.print("epd.begin() failed: ");
        Serial.println(epd.errorText());
        while (true) delay(1000);
    }

    Serial.println("--- panel diagnostics ---");
    printDiag();

    Serial.println("--- maintenance clear (black -> white) ---");
    epd.maintenanceClear();

    drawPattern(dark);
    epd.displayQuality();        // S1 + TR7 quality refresh + POF
    lastFlip = millis();
}

void loop() {
    if (millis() - lastFlip < 15000) return;
    lastFlip = millis();

    // Every other cycle: replay the current SRAM frame with DRF instead of a
    // full re-upload — measured to work repeatedly on this panel (§14.7).
    static uint8_t cycle = 0;
    if ((cycle & 3) == 3) {
        Serial.println("--- DRF replay of same frame ---");
        if (!epd.refreshByDRF(/*powerOffAfter=*/true)) {
            Serial.print("refreshByDRF failed: ");
            Serial.println(epd.errorText());
        }
    } else {
        dark = !dark;
        drawPattern(dark);
        if (!epd.display()) {
            Serial.print("display() failed: ");
            Serial.println(epd.errorText());
        }
    }
    cycle++;

    // --- partial window (EXPERIMENTAL — untested on this panel, guide §8.3) ---
    // {
    //     epd.begin(); // not needed if already begun; shown for reset recovery
    //     epd.powerOn();
    //     epd.setPartialWindow(100, 100, 200, 80);
    //     epd.partialIn();
    //     // partial DTM length is unknown per the guide — bench-test with
    //     // epd.partialRowBytes() / epd.partialFrameBytes() candidates:
    //     // epd.writeNewFrameRaw(buf, epd.partialFrameBytes());
    //     epd.refreshByDRF();
    //     epd.partialOut();
    // }
}
