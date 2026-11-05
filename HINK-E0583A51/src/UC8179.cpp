/*
 * UC8179.cpp - implementation of the UC8179 Arduino driver
 * See UC8179.h for the manual-derived rules this implements.
 */

#include "UC8179.h"

#if defined(UC8179_HW_SPI)
  #include "driver/spi_master.h"
  #include "esp_heap_caps.h"
  // Compat with the historical IDF typo of SPI_DEVICE_HALFDUPLEX
  #if !defined(SPI_DEVICE_HALFDUPLEX) && defined(SPI_DEVICE_HAlFDUPLEX)
    #define SPI_DEVICE_HALFDUPLEX SPI_DEVICE_HAlFDUPLEX
  #endif
  #if !defined(SPI_DEVICE_HALFDUPLEX)
    #define SPI_DEVICE_HALFDUPLEX (1 << 0)
  #endif
  #if !defined(SPI_DEVICE_3WIRE)
    #define SPI_DEVICE_3WIRE (1 << 1)
  #endif
  #if !defined(SPI_DMA_CH_AUTO)
    #define SPI_DMA_CH_AUTO 3
  #endif
#endif

#include "uc8179_font5x7.h"

#if defined(__AVR__)
  #define UC8179_MEMCPY_P memcpy_P
#else
  #define UC8179_MEMCPY_P memcpy
#endif

// busy-wait timeouts (ms). Measured values in the guide: PON 177-189 ms,
// POF 34-42 ms, KW full refresh 552-805 ms (TR4/TR3) up to ~2.9 s (TR7).
static const uint32_t BUSY_TIMEOUT_RESET   = 1000;
static const uint32_t BUSY_TIMEOUT_PON     = 2000;
static const uint32_t BUSY_TIMEOUT_POF     = 1000;
static const uint32_t BUSY_TIMEOUT_PREOP   = 1000;

// ---------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------

UC8179::UC8179(int8_t pinCs, int8_t pinDc, int8_t pinRst,
               int8_t pinBusy, int8_t pinSck, int8_t pinSda)
    : _pinCs(pinCs), _pinDc(pinDc), _pinRst(pinRst),
      _pinBusy(pinBusy), _pinSck(pinSck), _pinSda(pinSda) {}

UC8179::~UC8179() { end(); }

// ---------------------------------------------------------------------
// Debug / errors
// ---------------------------------------------------------------------

void UC8179::log(const char* msg) {
    if (_dbg) { _dbg->print(F("[UC8179] ")); _dbg->println(msg); }
}

void UC8179::logVal(const char* msg, uint32_t v, int base) {
    if (_dbg) {
        _dbg->print(F("[UC8179] "));
        _dbg->print(msg);
        _dbg->println(v, base);
    }
}

void UC8179::warn(const char* msg) {
    if (_dbg) { _dbg->print(F("[UC8179][WARN] ")); _dbg->println(msg); }
}

bool UC8179::err(uint8_t e) {
    _lastErr = e;
    warn(errorText());
    return false;
}

const char* UC8179::errorText() const {
    switch (_lastErr) {
        case ERR_NONE:        return "no error";
        case ERR_NOT_INIT:    return "not initialized (call begin())";
        case ERR_NO_RAM:      return "out of RAM for frame buffer / optional S0 shadow";
        case ERR_SPI:         return "SPI transfer failed";
        case ERR_TIMEOUT:     return "BUSY_N timeout (IC still busy)";
        case ERR_BUSY_STUCK:  return "BUSY_N stuck low before operation";
        case ERR_PARAM:       return "invalid parameter";
        case ERR_NOT_POWERED: return "panel power is off";
        case ERR_READ:        return "SPI read not available / failed";
        default:              return "unknown error";
    }
}

// ---------------------------------------------------------------------
// GPIO helpers
// ---------------------------------------------------------------------

void UC8179::csLow()  { digitalWrite(_pinCs, LOW); }
void UC8179::csHigh() { digitalWrite(_pinCs, HIGH); }
void UC8179::dcLow()  { digitalWrite(_pinDc, LOW); }
void UC8179::dcHigh() { digitalWrite(_pinDc, HIGH); }

// ---------------------------------------------------------------------
// SPI transport
// ---------------------------------------------------------------------

bool UC8179::spiBegin(uint32_t hz) {
    _spiHz = hz;
#if defined(UC8179_HW_SPI)
    spi_bus_config_t bus = {};
    bus.mosi_io_num = _pinSda;
    bus.miso_io_num = _pinSda;      // 3-wire half duplex: MOSI/MISO share SDA
    bus.sclk_io_num = _pinSck;
    bus.quadwp_io_num = -1;
    bus.quadhd_io_num = -1;
    bus.max_transfer_sz = 4096;

    esp_err_t e = spi_bus_initialize(UC8179_SPI_HOST, &bus, SPI_DMA_CH_AUTO);
    if (e != ESP_OK) {
        // Retry write-only (MISO unbound). Display still works; reads disabled.
        bus.miso_io_num = -1;
        e = spi_bus_initialize(UC8179_SPI_HOST, &bus, SPI_DMA_CH_AUTO);
        if (e != ESP_OK) {
            logVal("spi_bus_initialize failed, err=", (uint32_t)e);
            return false;
        }
        _canRead = false;
        warn("bus init fell back to write-only; status reads disabled");
    } else {
        _canRead = true;
    }

    spi_device_interface_config_t dev = {};
    dev.clock_speed_hz = (int)hz;
    dev.mode = 0;                   // measured adaptation, guide §3.2/§12.2 C-04
    dev.spics_io_num = -1;          // CS handled as plain GPIO
    dev.queue_size = 1;
    dev.pre_cb = nullptr;
    dev.flags = SPI_DEVICE_3WIRE | SPI_DEVICE_HALFDUPLEX;
    e = spi_bus_add_device(UC8179_SPI_HOST, &dev,
                           (spi_device_handle_t*)&_spiHw);
    if (e != ESP_OK) {
        logVal("spi_bus_add_device failed, err=", (uint32_t)e);
        spi_bus_free(UC8179_SPI_HOST);
        return false;
    }
    _spiReady = true;
    return true;
#else
    pinMode(_pinSck, OUTPUT);
    pinMode(_pinSda, OUTPUT);
    digitalWrite(_pinSck, LOW);     // SCK idle LOW (guide §14.1)
    digitalWrite(_pinSda, LOW);
    _spiReady = true;
    _canRead = true;
    (void)hz;
    return true;
#endif
}

