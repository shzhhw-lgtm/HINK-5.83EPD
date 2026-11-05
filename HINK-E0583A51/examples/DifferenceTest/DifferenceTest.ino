/*
 * DifferenceTest.ino - final acceptance tests for the characterized policy.
 *
 * Final policy:
 *   FAST    = S1 + TR4, no POF, at most two consecutive updates
 *   QUALITY = S1 + TR7, clears fast-update residue, POF allowed afterwards
 *
 * Serial monitor: 115200 baud
 *   1  test displayFast()
 *   2  test displayQuality()
 *   3  test displayManaged() for two complete 3-frame cycles
 *   4  first S1+TR7 refresh after a real power-cycle/reboot
 *   5  first S1+TR4 refresh after a real power-cycle/reboot
 *   6  simulate interruption after two fast updates (leaves power on)
 *   7  resume from test 6 with TR7 recovery and POF
 *   8  boundary/corner/final-byte quality test
 *   9  complex graphics and 1-bit dither quality test
 *   a  maintenanceClear() then normal S1+TR7 page
 *   b  deep sleep, hardware-reset recovery, first S1+TR7 page
 *   c  repeated REV/FLG read diagnostics
 *   d  EXPERIMENTAL partial-window test, then automatic full recovery
 *   P  partial control: S1+TR4, PT_SCAN=1
 *   Q  partial control: S1+TR7, PT_SCAN=0
 *   e  same complex frame as test 9, but S0+TR7 instead of S1+TR7
 *   f  S1+TR7 symmetric solid-density/location test
 *   g  S1+TR7 same lower solid circle, but without Bayer dither
 *   h  S1+TR7 complex frame with PSR=0x1F (whole image vertically mirrored)
 *   z  run all remaining SAFE tests: 8, 9, a, b, c
 *   w  complete partial-window suite (mapping, edges, direction, repeats)
 *   p  safe finish: TR7 refresh current frame, then POF
 *   x  emergency immediate POF without TR7 recovery
 *   i  status
 *   ?  help
 */

#include <UC8179.h>

UC8179 epd;
static bool pageB = false;

static void printStatus()
{
    Serial.print("ready="); Serial.print(epd.isReady() ? "yes" : "no");
    Serial.print(" powered="); Serial.print(epd.isPowered() ? "yes" : "no");
    Serial.print(" strategy=S"); Serial.print(epd.dtmStrategy());
    Serial.print(" fastCount="); Serial.println(epd.fastRefreshCount());
}

static void printHelp()
{
    Serial.println();
    Serial.println("=== UC8179 Final Acceptance Test ===");
    Serial.println("1  displayFast(): S1+TR4, keeps power on");
    Serial.println("2  displayQuality(): S1+TR7, then POF");
    Serial.println("3  displayManaged(): 2x fast + 1x quality, two cycles");
    Serial.println("4  FIRST S1+TR7 after REAL power-cycle (run first after reboot)");
    Serial.println("5  FIRST S1+TR4 after REAL power-cycle (run first after reboot)");
    Serial.println("6  interrupt simulation: quality setup + 2x fast, then stop");
    Serial.println("7  finish test 6: TR7 recovery of current frame + POF");
    Serial.println("8  boundary/corners/final-byte test with S1+TR7");
    Serial.println("9  Bayer stress test (known to cause pattern-dependent gray)");
    Serial.println("a  maintenanceClear, then normal S1+TR7 page");
    Serial.println("b  deep sleep -> begin/reset -> first S1+TR7 page");
    Serial.println("c  repeat REV/FLG reads");
    Serial.println("d  partial candidate: S1+TR4, PT_SCAN=0 (recommended)");
    Serial.println("P  partial control:   S1+TR4, PT_SCAN=1");
    Serial.println("Q  partial control:   S1+TR7, PT_SCAN=0");
    Serial.println("e  complex frame with S0+TR7 (compare directly with test 9)");
    Serial.println("f  S1+TR7 symmetric top/bottom solid-density test");
    Serial.println("g  S1+TR7 lower solid circle WITHOUT Bayer dither");
    Serial.println("h  S1+TR7 complex frame with PSR=1F (vertical mirror)");
    Serial.println("z  run all remaining SAFE tests (8,9,a,b,c)");
    Serial.println("w  COMPLETE partial suite: current vs Y-corrected mapping + repeats");
    Serial.println("p  safe finish: TR7 recovery + POF");
    Serial.println("x  emergency immediate POF (no quality recovery)");
    Serial.println("i  status    ?  help");
}

