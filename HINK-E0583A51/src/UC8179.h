/*
 * UC8179.h - Arduino driver for the HINK-E0583A51-A0 5.83" e-paper panel
 *
 * Controller : UC8179, 4-wire SPI (BS=GND), KW black/white mode
 * Resolution : 648 x 480, 1 bit/pixel, one full plane = 38880 bytes
 * Target MCU : ESP32-C3 (hardware SPI, 3-wire half-duplex, Mode 0, 4 MHz)
 *              other MCUs fall back to software bit-bang SPI
 *
 * Derived from "HINK-E0583A51-A0_UC8179_ESP32-C3_开发指南" v1.5.5. The following
 * rules from the guide are encoded in this library:
 *
 *   - OTP is trusted: PSR / CDI / BTST / PWS / TCON / TRES / GSST / LUT are never
 *     rewritten; OTP command-default override (0x000C=0xA5) loads them on reset (§16.9.1)
 *   - EVS (0x52) = 0x00 and VDCS (0x82) = 0x00 are applied after every hardware
 *     reset: EVS is not in the OTP override area and is cleared by deep sleep (§10.4/§16.9.1)
 *   - Production full update uses S1 single-plane DTM2 + DRF.
 *   - TR4 is temporary/fast and stays powered; TR7 is final/quality and may POF.
 *   - DSLP is NOT used by default (border artifacts measured, §10.4 / v1.5.2)
 *   - Pixel polarity 1 = black, 0 = white; D7 = first pixel of each byte; image Y is
 *     flipped in software (frame buffer here is stored already flipped) (§14.4)
 *   - S0 shadow-frame mode is retained only as a diagnostic/compatibility path;
 *     its 38880-byte shadow buffer is allocated lazily when S0 is requested.
 *   - BUSY_N: low = busy. Write commands sent while busy are silently ignored;
 *     only REV/FLG/VV reads are accepted during refresh (§4.2)
 *   - SPI: 8-bit MSB first, per-byte CS for commands, block CS for data bursts (§3.2/§16.9.3)
 *
 * Partial refresh has limited bench validation: S1+TR4 can repeatedly update
 * one fixed 320x160 window. Sequential updates of disjoint windows make older
 * windows fade, and S1+TR7 contaminates horizontal/vertical background areas.
 */

#ifndef UC8179_H
#define UC8179_H

#include <Arduino.h>

// ---------------------------------------------------------------------------
// Backend selection
// ---------------------------------------------------------------------------
// ESP32: ESP-IDF spi_master, SPI_DEVICE_3WIRE | SPI_DEVICE_HALFDUPLEX with
//        MOSI == MISO == SDA pin — the configuration proven in the guide's v6
//        firmware (§3.2/§3.4/§7.1). Mode 0, default 4 MHz.
// Others (or force): software bit-bang, SCK idle LOW, MSB first, sampled on
//        rising edge — the guide's early validated transport (§14.1).
#if defined(ARDUINO_ARCH_ESP32) && !defined(UC8179_FORCE_SW_SPI)
  #define UC8179_HW_SPI 1
#else
  #define UC8179_SW_SPI 1
#endif

// DC level assumed while reading status bytes back (REV / FLG / DSP return).
// The guide's SPI table leaves the read-phase DC level "依命令定义" (§3.2);
// DC=1 (data) follows common UC8xxx practice and the guide's successful reads.
// If reads come back as constant 0x00/0xFF, try setting this to 0.
#ifndef UC8179_READ_DC_LEVEL
  #define UC8179_READ_DC_LEVEL 1
#endif

// SPI host used on ESP32 (SPI2 = FSPI on ESP32-C3). Override before including.
#ifndef UC8179_SPI_HOST
  #define UC8179_SPI_HOST SPI2_HOST
#endif

class UC8179 : public Print {
public:
    // ---- geometry (OTP: 648 source x 480 gate, 1 bit/pixel, §5.4) ----
    static const uint16_t WIDTH       = 648;
    static const uint16_t HEIGHT      = 480;
    static const uint16_t ROW_BYTES   = 81;     // 648 / 8
    static const uint32_t FRAME_BYTES = 38880;  // 81 * 480 = one KW plane