void UC8179::spiEnd() {
#if defined(UC8179_HW_SPI)
    if (_spiHw) {
        spi_bus_remove_device((spi_device_handle_t)_spiHw);
        _spiHw = nullptr;
        spi_bus_free(UC8179_SPI_HOST);
    }
#endif
    _spiReady = false;
    _canRead = false;
}

#if defined(UC8179_HW_SPI)
bool UC8179::hwSpiWrite(const uint8_t* buf, uint32_t len) {
    if (len == 0) return true;
    spi_transaction_t t = {};
    if (len <= 4) {
        t.flags = SPI_TRANS_USE_TXDATA;     // <=4 bytes go via registers (no DMA)
        memcpy(t.tx_data, buf, len);
    } else {
        t.tx_buffer = buf;                  // caller guarantees DMA-capable buffer
    }
    t.length = len * 8;
    return spi_device_polling_transmit((spi_device_handle_t)_spiHw, &t) == ESP_OK;
}
#endif

bool UC8179::swSpiWrite(const uint8_t* buf, uint32_t len) {
    for (uint32_t i = 0; i < len; i++) {
        uint8_t v = buf[i];
        for (int8_t b = 7; b >= 0; b--) {   // MSB first, rising-edge sample
            digitalWrite(_pinSda, (v >> b) & 0x01);
            digitalWrite(_pinSck, HIGH);
            digitalWrite(_pinSck, LOW);
        }
    }
    return true;
}

bool UC8179::spiWrite(const uint8_t* buf, uint32_t len) {
#if defined(UC8179_HW_SPI)
    return hwSpiWrite(buf, len);
#else
    return swSpiWrite(buf, len);
#endif
}

bool UC8179::spiReadBytes(uint8_t* out, uint32_t len) {
    if (len == 0 || len > 8) return false;
#if defined(UC8179_HW_SPI)
    // 8-byte aligned static buffer so the DMA path (len>4) is always happy
    static uint8_t rxbuf[8] __attribute__((aligned(4)));
    spi_transaction_t t = {};
    t.rxlength = ((len <= 4) ? len : 8) * 8;
    if (len <= 4) {
        t.flags = SPI_TRANS_USE_RXDATA;
    } else {
        t.rx_buffer = rxbuf;
    }
    if (spi_device_polling_transmit((spi_device_handle_t)_spiHw, &t) != ESP_OK) return false;
    if (len <= 4) memcpy(out, t.rx_data, len);
    else          memcpy(out, rxbuf, len);
    return true;
#else
    // bit-bang read: SCK idle LOW, sample on rising edge, MSB first
    pinMode(_pinSda, INPUT);
    for (uint32_t i = 0; i < len; i++) {
        uint8_t v = 0;
        for (int8_t b = 7; b >= 0; b--) {
            digitalWrite(_pinSck, HIGH);
            v |= (uint8_t)(digitalRead(_pinSda) & 0x01) << b;
            digitalWrite(_pinSck, LOW);
        }
        out[i] = v;
    }
    pinMode(_pinSda, OUTPUT);
    return true;
#endif
}

// ---------------------------------------------------------------------
// Command / data primitives
// ---------------------------------------------------------------------

bool UC8179::cmdByte(uint8_t c) {
    csLow();
    dcLow();
    bool ok = spiWrite(&c, 1);
    csHigh();
    return ok;
}

bool UC8179::writeByte(uint8_t d) {
    return dataBytes(&d, 1);
}

bool UC8179::dataBytes(const uint8_t* d, uint32_t len) {
    if (!_stage) return false;
    csLow();
    dcHigh();
    bool ok = true;
    while (len) {
        uint32_t n = (len < _stageSize) ? len : _stageSize;
        memcpy(_stage, d, n);
        if (!spiWrite(_stage, n)) { ok = false; break; }
        d += n;
        len -= n;
    }
    csHigh();
    return ok;
}

bool UC8179::readData(uint8_t* out, uint32_t len) {
    if (!_canRead) return false;
    csLow();
    digitalWrite(_pinDc, UC8179_READ_DC_LEVEL ? HIGH : LOW);
    bool ok = spiReadBytes(out, len);
    csHigh();
    return ok;
}

bool UC8179::bulkFromMem(const uint8_t* src, uint32_t len) {
    while (len) {
        uint32_t n = (len < _stageSize) ? len : _stageSize;
        UC8179_MEMCPY_P(_stage, src, n);
        if (!spiWrite(_stage, n)) return false;
        src += n;
        len -= n;
    }
    return true;
}

bool UC8179::bulkFill(uint8_t value, uint32_t len) {
    memset(_stage, value, _stageSize);
    while (len) {
        uint32_t n = (len < _stageSize) ? len : _stageSize;
        if (!spiWrite(_stage, n)) return false;
        len -= n;
    }
    return true;
}

// Row-reversed streaming: controller row r receives image row (rows-1-r).
// This implements the guide's software Y flip (§14.4) for raw image arrays.
bool UC8179::bulkFromMemFlipped(const uint8_t* src, uint32_t len) {
    if (len == 0 || (len % ROW_BYTES) != 0) return false;
    uint32_t rows = len / ROW_BYTES;
    uint32_t next = rows;
    uint32_t remaining = len;
    while (remaining) {
        uint32_t off = 0;
        while (off + ROW_BYTES <= _stageSize && next) {
            next--;
            UC8179_MEMCPY_P(_stage + off, src + (uint32_t)next * ROW_BYTES, ROW_BYTES);
            off += ROW_BYTES;
        }
        if (off == 0) return false;
        if (!spiWrite(_stage, off)) return false;
        remaining -= off;
    }
    return true;
}

// ---------------------------------------------------------------------
// BUSY_N management (guide §4.2)
// ---------------------------------------------------------------------