static bool ensureBegin(bool restart = false)
{
    if (!restart && epd.isReady()) return true;
    if (!epd.begin()) {
        Serial.print("begin failed: ");
        Serial.println(epd.errorText());
        return false;
    }
    return true;
}

// Mixed pages only: no deliberate full-black/full-white clear frames.
static void drawPage(bool usePageB)
{
    epd.fillScreen(UC8179::INK_WHITE);
    epd.drawRect(2, 2, UC8179::WIDTH - 4, UC8179::HEIGHT - 4,
                 UC8179::INK_BLACK);

    // Unchanged reference target for same-state retention.
    epd.drawCircle(324, 240, 72, UC8179::INK_BLACK);
    epd.drawCircle(324, 240, 76, UC8179::INK_BLACK);
    epd.drawLine(248, 240, 400, 240, UC8179::INK_BLACK);
    epd.drawLine(324, 164, 324, 316, UC8179::INK_BLACK);

    if (!usePageB) {
        epd.fillRect(36, 52, 132, 92, UC8179::INK_BLACK);
        for (int16_t x = 40; x < 280; x += 16)
            epd.drawFastVLine(x, 190, 210, UC8179::INK_BLACK);
        for (int16_t y = 340; y < 430; y += 12)
            epd.drawFastHLine(360, y, 230, UC8179::INK_BLACK);
    } else {
        epd.fillRect(480, 52, 132, 92, UC8179::INK_BLACK);
        for (int16_t x = 368; x < 608; x += 16)
            epd.drawFastVLine(x, 190, 210, UC8179::INK_BLACK);
        for (int16_t y = 340; y < 430; y += 12)
            epd.drawFastHLine(58, y, 230, UC8179::INK_BLACK);
    }

    epd.setTextColor(UC8179::INK_BLACK);
    epd.setTextSize(4);
    epd.setCursor(usePageB ? 42 : 430, 18);
    epd.print(usePageB ? "PAGE B" : "PAGE A");
    epd.setTextSize(2);
    epd.setCursor(210, 438);
    epd.print("S1 TR4/TR7 FINAL");
}

static void drawNextPage()
{
    pageB = !pageB;
    drawPage(pageB);
    Serial.print("page="); Serial.println(pageB ? "B" : "A");
}

static void testFastApi()
{
    Serial.println("\n--- TEST 1: displayFast() ---");
    if (!ensureBegin()) return;
    drawNextPage();
    if (!epd.displayFast()) Serial.println(epd.errorText());
    printStatus(); // expected: powered=yes, S1, fastCount incremented
    Serial.println("Observe uniform faint residue; use p when finished.");
}

static void testQualityApi()
{
    Serial.println("\n--- TEST 2: displayQuality() ---");
    if (!ensureBegin()) return;
    drawNextPage();
    if (!epd.displayQuality()) Serial.println(epd.errorText());
    printStatus(); // expected: powered=no, S1, fastCount=0
}

static void testManagedApi()
{
    Serial.println("\n--- TEST 3: displayManaged(), 6 updates ---");
    if (!ensureBegin(true)) return;
    pageB = false;
    for (uint8_t i = 0; i < 6; i++) {
        drawNextPage();
        Serial.print("managed call "); Serial.print(i + 1); Serial.println("/6");
        if (!epd.displayManaged()) {
            Serial.println(epd.errorText());
            return;
        }
        printStatus();
        // Expected power/count: ON/1, ON/2, OFF/0, ON/1, ON/2, OFF/0.
        delay(1500);
    }
}