    // ---- pixel colors: panel polarity is 1 = black, 0 = white (DDX=00, §5.3/§14.4) ----
    static const uint8_t INK_WHITE = 0;
    static const uint8_t INK_BLACK = 1;

    // ---- UC8179 commands (guide §6 table / appendix A / §16.8) ----
    enum Command : uint8_t {
        CMD_PSR    = 0x00, CMD_PWR   = 0x01, CMD_POF   = 0x02, CMD_PFS   = 0x03,
        CMD_PON    = 0x04, CMD_PMES  = 0x05, CMD_BTST  = 0x06, CMD_DSLP  = 0x07,
        CMD_DTM1   = 0x10, CMD_DSP   = 0x11, CMD_DRF   = 0x12, CMD_DTM2  = 0x13,
        CMD_AUTO   = 0x17,
        CMD_LUTC   = 0x20, CMD_LUTWW = 0x21, CMD_LUTKW = 0x22, CMD_LUTWK = 0x23,
        CMD_LUTKK  = 0x24, CMD_LUTBD = 0x25,
        CMD_LUTOPT = 0x2A, CMD_KWOPT = 0x2B, CMD_PLL   = 0x30,
        CMD_TSC    = 0x40, CMD_TSE   = 0x41, CMD_TSW   = 0x42, CMD_TSR   = 0x43,
        CMD_PBC    = 0x44, CMD_CDI   = 0x50, CMD_EVS   = 0x52, CMD_VDCS  = 0x82,
        CMD_TCON   = 0x60, CMD_TRES  = 0x61, CMD_GSST  = 0x65,
        CMD_REV    = 0x70, CMD_FLG   = 0x71,
        CMD_PTL    = 0x90, CMD_PTIN  = 0x91, CMD_PTOUT = 0x92,
        CMD_ROTP   = 0xA2,
        CMD_CCSET  = 0xE0, CMD_PWS   = 0xE3, CMD_LVSEL = 0xE4, CMD_TSSET = 0xE5,
        CMD_TSBDRY = 0xE7,
    };

    // ---- FLG (0x71) bits, decoded from the guide's measured values (§14.1–14.3):
    //      0x13 = reset, 0x15 = after PON, 0x1D = after DTM2 (Data_Flag set) ----
    enum StatusFlag : uint8_t {
        FLG_BUSY = 0x01,  // BUSY_N pin level: 1 = high = idle
        FLG_POF  = 0x02,  // power off state
        FLG_PON  = 0x04,  // power on state
        FLG_DATA = 0x08,  // Data_Flag: set after DTM1+DTM2, cleared after DSP refresh
    };

    enum Error : uint8_t {
        ERR_NONE = 0, ERR_NOT_INIT, ERR_NO_RAM, ERR_SPI, ERR_TIMEOUT,
        ERR_BUSY_STUCK, ERR_PARAM, ERR_NOT_POWERED, ERR_READ,
    };

    // ---- DTM strategy (guide §16.7). S0 = DTM1 MCU shadow frame + DTM2 new
    //      frame (OTP default, true differential); S1 = single plane, DTM2 only,
    //      CDI=0x12 0x07 (DDX=10 absolute addressing).
    //      NOTE 2026-11-05 bench test: external-school strategies (佳显 const-
    //      white OLD / 微雪 DTM1=DTM2 same-state) were tried here as S2/S3 and
    //      REJECTED — DTM strategy is strongly bound to the OTP LUT waveforms;
    //      both variants darkened the active area and border electrode on this
    //      panel. Do not re-add without a matching LUT (guide §16.3 wave layer). ----
    enum DtmStrategy : uint8_t { DTM_S0_SHADOW = 0, DTM_S1_SINGLE = 1 };

    // Diagnostic A/B controls. These only change the two disputed transport /
    // refresh details and leave OTP, polarity, DTM strategy and drawing intact.
    enum DiagnosticRefresh : uint8_t {
        DIAG_REFRESH_AUTO = 0, // normal behavior: S0=DSP, S1=DRF
        DIAG_REFRESH_DSP  = 1,
        DIAG_REFRESH_DRF  = 2,
    };