bool UC8179::waitBusy(uint32_t timeoutMs, const char* tag) {
    uint32_t t0 = millis();
    while (digitalRead(_pinBusy) == LOW) {
        if (millis() - t0 > timeoutMs) {
            if (tag && _dbg) {
                _dbg->print(F("[UC8179] wait "));
                _dbg->print(tag);
                _dbg->println(F(" TIMEOUT"));
            }
            _lastErr = ERR_TIMEOUT;
            return false;
        }
        delay(1);
    }
    if (tag && _dbg) {
        uint32_t dt = millis() - t0;
        _dbg->print(F("[UC8179] wait "));
        _dbg->print(tag);
        _dbg->print(F(" "));
        _dbg->print(dt);
        _dbg->println(F(" ms"));
    }
    return true;
}

// ---------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------

bool UC8179::begin(uint32_t spiHz) {
    bool wasDeepSleeping = _deepSleeping;
    end();

    pinMode(_pinRst, OUTPUT);
    digitalWrite(_pinRst, HIGH);
    pinMode(_pinCs, OUTPUT);
    csHigh();
    pinMode(_pinDc, OUTPUT);
    dcHigh();
    pinMode(_pinBusy, INPUT);

    _frame = (uint8_t*)malloc(FRAME_BYTES);
    if (!_frame) return err(ERR_NO_RAM);
    memset(_frame, 0x00, FRAME_BYTES);   // white

#if defined(UC8179_HW_SPI)
    _stageSize = 1024;
    _stage = (uint8_t*)heap_caps_malloc(_stageSize, MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    if (!_stage) _stage = (uint8_t*)malloc(_stageSize);
#else
    _stageSize = 64;
    _stage = (uint8_t*)malloc(_stageSize);
#endif
    if (!_stage) return err(ERR_NO_RAM);

    if (!spiBegin(spiHz)) return err(ERR_SPI);

    // Controlled power-down before reset when the IC may still be powered
    // from a soft reboot (guide §10.2: 带电复位前先 POF, avoids full-screen
    // color flash). Ignored silently if the IC is already off / sleeping.
    // In deep sleep BUSY_N is not a usable readiness indication and only a
    // hardware reset can wake the IC. Skip the otherwise useful pre-reset POF
    // probe to avoid a guaranteed timeout in this known state.
    if (!wasDeepSleeping && waitBusy(BUSY_TIMEOUT_PREOP, "pre-reset")) {
        cmdByte(CMD_POF);
        waitBusy(BUSY_TIMEOUT_POF, "pre-reset POF");
    }

    hwReset();
    _deepSleeping = false;

    // OTP command defaults are now loaded (PSR/CDI/BTST/TRES/GSST/... §16.8-A).
    // Apply the two guide-recommended patches (§16.9.1):
    //   EVS  = 0x00  border end level pinned to 0 V (anti-blackout, not in OTP
    //                override area, cleared by deep sleep -> re-apply each reset)
    //   VDCS = 0x00  explicit VCOM_DC base (optional but makes state explicit)
    if (_diagnosticWriteEvs) {
        cmdByte(CMD_EVS);
        writeByte(0x00);
    }
    if (_diagnosticWriteVdcs) {
        cmdByte(CMD_VDCS);
        writeByte(0x00);
    }

    _inited = true;
    _powered = false;
    _shadowValid = false;     // physical screen state unknown after reset
    _dtmStrategy = DTM_S0_SHADOW;  // hardware reset returns CDI to OTP S0
    _fastRefreshCount = 0;
    _lastErr = ERR_NONE;

    // Advisory diagnostics (non-fatal if reads are unavailable)
    uint8_t rev[7];
    if (readREV(rev)) {
        if (_dbg) {
            _dbg->print(F("[UC8179] REV:"));
            for (uint8_t i = 0; i < 7; i++) {
                _dbg->print(F(" "));
                _dbg->print(rev[i], HEX);
            }
            _dbg->println();
        }
    } else {
        warn("status reads unavailable (BUSY polling still works)");
    }

    if (!powerOn()) return false;
    // Final production default: S1 single-plane. This avoids DTM1 upload and
    // keeps the 38880-byte S0 shadow buffer unallocated.
    if (!setDtmStrategy(DTM_S1_SINGLE)) return false;

    uint8_t flg;
    if (readFLG(flg)) logVal("FLG after PON (expect 0x15): ", flg, HEX);

    return true;
}

void UC8179::end() {
    if (_powered && _inited) powerOff();
    if (_frame)  { free(_frame);  _frame = nullptr; }
    if (_shadow) { free(_shadow); _shadow = nullptr; }
    if (_stage)  { free(_stage);  _stage = nullptr; }
    if (_spiReady) spiEnd();
    _inited = false;
    _powered = false;
    _shadowValid = false;
}

void UC8179::hwReset() {
    // RST_N low >= 50 us, then wait > 1 ms before any command (guide §4.1).
    // 10 ms / 10 ms used for comfortable margin.
    digitalWrite(_pinRst, LOW);
    delay(10);
    digitalWrite(_pinRst, HIGH);
    delay(10);
    waitBusy(BUSY_TIMEOUT_RESET, "hw-reset");
}

bool UC8179::powerOn() {
    if (!_inited) return err(ERR_NOT_INIT);
    if (_powered) return true;
    if (!waitBusy(BUSY_TIMEOUT_PREOP)) return err(ERR_BUSY_STUCK);
    cmdByte(CMD_PON);
    if (!waitBusy(BUSY_TIMEOUT_PON, "PON")) return err(ERR_TIMEOUT);
    _powered = true;
    return true;
}

bool UC8179::powerOff() {
    if (!_inited) return err(ERR_NOT_INIT);
    if (!_powered) return true;
    if (!waitBusy(BUSY_TIMEOUT_PREOP)) return err(ERR_BUSY_STUCK);
    cmdByte(CMD_POF);
    if (!waitBusy(BUSY_TIMEOUT_POF, "POF")) return err(ERR_TIMEOUT);
    _powered = false;
    return true;
}

bool UC8179::deepSleep() {
    if (!_inited) return err(ERR_NOT_INIT);
    warn("deepSleep(): guide measured border artifacts after DSLP; "
         "powerOff() is the recommended finish (§10.4)");
    if (!powerOff()) return false;
    if (!waitBusy(BUSY_TIMEOUT_PREOP)) return err(ERR_BUSY_STUCK);
    cmdByte(CMD_DSLP);
    writeByte(0xA5);
    _powered = false;
    _deepSleeping = true;
    _shadowValid = false;   // registers cleared; SRAM state not trusted
    _dtmStrategy = DTM_S0_SHADOW;
    return true;
}

// ---------------------------------------------------------------------
// Plane transmission (block CS across cmd + data, guide §16.9.3)
// ---------------------------------------------------------------------

bool UC8179::sendPlane(uint8_t cmd, const uint8_t* controllerOrderedFrame) {
    if (!waitBusy(BUSY_TIMEOUT_PREOP)) return err(ERR_BUSY_STUCK);
    if (_splitPlaneCs) {
        if (!cmdByte(cmd)) return err(ERR_SPI);
        csLow();
        dcHigh();
        bool ok = bulkFromMem(controllerOrderedFrame, FRAME_BYTES);
        csHigh();
        return ok ? true : err(ERR_SPI);
    }
    csLow();
    dcLow();
    if (!spiWrite(&cmd, 1)) { csHigh(); return err(ERR_SPI); }
    dcHigh();
    bool ok = bulkFromMem(controllerOrderedFrame, FRAME_BYTES);
    csHigh();
    return ok ? true : err(ERR_SPI);
}

bool UC8179::sendPlaneFill(uint8_t cmd, uint8_t fillValue) {
    if (!waitBusy(BUSY_TIMEOUT_PREOP)) return err(ERR_BUSY_STUCK);
    if (_splitPlaneCs) {
        if (!cmdByte(cmd)) return err(ERR_SPI);
        csLow();
        dcHigh();
        bool ok = bulkFill(fillValue, FRAME_BYTES);
        csHigh();
        return ok ? true : err(ERR_SPI);
    }
    csLow();
    dcLow();
    if (!spiWrite(&cmd, 1)) { csHigh(); return err(ERR_SPI); }
    dcHigh();
    bool ok = bulkFill(fillValue, FRAME_BYTES);
    csHigh();
    return ok ? true : err(ERR_SPI);
}

bool UC8179::sendPlaneFlipped(uint8_t cmd, const uint8_t* topToBottomImage,
                              uint32_t len) {
    if (!waitBusy(BUSY_TIMEOUT_PREOP)) return err(ERR_BUSY_STUCK);
    if (_splitPlaneCs) {
        if (!cmdByte(cmd)) return err(ERR_SPI);
        csLow();
        dcHigh();
        bool ok = bulkFromMemFlipped(topToBottomImage, len);
        csHigh();
        return ok ? true : err(ERR_SPI);
    }
    csLow();
    dcLow();
    if (!spiWrite(&cmd, 1)) { csHigh(); return err(ERR_SPI); }
    dcHigh();
    bool ok = bulkFromMemFlipped(topToBottomImage, len);
    csHigh();
    return ok ? true : err(ERR_SPI);
}

bool UC8179::sendPlaneRaw(uint8_t cmd, const uint8_t* data, uint32_t len) {
    if (!waitBusy(BUSY_TIMEOUT_PREOP)) return err(ERR_BUSY_STUCK);
    if (_splitPlaneCs) {
        if (!cmdByte(cmd)) return err(ERR_SPI);
        csLow();
        dcHigh();
        bool ok = bulkFromMem(data, len);
        csHigh();
        return ok ? true : err(ERR_SPI);
    }
    csLow();
    dcLow();
    if (!spiWrite(&cmd, 1)) { csHigh(); return err(ERR_SPI); }
    dcHigh();
    bool ok = bulkFromMem(data, len);
    csHigh();
    return ok ? true : err(ERR_SPI);
}

bool UC8179::sendDSP(uint8_t* flagOut) {
    if (!waitBusy(BUSY_TIMEOUT_PREOP)) return err(ERR_BUSY_STUCK);
    cmdByte(CMD_DSP);
    // Read back the completeness flag right after the command (guide §14.3:
    // measured 0x80 = Data_Flag set). Advisory only — never fails the flow.
    if (flagOut) {
        *flagOut = 0;
        if (_canRead) {
            uint8_t v = 0;
            if (readData(&v, 1)) *flagOut = v;
        }
    }
    return true;
}

bool UC8179::triggerRefresh(bool defaultDsp, const char* tag, uint8_t* dspFlag) {
    bool useDsp = defaultDsp;
    if (_diagnosticRefresh == DIAG_REFRESH_DSP) useDsp = true;
    if (_diagnosticRefresh == DIAG_REFRESH_DRF) useDsp = false;

    if (useDsp) {
        if (!sendDSP(dspFlag)) return false;
    } else {
        if (!waitBusy(BUSY_TIMEOUT_PREOP)) return err(ERR_BUSY_STUCK);
        if (!cmdByte(CMD_DRF)) return err(ERR_SPI);
    }
    // BUSY may assert shortly after the refresh command. GxEPD2 waits 1 ms
    // before polling; without this margin an immediate HIGH can be mistaken
    // for refresh completion and POF may be sent too early.
    delay(1);
    return waitBusy(_refreshTimeout, tag) ? true : err(ERR_TIMEOUT);
}

// ---------------------------------------------------------------------
// Raw plane writers (guide §7.2)
// ---------------------------------------------------------------------

bool UC8179::writeOldFrame(const uint8_t* topToBottomImage) {
    if (!_inited) return err(ERR_NOT_INIT);
    return sendPlaneFlipped(CMD_DTM1, topToBottomImage, FRAME_BYTES);
}

bool UC8179::writeNewFrame(const uint8_t* topToBottomImage) {
    if (!_inited) return err(ERR_NOT_INIT);
    return sendPlaneFlipped(CMD_DTM2, topToBottomImage, FRAME_BYTES);
}

bool UC8179::writeOldFrameRaw(const uint8_t* data, uint32_t len) {
    if (!_inited) return err(ERR_NOT_INIT);
    return sendPlaneRaw(CMD_DTM1, data, len);
}

bool UC8179::writeNewFrameRaw(const uint8_t* data, uint32_t len) {
    if (!_inited) return err(ERR_NOT_INIT);
    return sendPlaneRaw(CMD_DTM2, data, len);
}

// ---------------------------------------------------------------------
// Shadow frame (S0 strategy, guide §5.3/§14.9/§16.7)
// ---------------------------------------------------------------------

void UC8179::invalidateShadow() {
    _shadowValid = false;
}

bool UC8179::ensureShadow() {
    if (_shadow) return true;
    _shadow = (uint8_t*)malloc(FRAME_BYTES);
    if (!_shadow) return err(ERR_NO_RAM);
    memset(_shadow, 0x00, FRAME_BYTES);
    _shadowValid = false;
    return true;
}

bool UC8179::copyFrameToShadow() {
    if (!_frame || !ensureShadow()) return false;
    if (_dtmStrategy == DTM_S0_SHADOW) {
        if (!ensureShadow()) return false;
        memcpy(_shadow, _frame, FRAME_BYTES);
        _shadowValid = true;
    }
    return true;
}

bool UC8179::copyShadowToFrame() {
    if (!_shadowValid) return false;
    memcpy(_frame, _shadow, FRAME_BYTES);
    return true;
}

void UC8179::loadImage(const uint8_t* topToBottomImage) {
    if (!_frame) return;
    for (uint16_t c = 0; c < HEIGHT; c++) {
        UC8179_MEMCPY_P(_frame + (uint32_t)c * ROW_BYTES,
                        topToBottomImage + (uint32_t)(HEIGHT - 1 - c) * ROW_BYTES,
                        ROW_BYTES);
    }
}

// ---------------------------------------------------------------------
// Full update / refresh
// ---------------------------------------------------------------------

bool UC8179::display(bool powerOffAfter) {
    if (!_inited) return err(ERR_NOT_INIT);
    uint32_t t0 = millis();

    if (!_powered && !powerOn()) return false;
    log(dtmStrategyName(_dtmStrategy));
    if (_dtmStrategy == DTM_S0_SHADOW && !_shadowValid)
        warn("shadow invalid — this refresh falls back to constant-white OLD");

    // DTM1 (guide §6.1: KW mode DTM1 = OLD plane).
    // S1: single plane — CDI=0x12 (DDX=10), DTM1 is ignored by the IC.
    // S0: DTM1 = real previous frame (shadow), or constant white if invalid.
    // (2026-11-05 bench test: external const-white / same-state strategies are
    //  strongly bound to their own OTP LUTs and darkened this panel — removed.)
    if (_dtmStrategy != DTM_S1_SINGLE) {
        bool ok = _shadowValid ? sendPlane(CMD_DTM1, _shadow)
                               : sendPlaneFill(CMD_DTM1, 0x00);
        if (!ok) return false;
    }
    if (!sendPlane(CMD_DTM2, _frame)) return false;

    uint8_t dspFlag = 0;
    bool defaultDsp = (_dtmStrategy != DTM_S1_SINGLE);
    const char* refreshTag = ((_diagnosticRefresh == DIAG_REFRESH_DRF) ||
                              (_diagnosticRefresh == DIAG_REFRESH_AUTO && !defaultDsp))
                             ? "refresh(DRF)" : "refresh(DSP)";
    if (!triggerRefresh(defaultDsp, refreshTag, &dspFlag)) return false;
    if ((_diagnosticRefresh != DIAG_REFRESH_DRF) && defaultDsp && _canRead && _dbg) {
        if (dspFlag & 0x80) logVal("DSP data flag OK: 0x", dspFlag, HEX);
        else warn("DSP data flag NOT set — refresh may not start");
    }

    if (powerOffAfter && !powerOff()) return false;

    if (_dtmStrategy == DTM_S0_SHADOW) {
        if (!ensureShadow()) return false;
        memcpy(_shadow, _frame, FRAME_BYTES);
        _shadowValid = true;
    }

    if (_dbg) {
        _dbg->print(F("[UC8179] display total "));
        _dbg->print(millis() - t0);
        _dbg->println(F(" ms"));
    }
    return true;
}

bool UC8179::displayFast() {
    if (!_inited) return err(ERR_NOT_INIT);
    // Bench result: S0+TR4 accumulates thin edge trails. S1+TR4 has only a
    // faint, uniform background residue, transfers one plane less, and is
    // fully recovered by the scheduled TR7 quality refresh.
    if (_dtmStrategy != DTM_S1_SINGLE && !setDtmStrategy(DTM_S1_SINGLE)) return false;
    if (!setWaveformMode(WAVEFORM_FAST_TR4)) return false;
    // Production default: POF after every TR4. Set FAST_KEEP_POWERED for
    // explicit animation/behavior testing.
    if (!display(_fastPowerPolicy == FAST_POF_AFTER_REFRESH)) return false;
    if (_fastRefreshCount < 0xFF) _fastRefreshCount++;
    return true;
}

bool UC8179::displayQuality(bool powerOffAfter) {
    if (!_inited) return err(ERR_NOT_INIT);
    // S0 and S1 have equivalent TR7 image quality; keep S1 to avoid the OLD
    // plane upload and to use one consistent production strategy.
    if (_dtmStrategy != DTM_S1_SINGLE && !setDtmStrategy(DTM_S1_SINGLE)) return false;
    if (!setWaveformMode(WAVEFORM_QUALITY_TR7)) return false;
    if (!display(powerOffAfter)) return false;
    _fastRefreshCount = 0;
    return true;
}

bool UC8179::displayManaged(bool powerOffAfterQuality) {
    if (_fastRefreshCount >= 2) return displayQuality(powerOffAfterQuality);
    return displayFast();
}

bool UC8179::refreshByDSP(bool powerOffAfter, uint8_t* dspFlag) {
    if (!_inited) return err(ERR_NOT_INIT);
    if (_dtmStrategy == DTM_S1_SINGLE)
        warn("refreshByDSP under S1: Data_Flag needs DTM1+DTM2 (§6.2); "
             "DSP will not refresh after DTM2-only — use refreshByDRF()");
    if (!_powered && !powerOn()) return false;
    if (dspFlag) *dspFlag = 0;
    if (!sendDSP(dspFlag)) return false;
    if (_canRead && dspFlag && _dbg) {
        if (*dspFlag & 0x80) logVal("DSP data flag OK: 0x", *dspFlag, HEX);
        else warn("DSP data flag NOT set");
    }
    if (!waitBusy(_refreshTimeout, "refresh(DSP)")) return err(ERR_TIMEOUT);
    if (powerOffAfter && !powerOff()) return false;
    return true;
}

bool UC8179::refreshByDRF(bool powerOffAfter) {
    if (!_inited) return err(ERR_NOT_INIT);
    if (!_powered && !powerOn()) return false;
    if (!waitBusy(BUSY_TIMEOUT_PREOP)) return err(ERR_BUSY_STUCK);
    cmdByte(CMD_DRF);
    if (!waitBusy(_refreshTimeout, "refresh(DRF)")) return err(ERR_TIMEOUT);
    if (powerOffAfter && !powerOff()) return false;
    return true;
}

bool UC8179::maintenanceClear(bool powerOffAfter) {
    if (!_inited) return err(ERR_NOT_INIT);
    uint32_t t0 = millis();
    log("maintenance: unknown OLD -> black");
    // Maintenance is intentionally a dual-plane recovery sequence. Production
    // begins in S1, so select S0 explicitly for these two DTM1/DTM2 passes.
    if (_dtmStrategy != DTM_S0_SHADOW && !setDtmStrategy(DTM_S0_SHADOW)) return false;
    if (!_powered && !powerOn()) return false;

    // Pass 1: OLD unknown -> assume white, drive everything to black (§14.5)
    if (!sendPlaneFill(CMD_DTM1, 0x00)) return false;  // OLD = white
    if (!sendPlaneFill(CMD_DTM2, 0xFF)) return false;  // NEW = black
    if (!triggerRefresh(true, "maint black")) return false;

    // Pass 2: OLD = black (now true) -> drive to white
    log("maintenance: black -> white");
    if (!sendPlaneFill(CMD_DTM1, 0xFF)) return false;
    if (!sendPlaneFill(CMD_DTM2, 0x00)) return false;
    if (!triggerRefresh(true, "maint white")) return false;

    // Return to production S1 while the controller is still powered.
    if (!setDtmStrategy(DTM_S1_SINGLE)) return false;
    if (powerOffAfter && !powerOff()) return false;

    if (_shadow) {
        memset(_shadow, 0x00, FRAME_BYTES); // screen is now white
        _shadowValid = true;
    }
    if (_dbg) {
        _dbg->print(F("[UC8179] maintenanceClear total "));
        _dbg->print(millis() - t0);
        _dbg->println(F(" ms"));
    }
    return true;
}

// ---------------------------------------------------------------------
// Strategy knobs (guide §16 layer-1)
// ---------------------------------------------------------------------

const char* UC8179::dtmStrategyName(uint8_t s) const {
    switch (s) {
        case DTM_S1_SINGLE: return "DTM strategy = S1 (single plane, DTM2 only)";
        default:            return "DTM strategy = S0 shadow frame (dual plane)";
    }
}

bool UC8179::setDtmStrategy(DtmStrategy s) {
    if (!_inited) return err(ERR_NOT_INIT);
    if (s == DTM_S0_SHADOW && !ensureShadow()) return false;
    if (!waitBusy(BUSY_TIMEOUT_PREOP)) return err(ERR_BUSY_STUCK);
    // CDI (0x50) two bytes; keep OTP byte2 interval value 0x07 (§16.8-C).
    // S1 = 0x12 (DDX=10 single plane); S0 = 0x10 (OTP DDX=00 dual plane).
    uint8_t cdi[2] = { (uint8_t)(s == DTM_S1_SINGLE ? 0x12 : 0x10), 0x07 };
    cmdByte(CMD_CDI);
    if (!dataBytes(cdi, 2)) return err(ERR_SPI);
    _dtmStrategy = (uint8_t)s;
    log(dtmStrategyName(_dtmStrategy));
    return true;
}

bool UC8179::forceTempGear(uint8_t temperatureCode) {
    if (!_inited) return err(ERR_NOT_INIT);
    if (temperatureCode > 0x7F) {
        // TR8-TR11 are not burned into this OTP (all 0xFF) — forbidden (§16.5)
        return err(ERR_PARAM);
    }
    if (!waitBusy(BUSY_TIMEOUT_PREOP)) return err(ERR_BUSY_STUCK);
    cmdByte(CMD_CCSET);
    if (!writeByte(0x02)) return err(ERR_SPI);   // TSFIX = 1
    cmdByte(CMD_TSSET);
    if (!writeByte(temperatureCode)) return err(ERR_SPI);
    logVal("temp gear forced, TSSET=0x", temperatureCode, HEX);
    return true;
}

bool UC8179::autoTempGear() {
    if (!_inited) return err(ERR_NOT_INIT);
    if (!waitBusy(BUSY_TIMEOUT_PREOP)) return err(ERR_BUSY_STUCK);
    cmdByte(CMD_CCSET);
    if (!writeByte(0x00)) return err(ERR_SPI);   // back to OTP auto selection
    log("temp gear = OTP automatic");
    return true;
}

bool UC8179::setWaveformMode(WaveformMode mode) {
    if (mode == WAVEFORM_FAST_TR4) {
        log("waveform mode = FAST TR4 (TSSET=0x1E)");
        return forceTempGear(0x1E);
    }
    if (mode == WAVEFORM_QUALITY_TR7) {
        log("waveform mode = QUALITY TR7 (TSSET=0x7F)");
        return forceTempGear(0x7F);
    }
    return err(ERR_PARAM);
}

bool UC8179::diagnosticWritePSR(uint8_t value) {
    if (!_inited) return err(ERR_NOT_INIT);
    if (!waitBusy(BUSY_TIMEOUT_PREOP)) return err(ERR_BUSY_STUCK);
    if (!cmdByte(CMD_PSR)) return err(ERR_SPI);
    if (!writeByte(value)) return err(ERR_SPI);
    logVal("diagnostic PSR=0x", value, HEX);
    return true;
}

// ---------------------------------------------------------------------
// Partial window (experimental, guide §8.2 — untested on this panel)
// ---------------------------------------------------------------------

bool UC8179::setPartialWindow(int16_t x, int16_t y, int16_t w, int16_t h,
                              uint8_t ptScan) {
    if (!_inited) return err(ERR_NOT_INIT);
    if (w <= 0 || h <= 0) return err(ERR_PARAM);
    int16_t x0 = x, y0 = y;
    int16_t x1 = x + w - 1, y1 = y + h - 1;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > (int16_t)WIDTH - 1)  x1 = (int16_t)WIDTH - 1;
    if (y1 > (int16_t)HEIGHT - 1) y1 = (int16_t)HEIGHT - 1;
    if (x0 > x1 || y0 > y1) return err(ERR_PARAM);

    // PTL uses full pixel coordinates, not byte-bank indices. X boundaries
    // must be byte aligned, but their high address bits must be preserved.
    // The previous bank/low-5-bit encoding lost bit 8 for X >= 256, causing
    // right-side windows to overlap the left half of the panel.
    uint16_t xStart = (uint16_t)x0 & 0xFFF8;
    uint16_t xEnd = (uint16_t)x1 | 0x0007;
    if (xEnd >= WIDTH) xEnd = WIDTH - 1;
    uint16_t hrst = xStart / 8;
    uint16_t hred = xEnd / 8;
    uint16_t vrst = (uint16_t)y0;
    uint16_t vred = (uint16_t)y1;
    if (vred <= vrst) vred = vrst + 1;          // guide: VRED must be > VRST

    if (!waitBusy(BUSY_TIMEOUT_PREOP)) return err(ERR_BUSY_STUCK);
    uint8_t p[9];
    p[0] = (uint8_t)(xStart >> 8);
    p[1] = (uint8_t)(xStart & 0xFF);
    p[2] = (uint8_t)(xEnd >> 8);
    p[3] = (uint8_t)(xEnd & 0xFF);
    p[4] = (uint8_t)((vrst >> 8) & 0x03);
    p[5] = (uint8_t)(vrst & 0xFF);
    p[6] = (uint8_t)((vred >> 8) & 0x03);
    p[7] = (uint8_t)(vred & 0xFF);
    p[8] = (uint8_t)(ptScan & 0x01);
    cmdByte(CMD_PTL);
    if (!dataBytes(p, 9)) return err(ERR_SPI);

    _ptHrst = hrst; _ptHred = hred; _ptVrst = vrst; _ptVred = vred;
    warn("partial: S1+TR4, one repeated/combined window only; disjoint windows fade");
    if (_dbg) {
        _dbg->print(F("[UC8179] PTL banks: HRST=")); _dbg->print(hrst);
        _dbg->print(F(" HRED=")); _dbg->print(hred);
        _dbg->print(F(" VRST=")); _dbg->print(vrst);
        _dbg->print(F(" VRED=")); _dbg->println(vred);
    }
    return true;
}