static void testFirstQualityAfterBoot()
{
    Serial.println("\n--- TEST 4: first S1+TR7 after power-cycle ---");
    Serial.println("This is valid only if it is the first test after real power removal/reboot.");
    if (!ensureBegin(true)) return;
    pageB = false;
    drawPage(pageB);
    if (!epd.displayQuality()) Serial.println(epd.errorText());
    printStatus();
}

static void testFirstFastAfterBoot()
{
    Serial.println("\n--- TEST 5: first S1+TR4 after power-cycle ---");
    Serial.println("This is valid only if it is the first test after real power removal/reboot.");
    if (!ensureBegin(true)) return;
    pageB = false;
    drawPage(pageB);
    if (!epd.displayFast()) Serial.println(epd.errorText());
    printStatus();
    Serial.println("Panel remains powered. Send 7 or p for TR7 recovery + POF.");
}

static void testInterruptedFastSequence()
{
    Serial.println("\n--- TEST 6: interrupt after two fast updates ---");
    if (!ensureBegin(true)) return;

    // Establish a known quality page but deliberately keep power on.
    pageB = false;
    drawPage(pageB);
    if (!epd.displayQuality(false)) return;

    for (uint8_t i = 0; i < 2; i++) {
        drawNextPage();
        if (!epd.displayFast()) {
            Serial.println(epd.errorText());
            return;
        }
    }
    Serial.println("SIMULATED STOP: no more drawing, no POF, panel remains powered.");
    Serial.println("Observe residue, then send 7 to safely finish the SAME frame.");
    printStatus();
}

static void finishInterruptedSequence()
{
    Serial.println("\n--- TEST 7: TR7 recovery of current frame + POF ---");
    if (!epd.isReady()) {
        Serial.println("Run test 6 first; RAM frame is not available after a real reboot.");
        return;
    }
    // Do not alter the frame: re-drive exactly the interrupted final image.
    if (!epd.displayQuality()) Serial.println(epd.errorText());
    printStatus();
}

static void safeFinish()
{
    Serial.println("safe finish: TR7 quality refresh current frame, then POF");
    if (!epd.isReady()) {
        Serial.println("not initialized; nothing to finish");
        return;
    }
    if (!epd.displayQuality()) Serial.println(epd.errorText());
    printStatus();
}

static void testBoundaries()
{
    Serial.println("\n--- TEST 8: boundaries and final byte ---");
    if (!ensureBegin(true)) return;
    epd.fillScreen(UC8179::INK_WHITE);

    // Exact outermost coordinates and byte boundaries across all 81 bytes.
    epd.drawFastHLine(0, 0, UC8179::WIDTH, UC8179::INK_BLACK);
    epd.drawFastHLine(0, UC8179::HEIGHT - 1, UC8179::WIDTH, UC8179::INK_BLACK);
    epd.drawFastVLine(0, 0, UC8179::HEIGHT, UC8179::INK_BLACK);
    epd.drawFastVLine(UC8179::WIDTH - 1, 0, UC8179::HEIGHT, UC8179::INK_BLACK);
    for (int16_t x = 0; x < (int16_t)UC8179::WIDTH; x += 8)
        epd.drawFastVLine(x, 20, 30, UC8179::INK_BLACK);

    // Distinct corner markers reveal clipping and orientation.
    epd.fillRect(1, 1, 48, 48, UC8179::INK_BLACK);
    epd.drawRect(UC8179::WIDTH - 50, 1, 48, 48, UC8179::INK_BLACK);
    epd.drawCircle(25, UC8179::HEIGHT - 26, 22, UC8179::INK_BLACK);
    epd.fillCircle(UC8179::WIDTH - 26, UC8179::HEIGHT - 26, 22,
                   UC8179::INK_BLACK);
    epd.setTextColor(UC8179::INK_BLACK);
    epd.setTextSize(3);
    epd.setCursor(150, 210);
    epd.print("BOUNDARY 648x480");

    if (!epd.displayQuality()) Serial.println(epd.errorText());
    Serial.println("Check all 4 outer lines, corner marks, and every 8px top tick.");
    delay(4000);
}