    // Recommended application-level waveform choices for this HINK OTP.
    // Keep the generic forceTempGear() API for bench work, but normal code
    // should use only these two characterized modes.
    enum WaveformMode : uint8_t {
        WAVEFORM_FAST_TR4 = 0,    // TSSET=0x1E, fast UI updates
        WAVEFORM_QUALITY_TR7 = 1, // TSSET=0x7F, best density / recovery
    };

    enum FastPowerPolicy : uint8_t {
        FAST_POF_AFTER_REFRESH = 0,
        FAST_KEEP_POWERED = 1,
    };

    // Default pins = the guide's fixed ESP32-C3 wiring (§3.1):
    // SCK=GPIO6, SDA=GPIO7, CS=GPIO10, DC=GPIO3, RST=GPIO4, BUSY=GPIO5
    explicit UC8179(int8_t pinCs = 10, int8_t pinDc = 3, int8_t pinRst = 4,
                    int8_t pinBusy = 5, int8_t pinSck = 6, int8_t pinSda = 7);
    ~UC8179();

    // ------------------------------------------------------------------
    // Lifecycle / power
    // ------------------------------------------------------------------

    /// Allocate the current frame, init SPI, reset, apply EVS/VDCS, PON, and
    /// select production S1 mode. Call again to recover from deepSleep().
    bool begin(uint32_t spiHz = 4000000);

    /// Power off (if on) and release buffers / SPI.
    void end();

    /// PON (0x04) and wait for BUSY_N high. No-op if already powered.
    bool powerOn();

    /// POF (0x02) and wait for BUSY_N high — the guide's "golden finish".
    bool powerOff();

    /// POF + DSLP(0x07, 0xA5). WARNING: the guide measured border artifacts
    /// after deep sleep (§10.4); prefer powerOff() for normal use.
    /// After deepSleep() only a hardware reset (begin()) recovers the IC.
    bool deepSleep();

    bool isPowered() const { return _powered; }
    bool isReady()   const { return _inited; }

    /// Wait until BUSY_N is high (idle). Returns false on timeout.
    bool waitBusy(uint32_t timeoutMs, const char* tag = nullptr);

    // ------------------------------------------------------------------
    // Standard full-frame update (recommended path, §14.3/§16.9.3)
    // ------------------------------------------------------------------

    /// Draw the frame buffer: PON (if needed) -> DTM1(OLD = shadow frame, or
    /// constant white when the shadow is untrusted) -> DTM2(NEW = frame
    /// buffer) -> refresh -> wait BUSY -> immediate POF (golden finish) ->
    /// shadow frame updated.
    /// Refresh entry: S0 uses DSP (integrity flag check); S1 uses DRF —
    /// Data_Flag needs both planes (§6.2), so DSP cannot refresh after
    /// DTM2-only (bench-confirmed 2026-11-05).
    /// Set powerOffAfter=false to keep power on for back-to-back updates —
    /// recommended for interactive sessions: the guide's v5 measured a
    /// power-down transient (white flashes dark) on every per-frame POF,
    /// which v6 avoided by running powered and POF-ing only when idle.
    bool display(bool powerOffAfter = true);

    /// Characterized two-level update policy for this HINK panel:
    /// - fast: S1 + TR4, always keeps panel powered; use at most twice
    /// - quality: S1 + TR7, clears accumulated fast-update ghosting
    bool displayFast();
    bool displayQuality(bool powerOffAfter = true);

    /// Managed policy: first two calls use displayFast(); the third call uses
    /// displayQuality() and resets the cycle. The quality call optionally POFs.
    bool displayManaged(bool powerOffAfterQuality = true);
    uint8_t fastRefreshCount() const { return _fastRefreshCount; }
    // CONFIGURATION POINT: change this in setup() to test keep-powered mode.
    void setFastPowerPolicy(FastPowerPolicy policy) { _fastPowerPolicy = policy; }
    FastPowerPolicy fastPowerPolicy() const { return _fastPowerPolicy; }

    /// Replay / re-refresh the data already in SRAM via DRF (0x12) without
    /// re-sending planes. Measured to work repeatedly on this panel (§14.7).
    /// Note: DRF after POF+PON relies on SRAM retention through POF, which the
    /// guide has not tested — keep power on between DRF replays if unsure.
    bool refreshByDRF(bool powerOffAfter = true);