bool UC8179::partialIn() {
    if (!_inited) return err(ERR_NOT_INIT);
    if (!waitBusy(BUSY_TIMEOUT_PREOP)) return err(ERR_BUSY_STUCK);
    return cmdByte(CMD_PTIN) ? true : err(ERR_SPI);
}

bool UC8179::partialOut() {
    if (!_inited) return err(ERR_NOT_INIT);
    if (!waitBusy(BUSY_TIMEOUT_PREOP)) return err(ERR_BUSY_STUCK);
    return cmdByte(CMD_PTOUT) ? true : err(ERR_SPI);
}

uint16_t UC8179::partialRowBytes() const {
    return (uint16_t)(_ptHred - _ptHrst + 1);   // 1 bank = 8 px = 1 byte
}

uint32_t UC8179::partialFrameBytes() const {
    return (uint32_t)partialRowBytes() * (_ptVred - _ptVrst + 1);
}

// ---------------------------------------------------------------------
// Diagnostics
// ---------------------------------------------------------------------

bool UC8179::readFLG(uint8_t& flg) {
    if (!_inited) { _lastErr = ERR_NOT_INIT; return false; }
    if (!cmdByte(CMD_FLG)) return err(ERR_SPI);
    uint8_t v = 0;
    if (!readData(&v, 1)) { _lastErr = ERR_READ; return false; }
    flg = v;
    _lastErr = ERR_NONE;
    return true;
}