static const uint8_t BAYER4_FINAL[16] = {
    0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5
};

static void drawComplexFrame(const char* modeLabel)
{
    epd.fillScreen(UC8179::INK_WHITE);

    // Four deterministic 1-bit density blocks; no analog gray is sent.
    const uint8_t levels[4] = {3, 7, 11, 15};
    for (uint8_t block = 0; block < 4; block++) {
        int16_t x0 = 20 + block * 155;
        for (int16_t y = 80; y < 300; y++) {
            for (int16_t x = x0; x < x0 + 140; x++) {
                uint8_t threshold = BAYER4_FINAL[(y & 3) * 4 + (x & 3)];
                if (threshold < levels[block])
                    epd.drawPixel(x, y, UC8179::INK_BLACK);
            }
        }
        epd.drawRect(x0, 80, 140, 220, UC8179::INK_BLACK);
    }

    epd.drawCircle(110, 390, 64, UC8179::INK_BLACK);
    epd.fillCircle(270, 390, 64, UC8179::INK_BLACK);
    epd.drawLine(360, 326, 620, 454, UC8179::INK_BLACK);
    epd.drawLine(620, 326, 360, 454, UC8179::INK_BLACK);
    epd.setTextColor(UC8179::INK_BLACK);
    epd.setTextSize(3);
    epd.setCursor(110, 24);
    epd.print(modeLabel);
}

static void testComplexGraphics()
{
    Serial.println("\n--- TEST 9: complex graphics and dither, S1+TR7 ---");
    if (!ensureBegin(true)) return;
    drawComplexFrame("S1 + TR7 COMPLEX");

    // Keep power on after TR7 so refresh completion and POF can be observed
    // separately. Dither data is still strictly 1-bit black/white.
    if (!epd.displayQuality(false)) Serial.println(epd.errorText());
    Serial.println("TR7 COMPLETE, POWER STILL ON: observe for 5 seconds.");
    delay(5000);
    Serial.println("POF NOW: watch whether the brief dark flash occurs here.");
    epd.powerOff();
    Serial.println("Check density order, thin edges, circles, diagonals, and ghosting.");
    delay(3000);
}

static void testComplexS0Quality()
{
    Serial.println("\n--- TEST e: SAME complex frame, S0+TR7 ---");
    if (!ensureBegin(true)) return;
    epd.setDiagnosticRefresh(UC8179::DIAG_REFRESH_DRF);
    if (!epd.setDtmStrategy(UC8179::DTM_S0_SHADOW)) return;
    if (!epd.setWaveformMode(UC8179::WAVEFORM_QUALITY_TR7)) return;
    epd.invalidateShadow();
    drawComplexFrame("S0 + TR7 COMPLEX");

    // Use raw display() so the high-level quality API does not force S1.
    if (!epd.display(false)) Serial.println(epd.errorText());
    Serial.println("S0 TR7 COMPLETE, POWER STILL ON: observe filled lower circle for 5 seconds.");
    delay(5000);
    Serial.println("POF NOW");
    epd.powerOff();
    delay(3000);
}