    /// Send DSP (0x11) after you populated both planes yourself
    /// (writeOldFrame/writeNewFrame), wait for BUSY, optionally POF.
    /// dspFlag (optional) receives the byte read back after DSP (bit7 =
    /// Data_Flag, measured 0x80 when complete).
    bool refreshByDSP(bool powerOffAfter = true, uint8_t* dspFlag = nullptr);

    /// Recovery / maintenance refresh (guide §14.5, §16.9.3): unknown OLD ->
    /// full black -> full white, ends powered-off with a valid white shadow.
    /// Recommended once after begin() on first boot or after power loss.
    bool maintenanceClear(bool powerOffAfter = true);

    // ------------------------------------------------------------------
    // Raw plane writers (guide §7.2 interface)
    // ------------------------------------------------------------------

    /// Send one full plane as DTM1 (OLD). Image is top-to-bottom row order
    /// (like a normal bitmap); the Y flip to controller order is done here.
    bool writeOldFrame(const uint8_t* topToBottomImage);

    /// Send one full plane as DTM2 (NEW). Row order as above.
    bool writeNewFrame(const uint8_t* topToBottomImage);

    /// Send len bytes to DTM1 / DTM2 exactly as given (no flip, no padding).
    /// Intended for partial-window and byte-exact experiments.
    bool writeOldFrameRaw(const uint8_t* data, uint32_t len);
    bool writeNewFrameRaw(const uint8_t* data, uint32_t len);

    // ------------------------------------------------------------------
    // Shadow frame management (N2OCP=0: MCU must supply OLD every refresh)
    // ------------------------------------------------------------------

    void invalidateShadow();            // after power loss / state loss
    bool isShadowValid() const { return _shadowValid; }
    bool copyFrameToShadow();           // mark current buffer as "displayed"
    bool copyShadowToFrame();           // restore buffer from shadow
    uint8_t* frameBuffer()  { return _frame; }
    /// Returns nullptr until S0 is explicitly selected or shadow storage is used.
    uint8_t* shadowFrame()  { return _shadow; }

    /// Copy a full 38880-byte top-to-bottom image (e.g. PROGMEM photo array,
    /// guide §14.6 pipeline) into the frame buffer, applying the Y flip.
    void loadImage(const uint8_t* topToBottomImage);

    // ------------------------------------------------------------------
    // Strategy knobs (guide §16 layer-1 patches, all optional)
    // ------------------------------------------------------------------

    /// Select the DTM strategy used by display():
    /// S0 (default, OTP CDI=0x10): dual plane, DTM1=MCU shadow frame (true diff).
    /// S1: writes CDI=0x12 0x07 (DDX=10, absolute addressing), display() skips DTM1.
    /// Register change survives POF; reset/deep sleep returns to S0.
    bool setDtmStrategy(DtmStrategy s);
    /// Currently selected DTM strategy (DtmStrategy value).
    uint8_t dtmStrategy() const { return _dtmStrategy; }

    /// Force a temperature gear: CCSET=0x02 + TSSET=code. code is a
    /// temperature code 0x00..0x7F (>=0x80 rejected: TR8–TR11 are not burned
    /// into this OTP, §16.5). Applies at the next refresh. Example mapping
    /// from the OTP table: TR3=0x14, TR4=0x1E, TR5=0x50, TR7=0x7F (quality).
    bool forceTempGear(uint8_t temperatureCode);

    /// Back to OTP automatic gear selection (CCSET=0x00).
    bool autoTempGear();

    /// Select one of the two characterized HINK waveform modes.
    bool setWaveformMode(WaveformMode mode);

    /// Diagnostic only: false keeps the original continuous-CS DTM transfer;
    /// true sends the DTM command and frame data as two separate CS cycles,
    /// matching the working GxEPD2 transport.
    void setDiagnosticSplitPlaneCs(bool enabled) { _splitPlaneCs = enabled; }
    bool diagnosticSplitPlaneCs() const { return _splitPlaneCs; }

    /// Diagnostic only: force display()/maintenanceClear() to enter refresh
    /// through DSP or DRF. AUTO preserves the normal strategy-dependent path.
    void setDiagnosticRefresh(DiagnosticRefresh mode) { _diagnosticRefresh = mode; }
    uint8_t diagnosticRefresh() const { return _diagnosticRefresh; }