bool UC8179::readREV(uint8_t out[7]) {
    if (!_inited) { _lastErr = ERR_NOT_INIT; return false; }
    if (!cmdByte(CMD_REV)) return err(ERR_SPI);
    if (!readData(out, 7)) { _lastErr = ERR_READ; return false; }
    _lastErr = ERR_NONE;
    return true;
}

// ---------------------------------------------------------------------
// Drawing primitives
// ---------------------------------------------------------------------

void UC8179::fillScreen(uint8_t color) {
    if (!_frame) return;
    memset(_frame, color ? 0xFF : 0x00, FRAME_BYTES);
}

void UC8179::drawPixel(int16_t x, int16_t y, uint8_t color) {
    if (!_frame || x < 0 || y < 0 || x >= (int16_t)WIDTH || y >= (int16_t)HEIGHT)
        return;
    uint8_t bit = (uint8_t)(0x80 >> (x & 7));   // D7 = first pixel (§14.4)
    uint8_t* p = _frame + pxOffset(x, y);
    if (color) *p |= bit;
    else       *p &= (uint8_t)~bit;
}

uint8_t UC8179::readPixel(int16_t x, int16_t y) {
    if (!_frame || x < 0 || y < 0 || x >= (int16_t)WIDTH || y >= (int16_t)HEIGHT)
        return INK_WHITE;
    uint8_t bit = (uint8_t)(0x80 >> (x & 7));
    return (_frame[pxOffset(x, y)] & bit) ? INK_BLACK : INK_WHITE;
}