static void testSolidLocationMap()
{
    Serial.println("\n--- TEST f: S1+TR7 solid density/location map ---");
    if (!ensureBegin(true)) return;
    epd.fillScreen(UC8179::INK_WHITE);

    // Identical solid shapes in upper and lower halves. If only lower shapes
    // fade, the issue is spatial/scan-related. If all solid shapes fade while
    // thin outlines stay dark, the issue is large-area S1 drive behavior.
    epd.fillRect(50, 50, 140, 120, UC8179::INK_BLACK);
    epd.fillRect(458, 50, 140, 120, UC8179::INK_BLACK);
    epd.fillRect(50, 310, 140, 120, UC8179::INK_BLACK);
    epd.fillRect(458, 310, 140, 120, UC8179::INK_BLACK);
    epd.fillCircle(324, 110, 62, UC8179::INK_BLACK);
    epd.fillCircle(324, 370, 62, UC8179::INK_BLACK);

    // Thin references should remain optically dense if only area coverage is
    // responsible for the apparent gray.
    epd.drawRect(12, 12, UC8179::WIDTH - 24, UC8179::HEIGHT - 24,
                 UC8179::INK_BLACK);
    epd.drawCircle(324, 240, 70, UC8179::INK_BLACK);
    epd.setTextColor(UC8179::INK_BLACK);
    epd.setTextSize(2);
    epd.setCursor(235, 226);
    epd.print("SOLID MAP");

    if (!epd.displayQuality(false)) Serial.println(epd.errorText());
    Serial.println("TR7 COMPLETE, POWER ON: compare TOP vs BOTTOM and SOLID vs OUTLINE for 8s.");
    delay(8000);
    Serial.println("POF NOW");
    epd.powerOff();
}

static void testComplexWithoutDither()
{
    Serial.println("\n--- TEST g: lower solid circle without Bayer dither ---");
    if (!ensureBegin(true)) return;
    epd.fillScreen(UC8179::INK_WHITE);

    // Same lower filled circle and crossing diagonals as the complex test,
    // while high-frequency dither blocks are replaced by low-frequency bars.
    for (uint8_t block = 0; block < 4; block++) {
        int16_t x0 = 20 + block * 155;
        epd.drawRect(x0, 80, 140, 220, UC8179::INK_BLACK);
        for (int16_t y = 96; y < 284; y += 24)
            epd.fillRect(x0 + 12, y, 116, 8, UC8179::INK_BLACK);
    }
    epd.drawCircle(110, 390, 64, UC8179::INK_BLACK);
    epd.fillCircle(270, 390, 64, UC8179::INK_BLACK);
    epd.drawLine(360, 326, 620, 454, UC8179::INK_BLACK);
    epd.drawLine(620, 326, 360, 454, UC8179::INK_BLACK);
    epd.setTextColor(UC8179::INK_BLACK);
    epd.setTextSize(3);
    epd.setCursor(88, 24);
    epd.print("NO DITHER + SOLID");

    if (!epd.displayQuality(false)) Serial.println(epd.errorText());
    Serial.println("TR7 COMPLETE, POWER ON: observe lower filled circle for 8s.");
    delay(8000);
    Serial.println("POF NOW");
    epd.powerOff();
}

static void testComplexMirroredScan()
{
    Serial.println("\n--- TEST h: complex frame with PSR=0x1F ---");
    Serial.println("The whole image is expected to be vertically mirrored.");
    if (!ensureBegin(true)) return;
    if (!epd.diagnosticWritePSR(0x1F)) return;
    drawComplexFrame("PSR1F MIRROR TEST");
    if (!epd.displayQuality(false)) Serial.println(epd.errorText());
    Serial.println("TR7 COMPLETE, POWER ON: observe whether gray follows the circle or physical lower half.");
    delay(8000);
    Serial.println("POF NOW");
    epd.powerOff();
}

static void testMaintenanceRecovery()
{
    Serial.println("\n--- TEST a: maintenanceClear recovery ---");
    if (!ensureBegin(true)) return;
    if (!epd.maintenanceClear()) {
        Serial.print("maintenanceClear failed: "); Serial.println(epd.errorText());
        return;
    }
    drawPage(false);
    if (!epd.displayQuality()) Serial.println(epd.errorText());
    Serial.println("Expected: clean white recovery, then clear PAGE A, powered off.");
    delay(4000);
}