    /// Diagnostic only: choose whether begin() applies the EVS=0x00 and
    /// VDCS=0x00 post-reset patches. Defaults preserve normal behavior.
    void setDiagnosticInitPatches(bool writeEvs, bool writeVdcs) {
        _diagnosticWriteEvs = writeEvs;
        _diagnosticWriteVdcs = writeVdcs;
    }

    /// Diagnostic only: overwrite PSR after begin(), for OTP 0x17 vs Gx 0x1F
    /// A/B testing. Changing UD is expected to vertically mirror the picture.
    bool diagnosticWritePSR(uint8_t value);

    // ------------------------------------------------------------------
    // Partial window (LIMITED: S1+TR4, one repeated/combined window only)
    // ------------------------------------------------------------------

    /// Set the partial window from pixel coordinates. Horizontal bounds are
    /// snapped to 8-pixel source-channel banks (HRST floor, HRED ceil);
    /// vertical bounds are gate lines. ptScan: 0 = inside only (recommended),
    /// 1 = scan inside+outside (also worked with TR4 in the centered test).
    bool setPartialWindow(int16_t x, int16_t y, int16_t w, int16_t h,
                          uint8_t ptScan = 0);
    bool partialIn();                   // PTIN (0x91)
    bool partialOut();                  // PTOUT (0x92)
    /// Bank-aligned partial DTM size. Validated for the centered 320x160 test;
    /// other sizes and edge windows still need application-level verification.
    uint16_t partialRowBytes() const;
    uint32_t partialFrameBytes() const;

    // ------------------------------------------------------------------
    // Diagnostics (reads; REV/FLG accepted even while BUSY_N low, §4.2)
    // ------------------------------------------------------------------

    /// Read FLG (0x71). See StatusFlag bits. After PON expect 0x15.
    bool readFLG(uint8_t& flg);
    /// Read REV (0x70), 7 bytes; last byte = chip revision (0x0C on this IC).
    bool readREV(uint8_t out[7]);

    // ------------------------------------------------------------------
    // Frame-buffer drawing. Origin = top-left, (0,0)..(647,479).
    // The controller Y flip is handled internally (§14.4).
    // ------------------------------------------------------------------

    void fillScreen(uint8_t color = INK_WHITE);
    void clearScreen() { fillScreen(INK_WHITE); }
    void drawPixel(int16_t x, int16_t y, uint8_t color = INK_BLACK);
    uint8_t readPixel(int16_t x, int16_t y);
    void drawLine(int16_t x0, int16_t y0, int16_t x1, int16_t y1,
                  uint8_t color = INK_BLACK);
    void drawFastHLine(int16_t x, int16_t y, int16_t w, uint8_t color = INK_BLACK);
    void drawFastVLine(int16_t x, int16_t y, int16_t h, uint8_t color = INK_BLACK);
    void drawRect(int16_t x, int16_t y, int16_t w, int16_t h,
                  uint8_t color = INK_BLACK);
    void fillRect(int16_t x, int16_t y, int16_t w, int16_t h,
                  uint8_t color = INK_BLACK);
    void drawCircle(int16_t x0, int16_t y0, int16_t r, uint8_t color = INK_BLACK);
    void fillCircle(int16_t x0, int16_t y0, int16_t r, uint8_t color = INK_BLACK);
    /// Classic 1-bit bitmap: rows top-to-bottom, (w+7)/8 bytes/row, MSB=leftmost.
    void drawBitmap(int16_t x, int16_t y, const uint8_t* bitmap,
                    int16_t w, int16_t h, uint8_t color = INK_BLACK);

    // ---- text: built-in 5x7 ASCII font via the Print interface ----
    void setCursor(int16_t x, int16_t y);
    int16_t getCursorX() const { return _curX; }
    int16_t getCursorY() const { return _curY; }
    void setTextSize(uint8_t s);                  // integer scale, default 1
    void setTextColor(uint8_t fg, int8_t bg = -1); // bg<0 = transparent
    void drawChar(int16_t x, int16_t y, uint8_t c, uint8_t fg, int8_t bg,
                  uint8_t size);
    virtual size_t write(uint8_t c);
    using Print::write;

    // ------------------------------------------------------------------
    // Misc
    // ------------------------------------------------------------------