void UC8179::hlineBuf(uint32_t rowBase, int16_t x, int16_t w, uint8_t color) {
    if (w <= 0 || !_frame) return;
    int16_t x0 = x, x1 = x + w - 1;
    if (x0 < 0) x0 = 0;
    if (x1 > (int16_t)WIDTH - 1) x1 = (int16_t)WIDTH - 1;
    if (x0 > x1) return;

    uint8_t* row = _frame + rowBase;
    int16_t bStart = x0 >> 3, bEnd = x1 >> 3;
    for (int16_t b = bStart; b <= bEnd; b++) {
        uint8_t mask = 0xFF;
        if (b == bStart) mask &= (uint8_t)(0xFF >> (x0 & 7));
        if (b == bEnd)   mask &= (uint8_t)(0xFF << (7 - (x1 & 7)));
        if (color) row[b] |= mask;
        else       row[b] &= (uint8_t)~mask;
    }
}

void UC8179::drawFastHLine(int16_t x, int16_t y, int16_t w, uint8_t color) {
    if (!_frame || y < 0 || y >= (int16_t)HEIGHT) return;
    hlineBuf(pxOffset(0, y), x, w, color);
}

void UC8179::drawFastVLine(int16_t x, int16_t y, int16_t h, uint8_t color) {
    for (int16_t i = 0; i < h; i++) drawPixel(x, y + i, color);
}