static void testDeepSleepRecovery()
{
    Serial.println("\n--- TEST b: deep sleep and reset recovery ---");
    if (!ensureBegin(true)) return;
    drawPage(false);
    if (!epd.displayQuality(false)) return;
    Serial.println("enter deep sleep");
    if (!epd.deepSleep()) {
        Serial.print("deepSleep failed: "); Serial.println(epd.errorText());
        return;
    }
    delay(1000);
    Serial.println("recover through begin() hardware reset");
    if (!epd.begin()) {
        Serial.print("wake begin failed: "); Serial.println(epd.errorText());
        return;
    }
    drawPage(true);
    if (!epd.displayQuality()) Serial.println(epd.errorText());
    Serial.println("Expected: PAGE B clear after wake, then powered off.");
    delay(4000);
}

static void testStatusReads()
{
    Serial.println("\n--- TEST c: repeated REV/FLG reads ---");
    if (!ensureBegin(true)) return;
    for (uint8_t n = 0; n < 5; n++) {
        uint8_t rev[7] = {};
        uint8_t flg = 0;
        Serial.print("read "); Serial.print(n + 1); Serial.print(": REV=");
        if (epd.readREV(rev)) {
            for (uint8_t i = 0; i < 7; i++) {
                if (rev[i] < 0x10) Serial.print('0');
                Serial.print(rev[i], HEX);
                if (i != 6) Serial.print(' ');
            }
        } else Serial.print("FAIL");
        Serial.print(" FLG=");
        if (epd.readFLG(flg)) {
            if (flg < 0x10) Serial.print('0');
            Serial.println(flg, HEX);
        } else Serial.println("FAIL");
        delay(100);
    }
    epd.powerOff();
    Serial.println("Expected: all 5 reads identical; REV last byte 0C, FLG after PON 15.");
}

static void testPartialExperimental(UC8179::WaveformMode waveform,
                                    uint8_t ptScan, const char* label)
{
    Serial.println();
    Serial.print("--- PARTIAL TEST "); Serial.print(label); Serial.println(" ---");
    Serial.print("waveform=");
    Serial.print(waveform == UC8179::WAVEFORM_FAST_TR4 ? "TR4" : "TR7");
    Serial.print(" PT_SCAN="); Serial.println(ptScan);
    Serial.println("A 320x160 center window will be driven black for 8 seconds.");
    Serial.println("The test then hard-resets and performs a full S1+TR7 recovery.");
    if (!ensureBegin(true)) return;

    // Establish a known white full screen first.
    epd.fillScreen(UC8179::INK_WHITE);
    if (!epd.displayQuality(false)) return;
    if (!epd.setDtmStrategy(UC8179::DTM_S1_SINGLE)) return;
    if (!epd.setWaveformMode(waveform)) return;

    if (!epd.setPartialWindow(160, 160, 320, 160, ptScan)) return;
    uint32_t bytes = epd.partialFrameBytes();
    Serial.print("candidate partial bytes="); Serial.println(bytes);
    uint8_t* partial = (uint8_t*)malloc(bytes);
    if (!partial) {
        Serial.println("partial buffer allocation failed");
        return;
    }
    memset(partial, 0xFF, bytes); // S1 polarity: black

    bool ok = epd.partialIn() && epd.writeNewFrameRaw(partial, bytes) &&
              epd.refreshByDRF(false) && epd.partialOut();
    free(partial);
    Serial.println(ok ? "partial command flow completed" : "partial command flow failed");
    delay(8000);

    // Always recover known full-screen state after this experimental path.
    Serial.println("full reset + TR7 recovery");
    if (!epd.begin()) return;
    drawPage(false);
    if (!epd.displayQuality()) Serial.println(epd.errorText());
}

static void partialSetPixel(uint8_t* data, uint16_t rowBytes,
                            uint16_t width, uint16_t height,
                            int16_t x, int16_t y, bool reverseRows)
{
    if (x < 0 || y < 0 || x >= (int16_t)width || y >= (int16_t)height) return;
    uint16_t row = reverseRows ? (height - 1 - y) : y;
    data[(uint32_t)row * rowBytes + (x >> 3)] |= (uint8_t)(0x80 >> (x & 7));
}