    void setDebug(Stream* dbg) { _dbg = dbg; }     // e.g. setDebug(&Serial)
    void setRefreshTimeout(uint32_t ms) { _refreshTimeout = ms; }
    uint8_t lastError() const { return _lastErr; }
    const char* errorText() const;

private:
    // low-level transport
    void csLow();  void csHigh();
    void dcLow();  void dcHigh();
    bool spiBegin(uint32_t hz);
    void spiEnd();
    bool spiWrite(const uint8_t* buf, uint32_t len);
    bool hwSpiWrite(const uint8_t* buf, uint32_t len);
    bool swSpiWrite(const uint8_t* buf, uint32_t len);
    bool spiReadBytes(uint8_t* out, uint32_t len);
    bool cmdByte(uint8_t c);
    bool writeByte(uint8_t d);
    bool dataBytes(const uint8_t* d, uint32_t len);
    bool readData(uint8_t* out, uint32_t len);
    bool bulkFromMem(const uint8_t* src, uint32_t len);
    bool bulkFill(uint8_t value, uint32_t len);
    bool bulkFromMemFlipped(const uint8_t* src, uint32_t len);

    // panel sequences
    void hwReset();
    bool sendPlane(uint8_t cmd, const uint8_t* controllerOrderedFrame);
    bool sendPlaneFill(uint8_t cmd, uint8_t fillValue);
    bool sendPlaneFlipped(uint8_t cmd, const uint8_t* topToBottomImage, uint32_t len);
    bool sendPlaneRaw(uint8_t cmd, const uint8_t* data, uint32_t len);
    bool sendDSP(uint8_t* flagOut);
    bool triggerRefresh(bool defaultDsp, const char* tag, uint8_t* dspFlag = nullptr);
    bool ensureShadow();
    const char* dtmStrategyName(uint8_t s) const;

    // drawing helpers
    void hlineBuf(uint32_t rowBase, int16_t x, int16_t w, uint8_t color);
    uint32_t pxOffset(int16_t x, int16_t y) const {
        return (uint32_t)(HEIGHT - 1 - y) * ROW_BYTES + (x >> 3);
    }

    // debug / errors
    bool err(uint8_t e);
    void warn(const char* msg);
    void log(const char* msg);
    void logVal(const char* msg, uint32_t v, int base = 10);

    // pins (guide §3.1 defaults)
    int8_t _pinCs, _pinDc, _pinRst, _pinBusy, _pinSck, _pinSda;

    // state
    bool     _inited       = false;
    bool     _powered      = false;
    bool     _shadowValid  = false;
    bool     _canRead      = false;
    bool     _spiReady     = false;
    uint8_t  _dtmStrategy  = DTM_S0_SHADOW;
    bool     _splitPlaneCs = false;
    uint8_t  _diagnosticRefresh = DIAG_REFRESH_AUTO;
    bool     _diagnosticWriteEvs = true;
    bool     _diagnosticWriteVdcs = true;
    uint8_t  _fastRefreshCount = 0;
    FastPowerPolicy _fastPowerPolicy = FAST_POF_AFTER_REFRESH;
    bool     _deepSleeping = false;
    uint8_t  _lastErr      = ERR_NONE;
    uint32_t _spiHz        = 4000000;
    uint32_t _refreshTimeout = 8000;   // >= TR7 quality gear (2.9 s) + margin
    uint8_t* _frame  = nullptr;
    uint8_t* _shadow = nullptr;
    uint8_t* _stage  = nullptr;
    uint32_t _stageSize = 0;
    Stream*  _dbg = nullptr;

    // text state
    int16_t _curX = 0, _curY = 0, _curX0 = 0;
    uint8_t _textSize = 1;
    uint8_t _textFg = INK_BLACK;
    int8_t  _textBg = -1;

    // partial window state (pixel-snapped bank values)
    uint16_t _ptHrst = 0, _ptHred = 0, _ptVrst = 0, _ptVred = 0;

#if defined(UC8179_HW_SPI)
    // spi_device_handle_t, stored as void* so sketches including this header
    // do not need the ESP-IDF spi_master headers (cast in UC8179.cpp)
    void* _spiHw = nullptr;
#endif
};

#endif // UC8179_H