void UC8179::drawLine(int16_t x0, int16_t y0, int16_t x1, int16_t y1,
                      uint8_t color) {
    int16_t dx = x1 - x0, dy = y1 - y0;
    int16_t sx = dx < 0 ? -1 : 1, sy = dy < 0 ? -1 : 1;
    dx = dx < 0 ? -dx : dx;
    dy = dy < 0 ? -dy : dy;
    int16_t e = dx - dy;
    for (;;) {
        drawPixel(x0, y0, color);
        if (x0 == x1 && y0 == y1) break;
        int16_t e2 = e * 2;
        if (e2 > -dy) { e -= dy; x0 += sx; }
        if (e2 < dx)  { e += dx; y0 += sy; }
    }
}

void UC8179::drawRect(int16_t x, int16_t y, int16_t w, int16_t h, uint8_t color) {
    if (w <= 0 || h <= 0) return;
    drawFastHLine(x, y, w, color);
    drawFastHLine(x, y + h - 1, w, color);
    drawFastVLine(x, y, h, color);
    drawFastVLine(x + w - 1, y, h, color);
}

void UC8179::fillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint8_t color) {
    if (w <= 0 || h <= 0 || !_frame) return;
    for (int16_t row = y; row < y + h; row++) {
        if (row < 0 || row >= (int16_t)HEIGHT) continue;
        hlineBuf(pxOffset(0, row), x, w, color);
    }
}