static void partialFillRect(uint8_t* data, uint16_t rowBytes,
                            uint16_t width, uint16_t height,
                            int16_t x, int16_t y, int16_t w, int16_t h,
                            bool reverseRows)
{
    for (int16_t yy = y; yy < y + h; yy++)
        for (int16_t xx = x; xx < x + w; xx++)
            partialSetPixel(data, rowBytes, width, height, xx, yy, reverseRows);
}

static bool sendPartialPattern(int16_t screenX, int16_t screenY,
                               uint16_t width, uint16_t height,
                               uint8_t markerCorner, bool correctedY)
{
    // Current low-level API forwards Y directly to PTL. Under OTP PSR=0x17,
    // the candidate screen-coordinate correction is y'=HEIGHT-y-h and row
    // reversal. Both paths are tested in one run before changing the API.
    int16_t ptlY = correctedY ? (UC8179::HEIGHT - screenY - height) : screenY;
    if (!epd.setPartialWindow(screenX, ptlY, width, height, 0)) return false;

    uint16_t rowBytes = epd.partialRowBytes();
    uint32_t bytes = epd.partialFrameBytes();
    if (bytes != (uint32_t)rowBytes * height) {
        Serial.print("unexpected partial size: "); Serial.println(bytes);
        return false;
    }
    uint8_t* data = (uint8_t*)malloc(bytes);
    if (!data) return false;
    memset(data, 0x00, bytes); // S1: white

    // Border and asymmetric diagonal identify row order and mirroring.
    for (uint16_t x = 0; x < width; x++) {
        partialSetPixel(data, rowBytes, width, height, x, 0, correctedY);
        partialSetPixel(data, rowBytes, width, height, x, height - 1, correctedY);
    }
    for (uint16_t y = 0; y < height; y++) {
        partialSetPixel(data, rowBytes, width, height, 0, y, correctedY);
        partialSetPixel(data, rowBytes, width, height, width - 1, y, correctedY);
        int16_t dx = (int32_t)y * (width - 1) / (height - 1);
        partialSetPixel(data, rowBytes, width, height, dx, y, correctedY);
    }

    // A 28x28 solid marker must appear at the selected physical corner.
    int16_t mx = (markerCorner & 1) ? width - 34 : 6;
    int16_t my = (markerCorner & 2) ? height - 34 : 6;
    partialFillRect(data, rowBytes, width, height, mx, my, 28, 28, correctedY);

    // Low-frequency bars expose data length and clipping without Bayer stress.
    for (int16_t x = 16; x < (int16_t)width - 16; x += 24)
        partialFillRect(data, rowBytes, width, height, x, height / 2 - 4,
                        10, 8, correctedY);

    bool ok = epd.partialIn() && epd.writeNewFrameRaw(data, bytes) &&
              epd.refreshByDRF(false) && epd.partialOut();
    free(data);
    return ok;
}

static bool preparePartialWhite()
{
    if (!ensureBegin(true)) return false;
    epd.fillScreen(UC8179::INK_WHITE);
    if (!epd.displayQuality(false)) return false;
    if (!epd.setDtmStrategy(UC8179::DTM_S1_SINGLE)) return false;
    return epd.setWaveformMode(UC8179::WAVEFORM_FAST_TR4);
}

static bool sendFourCornerWindows(bool correctedY)
{
    struct Window { int16_t x, y; uint8_t corner; };
    static const Window windows[4] = {
        {0, 0, 0}, {488, 0, 1}, {0, 384, 2}, {488, 384, 3}
    };
    for (uint8_t i = 0; i < 4; i++) {
        Serial.print("corner window "); Serial.print(i + 1); Serial.println("/4");
        if (!sendPartialPattern(windows[i].x, windows[i].y, 160, 96,
                                windows[i].corner, correctedY)) return false;
        delay(500);
    }
    return true;
}