void UC8179::drawCircle(int16_t x0, int16_t y0, int16_t r, uint8_t color) {
    if (r < 0) return;
    if (r == 0) { drawPixel(x0, y0, color); return; }
    int16_t f = 1 - r;
    int16_t ddx = 1, ddy = -2 * r;
    int16_t px = 0, py = r;
    drawPixel(x0, y0 + r, color);
    drawPixel(x0, y0 - r, color);
    drawPixel(x0 + r, y0, color);
    drawPixel(x0 - r, y0, color);
    while (px < py) {
        if (f >= 0) { py--; ddy += 2; f += ddy; }
        px++;
        ddx += 2;
        f += ddx;
        drawPixel(x0 + px, y0 + py, color);
        drawPixel(x0 - px, y0 + py, color);
        drawPixel(x0 + px, y0 - py, color);
        drawPixel(x0 - px, y0 - py, color);
        drawPixel(x0 + py, y0 + px, color);
        drawPixel(x0 - py, y0 + px, color);
        drawPixel(x0 + py, y0 - px, color);
        drawPixel(x0 - py, y0 - px, color);
    }
}

void UC8179::fillCircle(int16_t x0, int16_t y0, int16_t r, uint8_t color) {
    if (r < 0) return;
    for (int16_t dy = -r; dy <= r; dy++) {
        int16_t dx = r;
        while ((int32_t)dx * dx + (int32_t)dy * dy > (int32_t)r * r) dx--;
        drawFastHLine(x0 - dx, y0 + dy, 2 * dx + 1, color);
    }
}

void UC8179::drawBitmap(int16_t x, int16_t y, const uint8_t* bitmap,
                        int16_t w, int16_t h, uint8_t color) {
    int16_t bytesPerRow = (w + 7) / 8;
    for (int16_t j = 0; j < h; j++) {
        for (int16_t i = 0; i < w; i++) {
            uint8_t v = pgm_read_byte(bitmap + (int32_t)j * bytesPerRow + i / 8);
            if (v & (0x80 >> (i & 7))) drawPixel(x + i, y + j, color);
        }
    }
}

// ---------------------------------------------------------------------
// Text (Print interface)
// ---------------------------------------------------------------------

void UC8179::setCursor(int16_t x, int16_t y) {
    _curX = x;
    _curY = y;
    _curX0 = x;
}

void UC8179::setTextSize(uint8_t s) {
    _textSize = (s == 0) ? 1 : s;
}

void UC8179::setTextColor(uint8_t fg, int8_t bg) {
    _textFg = fg;
    _textBg = bg;
}

void UC8179::drawChar(int16_t x, int16_t y, uint8_t c, uint8_t fg, int8_t bg,
                      uint8_t size) {
    if (c < 0x20 || c > 0x7E) c = '?';
    if (size == 0) size = 1;
    for (uint8_t col = 0; col < 5; col++) {
        uint8_t bits = pgm_read_byte(&uc8179Font5x7[((uint16_t)c - 0x20) * 5 + col]);
        for (uint8_t row = 0; row < 8; row++) {
            if (bits & (1 << row))
                fillRect(x + col * size, y + row * size, size, size, fg);
            else if (bg >= 0)
                fillRect(x + col * size, y + row * size, size, size, (uint8_t)bg);
        }
    }
}

size_t UC8179::write(uint8_t c) {
    if (!_frame) return 0;
    if (c == '\r') return 1;
    if (c == '\n') {
        _curY += 8 * _textSize;
        _curX = _curX0;
        return 1;
    }
    if (c == '\t') c = ' ';
    if (c < 0x20) return 1;
    if (c > 0x7E) c = '?';
    if (_curX + 6 * _textSize > (int16_t)WIDTH) {   // wrap at right edge
        _curY += 8 * _textSize;
        _curX = _curX0;
    }
    drawChar(_curX, _curY, c, _textFg, _textBg, _textSize);
    _curX += 6 * _textSize;
    return 1;
}