static void testPartialCompleteSuite()
{
    Serial.println("\n=== COMPLETE PARTIAL-WINDOW SUITE ===");
    Serial.println("Each corner window has a border, diagonal, bars, and a solid marker");
    Serial.println("that should touch the corresponding PHYSICAL screen corner.");

    // Phase 1: document current raw behavior.
    Serial.println("\nPHASE 1/3: CURRENT raw Y mapping (no correction)");
    if (!preparePartialWhite()) return;
    if (!sendFourCornerWindows(false)) {
        Serial.println("phase 1 failed");
        return;
    }
    Serial.println("Observe 8s: note corner-marker locations and diagonal direction.");
    delay(8000);

    // Phase 2: candidate normal top-left screen-coordinate mapping.
    Serial.println("\nPHASE 2/3: PSR=0x17 Y-corrected mapping + reversed rows");
    if (!preparePartialWhite()) return;
    if (!sendFourCornerWindows(true)) {
        Serial.println("phase 2 failed");
        return;
    }
    Serial.println("Observe 8s: markers should now match all four physical corners.");
    delay(8000);

    // Phase 3: repeat one mixed center window without reset between updates.
    Serial.println("\nPHASE 3/3: 3 repeated center partial updates (corrected mapping)");
    if (!preparePartialWhite()) return;
    for (uint8_t i = 0; i < 3; i++) {
        Serial.print("center update "); Serial.print(i + 1); Serial.println("/3");
        if (!sendPartialPattern(160, 160, 320, 160, i & 3, true)) {
            Serial.println("center update failed");
            return;
        }
        delay(1500);
    }
    Serial.println("Observe 8s for residue outside window and previous markers.");
    delay(8000);

    Serial.println("FINAL: hard reset + full S1+TR7 recovery + POF");
    if (!epd.begin()) return;
    drawPage(false);
    if (!epd.displayQuality()) Serial.println(epd.errorText());
    Serial.println("=== PARTIAL SUITE COMPLETE ===");
}

static void runRemainingSafeTests()
{
    Serial.println("\n=== RUN ALL REMAINING SAFE TESTS ===");
    testBoundaries();
    testComplexGraphics();
    testMaintenanceRecovery();
    testDeepSleepRecovery();
    testStatusReads();
    Serial.println("=== SAFE TEST BATCH COMPLETE ===");
    Serial.println("Partial-window test d is excluded because it is experimental.");
}

void setup()
{
    Serial.begin(115200);
    delay(800);
    epd.setDebug(&Serial);
    printHelp();
}

void loop()
{
    if (!Serial.available()) return;
    char c = Serial.read();
    switch (c) {
        case '1': testFastApi(); break;
        case '2': testQualityApi(); break;
        case '3': testManagedApi(); break;
        case '4': testFirstQualityAfterBoot(); break;
        case '5': testFirstFastAfterBoot(); break;
        case '6': testInterruptedFastSequence(); break;
        case '7': finishInterruptedSequence(); break;
        case '8': testBoundaries(); break;
        case '9': testComplexGraphics(); break;
        case 'a': testMaintenanceRecovery(); break;
        case 'b': testDeepSleepRecovery(); break;
        case 'c': testStatusReads(); break;
        case 'd': testPartialExperimental(UC8179::WAVEFORM_FAST_TR4, 0,
                                          "d: TR4 / inside-only"); break;
        case 'P': testPartialExperimental(UC8179::WAVEFORM_FAST_TR4, 1,
                                          "P: TR4 / inside+outside"); break;
        case 'Q': testPartialExperimental(UC8179::WAVEFORM_QUALITY_TR7, 0,
                                          "Q: TR7 / inside-only"); break;
        case 'e': testComplexS0Quality(); break;
        case 'f': testSolidLocationMap(); break;
        case 'g': testComplexWithoutDither(); break;
        case 'h': testComplexMirroredScan(); break;
        case 'z': runRemainingSafeTests(); break;
        case 'w': testPartialCompleteSuite(); break;
        case 'p': safeFinish(); break;
        case 'x':
            Serial.println("EMERGENCY immediate POF without TR7 recovery");
            if (epd.isReady()) epd.powerOff();
            printStatus();
            break;
        case 'i': printStatus(); break;
        case '?': printHelp(); break;
        default: break;
    }
}
