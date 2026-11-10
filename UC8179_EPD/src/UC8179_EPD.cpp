#include "UC8179_EPD.h"

#include <stdarg.h>
#include <string.h>

// 648 / 8 = 81 字节/行
#define ROW_BYTES (UC8179_PANEL_WIDTH / 8)

bool UC8179_EPD::_spiBegun = false;

// TR0~TR7 对应的强制温度值（取自 OTP 温度分档边界）
static const uint8_t UC8179_TR_TEMP[8] = {5, 10, 15, 20, 30, 80, 90, 127};

UC8179_EPD::UC8179_EPD(int16_t cs, int16_t dc, int16_t rst, int16_t busy)
    : _cs(cs),
      _dc(dc),
      _rst(rst),
      _busy(busy),
      _sck(-1),
      _mosi(-1),
      _readPin(-1),
      _spiHz(UC8179_SPI_DEFAULT_HZ),
      _spiSettings(UC8179_SPI_DEFAULT_HZ, MSBFIRST, SPI_MODE0),
      _rotation(0),
      _width(UC8179_PANEL_WIDTH),
      _height(UC8179_PANEL_HEIGHT),
      _buffer(nullptr),
      _ownBuffer(false),
      _invert(false),
      _refreshing(false),
      _timedOut(false),
      _powered(false),
      _everPowered(false),
      _vdcsValid(false),
      _powerOffAfter(false),
      _oldSame(false),
      _shadow(nullptr),
      _ownShadow(false),
      _shadowValid(false),
      _readOk(false),
      _readProbed(false),
      _readDummy(0),
      _otpValid(false),
      _otpProfile(false),
      _otpResMatch(true),
      _verifyFrame(true),
      _frameVerified(false),
      _frameLoaded(false),
      _lastStatus(0),
      _basePsr(UC8179_PROFILE_PSR),
      _tbCount(UC8179_PROFILE_TB_COUNT),
      _hst(UC8179_PANEL_HST),
      _vst(UC8179_PANEL_VST),
      _refreshStartMs(0),
      _lastRefreshMs(0),
      _lastDisplayMs(0),
      _refreshMode(REFRESH_AUTO) {
  _loadDefaults();
}

UC8179_EPD::~UC8179_EPD() {
  if (_ownBuffer && _buffer) {
    free(_buffer);
  }
  _buffer = nullptr;
  if (_ownShadow && _shadow) {
    free(_shadow);
  }
  _shadow = nullptr;
}

void UC8179_EPD::_loadDefaults() {
  _basePsr = UC8179_PROFILE_PSR;
  _p.psr = (uint8_t)(_basePsr | UC8179_UD_FLIP_BIT);
  _p.vg = UC8179_PROFILE_VG;
  _p.vdh = UC8179_PROFILE_VDH;
  _p.vdl = UC8179_PROFILE_VDL;
  _p.vdhr = UC8179_PROFILE_VDHR;
  _p.btstA = UC8179_PROFILE_BTST1;
  _p.btstB = UC8179_PROFILE_BTST2;
  _p.btstC1 = UC8179_PROFILE_BTST3;
  _p.btstC2 = UC8179_PROFILE_BTST4;
  _p.frs = 0x06;                 // 50Hz
  _p.cdi1 = 0x12;                // DDX=10 单平面，BDV=01
  _p.cdi2 = 0x07;                // 10 hsync
  _p.tcon = UC8179_PROFILE_TCON;
  _p.evs = 0x02;                 // VCEND=0, BDEND=10(VCOM_DC)
  _p.vdcs = UC8179_PROFILE_VDCS; // -1.00V，默认不写，交给 OTP TR
  _p.pws = 0x88;
  _p.lvsel = 0x03;
  _p.tse = 0x00;   // 内部温度传感器，偏移 0
  _p.tsfix = 0;    // 自动温度分档
  _p.tsset = 0;
  _vdcsValid = false;
  _refreshMode = REFRESH_AUTO;

  static const int8_t TB[11] = {
      UC8179_PROFILE_TB0, UC8179_PROFILE_TB1, UC8179_PROFILE_TB2,
      UC8179_PROFILE_TB3, UC8179_PROFILE_TB4, UC8179_PROFILE_TB5,
      UC8179_PROFILE_TB6, UC8179_PROFILE_TB7, UC8179_PROFILE_TB8,
      UC8179_PROFILE_TB9, UC8179_PROFILE_TB10};
  for (uint8_t i = 0; i < 11; i++) {
    _tb[i] = (uint8_t)TB[i];
  }
  _tbCount = UC8179_PROFILE_TB_COUNT;
  _hst = UC8179_PANEL_HST;
  _vst = UC8179_PANEL_VST;
  _otpProfile = false;
  _otpResMatch = true;
}

// ================================================================ 初始化

bool UC8179_EPD::begin(int8_t sck, int8_t miso, int8_t mosi, uint32_t spiHz,
                       bool doReset) {
  _spiHz = spiHz;
  _spiSettings = SPISettings(spiHz, MSBFIRST, SPI_MODE0);

  pinMode(_cs, OUTPUT);
  digitalWrite(_cs, HIGH);
  pinMode(_dc, OUTPUT);
  digitalWrite(_dc, LOW);
  if (_busy >= 0) {
    pinMode(_busy, INPUT_PULLUP);
  }
  if (_rst >= 0) {
    pinMode(_rst, OUTPUT);
    digitalWrite(_rst, HIGH);
  }

  // 记录引脚：读取走位翻转，MISO 不挂到硬件 SPI，避免 MOSI/MISO 同线冲突
  _sck = (sck >= 0) ? sck : -1;
  _mosi = (mosi >= 0) ? mosi : -1;
  _readPin = (miso >= 0) ? miso : -1;
  if (_sck < 0 || _mosi < 0) {
    _readPin = -1;  // 用了核心默认引脚，位翻转无法定位，关闭读取
  }

  if (!_spiBegun) {
#if defined(ARDUINO_ARCH_ESP32)
    SPI.begin(sck, -1, mosi, -1);
#else
    (void)sck;
    (void)miso;
    (void)mosi;
    SPI.begin();
#endif
    _spiBegun = true;
  }

  if (!_buffer) {
    _buffer = (uint8_t *)malloc(bufferSize());
    if (!_buffer) {
      return false;
    }
    _ownBuffer = true;
  }
  memset(_buffer, 0x00, bufferSize());  // 0 = 白

  if (doReset && _rst >= 0) {
    hardwareReset();
  } else {
    softReset();
  }
  delay(2);

  // 有回读线就校准读时序，并检查 OTP 是否烧写
  if (_readPin >= 0) {
    _readProbed = true;
    _readOk = _probeRead();
    if (_readOk) {
      loadOtpProfile();  // 读到 OTP 就直接套用面板参数
    }
  }

  _config();
  return true;
}

bool UC8179_EPD::setBuffer(uint8_t *buf) {
  if (!buf) {
    return false;
  }
  if (_ownBuffer && _buffer) {
    free(_buffer);
  }
  _buffer = buf;
  _ownBuffer = false;
  return true;
}

bool UC8179_EPD::enableShadow(bool on) {
  if (on) {
    if (_shadow) {
      return true;
    }
    _shadow = (uint8_t *)malloc(bufferSize());
    if (!_shadow) {
      return false;
    }
    memset(_shadow, 0x00, bufferSize());
    _ownShadow = true;
    _shadowValid = false;
    return true;
  }
  if (_ownShadow && _shadow) {
    free(_shadow);
  }
  _shadow = nullptr;
  _ownShadow = false;
  _shadowValid = false;
  return true;
}

bool UC8179_EPD::setReadPin(int8_t pin) {
  if (pin < 0) {
    _readPin = -1;
    _readOk = false;
    return false;
  }
  _readPin = pin;
  _readProbed = true;
  _readOk = _probeRead();
  if (_readOk) {
    loadOtpProfile();
    if (_buffer) {
      _config();
    }
  }
  return _readOk;
}

void UC8179_EPD::_config() {
  _writePsr();
  _writePwr();
  _writeBtst();
  _writePll();
  _writeCdi();
  _writeTcon();
  _writeTres();
  _writeGsst();
  _writeEvs();
  if (_vdcsValid) {
    _writeVdcs();
  }
  _writePws();
  _writeLvsel();
  _writeTse();
  _writeTemp();
}

// ---- 单个寄存器写入 ----

void UC8179_EPD::_writePsr() {
  _writeCommand(0x00);
  _writeData(_p.psr);
}

void UC8179_EPD::_writePwr() {
  _writeCommand(0x01);
  _writeData(0x07);  // BD_EN=0, VSR/VS/VG_EN=1
  _writeData((uint8_t)(0x10 | (_p.vg & 0x07)));
  _writeData((uint8_t)(_p.vdh & 0x3F));
  _writeData((uint8_t)(_p.vdl & 0x3F));
  _writeData((uint8_t)(_p.vdhr & 0x3F));
}

void UC8179_EPD::_writeBtst() {
  _writeCommand(0x06);
  _writeData(_p.btstA);
  _writeData(_p.btstB);
  _writeData(_p.btstC1);
  _writeData(_p.btstC2);
}

void UC8179_EPD::_writePll() {
  _writeCommand(0x30);
  _writeData((uint8_t)(_p.frs & 0x0F));
}

void UC8179_EPD::_writeCdi() {
  _writeCommand(0x50);
  _writeData(_p.cdi1);
  _writeData((uint8_t)(_p.cdi2 & 0x0F));
}

void UC8179_EPD::_writeTcon() {
  _writeCommand(0x60);
  _writeData(_p.tcon);
}

void UC8179_EPD::_writeTres() {
  _writeCommand(0x61);
  _writeData((uint8_t)((UC8179_PANEL_HRES >> 8) & 0x03));
  _writeData((uint8_t)(UC8179_PANEL_HRES & 0xF8));
  _writeData((uint8_t)((UC8179_PANEL_VRES >> 8) & 0x03));
  _writeData((uint8_t)(UC8179_PANEL_VRES & 0xFF));
}

void UC8179_EPD::_writeGsst() {
  _writeCommand(0x65);
  _writeData((uint8_t)((_hst >> 8) & 0x03));
  _writeData((uint8_t)(_hst & 0xF8));
  _writeData((uint8_t)((_vst >> 8) & 0x03));
  _writeData((uint8_t)(_vst & 0xFF));
}

void UC8179_EPD::_writeEvs() {
  _writeCommand(0x52);
  _writeData(_p.evs);
}

void UC8179_EPD::_writeVdcs() {
  _writeCommand(0x82);
  _writeData((uint8_t)(_p.vdcs & 0x7F));
}

void UC8179_EPD::_writePws() {
  _writeCommand(0xE3);
  _writeData(_p.pws);
}

void UC8179_EPD::_writeLvsel() {
  _writeCommand(0xE4);
  _writeData((uint8_t)(_p.lvsel & 0x03));
}

void UC8179_EPD::_writeTse() {
  _writeCommand(0x41);
  _writeData(_p.tse);
}

void UC8179_EPD::_writeTemp() {
  _writeCommand(0xE0);
  _writeData(_p.tsfix ? 0x02 : 0x00);  // TSFIX
  if (_p.tsfix) {
    _writeCommand(0xE5);
    _writeData(_p.tsset);  // TS_SET
  }
}

void UC8179_EPD::hardwareReset() {
  if (_rst < 0) {
    softReset();
    return;
  }
  digitalWrite(_rst, HIGH);
  delay(1);
  digitalWrite(_rst, LOW);
  delay(10);
  digitalWrite(_rst, HIGH);
  delay(10);
  _waitBusy(2000);
}

void UC8179_EPD::softReset() {
  _writeCommand(0x00);
  _writeData((uint8_t)(_p.psr & ~0x01));
  delay(20);          // 软复位后内部初始化，命令太早会被忽略
  _waitBusy(2000);
}

// ================================================================ 参数设置

void UC8179_EPD::resetParams() {
  _loadDefaults();
  if (_readOk) {
    loadOtpProfile();  // 有回读就重新套 OTP
  }
  if (_buffer) {
    // 探测可能改过 VDCS，这里显式写回复位默认 0x00，避免依赖软复位
    _p.vdcs = 0x00;
    _vdcsValid = false;
    _writeCommand(0x82);
    _writeData(0x00);
    _config();
  }
}

void UC8179_EPD::setRotation(uint8_t r) {
  _rotation = r & 0x03;
  if (_rotation & 0x01) {
    _width = UC8179_PANEL_HEIGHT;
    _height = UC8179_PANEL_WIDTH;
  } else {
    _width = UC8179_PANEL_WIDTH;
    _height = UC8179_PANEL_HEIGHT;
  }
}

void UC8179_EPD::setMirror(bool mirrorX, bool mirrorY) {
  _p.psr = (uint8_t)(_basePsr | UC8179_UD_FLIP_BIT);
  if (mirrorY) _p.psr ^= 0x08;  // UD
  if (mirrorX) _p.psr ^= 0x04;  // SHL
  if (_buffer) {
    _writePsr();
  }
}

void UC8179_EPD::setWaveformGear(uint8_t gear) {
  if (gear > 7) {
    return;
  }
  _p.tsfix = 1;
  _p.tsset = UC8179_TR_TEMP[gear];
  _refreshMode = REFRESH_CUSTOM;
  if (_buffer) {
    _writeTemp();
  }
}

void UC8179_EPD::setTemperatureForce(uint8_t tempCode) {
  _p.tsfix = 1;
  _p.tsset = tempCode;
  _refreshMode = REFRESH_CUSTOM;
  if (_buffer) {
    _writeTemp();
  }
}

void UC8179_EPD::setTemperatureAuto() {
  _p.tsfix = 0;
  _refreshMode = REFRESH_AUTO;
  if (_buffer) {
    _writeTemp();
  }
}

void UC8179_EPD::setTemperatureOffset(int8_t deg) {
  if (deg < -8) deg = -8;
  if (deg > 7) deg = 7;
  _p.tse = (uint8_t)((_p.tse & 0x80) | (deg & 0x0F));
  if (_buffer) {
    _writeTse();
  }
}

void UC8179_EPD::setRefreshMode(RefreshMode mode) {
  _refreshMode = mode;
  if (mode == REFRESH_FAST) {
    _p.tsfix = 1;
    _p.tsset = 0x1E;
  } else if (mode == REFRESH_QUALITY) {
    _p.tsfix = 1;
    _p.tsset = 0x7F;
  } else {
    _p.tsfix = 0;
    _refreshMode = REFRESH_AUTO;
  }
  if (_buffer) {
    _writeTemp();
  }
}

void UC8179_EPD::setVcomDc(uint8_t code) {
  _p.vdcs = (uint8_t)(code & 0x7F);
  _vdcsValid = true;
  if (_buffer) {
    _writeVdcs();
  }
}

void UC8179_EPD::clearVcomDc() {
  _vdcsValid = false;
}

void UC8179_EPD::setSourceVoltage(uint8_t vdh, uint8_t vdl) {
  _p.vdh = (uint8_t)(vdh & 0x3F);
  _p.vdl = (uint8_t)(vdl & 0x3F);
  if (_buffer) {
    _writePwr();
  }
}

void UC8179_EPD::setVdhr(uint8_t code) {
  _p.vdhr = (uint8_t)(code & 0x3F);
  if (_buffer) {
    _writePwr();
  }
}

void UC8179_EPD::setGateVoltage(uint8_t vg) {
  _p.vg = (uint8_t)(vg & 0x07);
  if (_buffer) {
    _writePwr();
  }
}

void UC8179_EPD::setFrameRateCode(uint8_t frs) {
  _p.frs = (uint8_t)(frs & 0x0F);
  if (_buffer) {
    _writePll();
  }
}

void UC8179_EPD::setCdiInterval(uint8_t cdi) {
  _p.cdi2 = (uint8_t)(cdi & 0x0F);
  if (_buffer) {
    _writeCdi();
  }
}

void UC8179_EPD::setCdiRegister(uint8_t cdi1, uint8_t cdi2) {
  _p.cdi1 = cdi1;
  _p.cdi2 = (uint8_t)(cdi2 & 0x0F);
  if (_buffer) {
    _writeCdi();
  }
}

void UC8179_EPD::setBorderLut(uint8_t bdv) {
  _p.cdi1 = (uint8_t)((_p.cdi1 & ~0x30) | ((bdv & 0x03) << 4));
  if (_buffer) {
    _writeCdi();
  }
}

void UC8179_EPD::setBorderHiZ(bool on) {
  if (on) {
    _p.cdi1 |= 0x80;
  } else {
    _p.cdi1 &= (uint8_t)~0x80;
  }
  if (_buffer) {
    _writeCdi();
  }
}

void UC8179_EPD::setN2Ocp(bool on) {
  if (on) {
    _p.cdi1 |= 0x08;
  } else {
    _p.cdi1 &= (uint8_t)~0x08;
  }
  if (_buffer) {
    _writeCdi();
  }
}

void UC8179_EPD::setTcon(uint8_t tcon) {
  _p.tcon = tcon;
  if (_buffer) {
    _writeTcon();
  }
}

void UC8179_EPD::setBorderEnd(uint8_t evs) {
  _p.evs = evs;
  if (_buffer) {
    _writeEvs();
  }
}

void UC8179_EPD::setBoosterSoftStart(uint8_t a, uint8_t b, uint8_t c1,
                                     uint8_t c2) {
  _p.btstA = a;
  _p.btstB = b;
  _p.btstC1 = c1;
  _p.btstC2 = c2;
  if (_buffer) {
    _writeBtst();
  }
}

void UC8179_EPD::setPowerSaving(uint8_t pws) {
  _p.pws = pws;
  if (_buffer) {
    _writePws();
  }
}

void UC8179_EPD::setLvsel(uint8_t lvsel) {
  _p.lvsel = (uint8_t)(lvsel & 0x03);
  if (_buffer) {
    _writeLvsel();
  }
}

void UC8179_EPD::setSinglePlane(bool on) {
  _p.cdi1 = (uint8_t)((_p.cdi1 & ~0x03) | (on ? 0x02 : 0x00));
  if (_buffer) {
    _writeCdi();
  }
}

bool UC8179_EPD::singlePlane() const {
  return (_p.cdi1 & 0x03) == 0x02;
}

// ================================================================ 参数打印

static void printHexEntry(Print &out, const __FlashStringHelper *name,
                          uint8_t v) {
  out.print(name);
  out.print(F("=0x"));
  if (v < 0x10) out.print('0');
  out.print(v, HEX);
  out.print(' ');
}

void UC8179_EPD::printConfig(Print &out) {
  printHexEntry(out, F("PSR"), _p.psr);
  printHexEntry(out, F("VG"), _p.vg);
  printHexEntry(out, F("VDH"), _p.vdh);
  printHexEntry(out, F("VDL"), _p.vdl);
  printHexEntry(out, F("VDHR"), _p.vdhr);
  printHexEntry(out, F("FRS"), _p.frs);
  printHexEntry(out, F("CDI1"), _p.cdi1);
  printHexEntry(out, F("CDI2"), _p.cdi2);
  printHexEntry(out, F("TCON"), _p.tcon);
  printHexEntry(out, F("EVS"), _p.evs);
  printHexEntry(out, F("PWS"), _p.pws);
  printHexEntry(out, F("LVSEL"), _p.lvsel);
  printHexEntry(out, F("TSE"), _p.tse);
  printHexEntry(out, F("TSFIX"), _p.tsfix);
  printHexEntry(out, F("TSSET"), _p.tsset);
  if (_vdcsValid) {
    printHexEntry(out, F("VDCS"), _p.vdcs);
  } else {
    out.print(F("VDCS=OTP "));
  }
  out.print(F("BTST="));
  if (_p.btstA < 0x10) out.print('0');
  out.print(_p.btstA, HEX);
  out.print(',');
  if (_p.btstB < 0x10) out.print('0');
  out.print(_p.btstB, HEX);
  out.print(',');
  if (_p.btstC1 < 0x10) out.print('0');
  out.print(_p.btstC1, HEX);
  out.print(',');
  if (_p.btstC2 < 0x10) out.print('0');
  out.print(_p.btstC2, HEX);
  out.println();
}

// ================================================================ SPI

void UC8179_EPD::_writeCommand(uint8_t cmd) {
  SPI.beginTransaction(_spiSettings);
  digitalWrite(_cs, LOW);
  digitalWrite(_dc, LOW);
  SPI.transfer(cmd);
  digitalWrite(_cs, HIGH);
  SPI.endTransaction();
}

void UC8179_EPD::_writeData(uint8_t data) {
  SPI.beginTransaction(_spiSettings);
  digitalWrite(_cs, LOW);
  digitalWrite(_dc, HIGH);
  SPI.transfer(data);
  digitalWrite(_cs, HIGH);
  SPI.endTransaction();
}

void UC8179_EPD::_writeData(const uint8_t *data, size_t len) {
  if (!data || len == 0) {
    return;
  }
  SPI.beginTransaction(_spiSettings);
  digitalWrite(_cs, LOW);
  digitalWrite(_dc, HIGH);
#if defined(ARDUINO_ARCH_ESP32)
  SPI.writeBytes(data, len);
#else
  for (size_t i = 0; i < len; i++) {
    SPI.transfer(UC8179_PGM_READ(data + i));
  }
#endif
  digitalWrite(_cs, HIGH);
  SPI.endTransaction();
}

void UC8179_EPD::_writeDataMasked(const uint8_t *data, size_t len,
                                   uint8_t mask) {
  if (!mask) {
    _writeData(data, len);
    return;
  }
  uint8_t chunk[256];
  SPI.beginTransaction(_spiSettings);
  digitalWrite(_cs, LOW);
  digitalWrite(_dc, HIGH);
  while (len) {
    size_t n = len > sizeof(chunk) ? sizeof(chunk) : len;
    for (size_t i = 0; i < n; i++) {
      chunk[i] = (uint8_t)(UC8179_PGM_READ(data + i) ^ mask);
    }
#if defined(ARDUINO_ARCH_ESP32)
    SPI.writeBytes(chunk, n);
#else
    for (size_t i = 0; i < n; i++) {
      SPI.transfer(chunk[i]);
    }
#endif
    data += n;
    len -= n;
  }
  digitalWrite(_cs, HIGH);
  SPI.endTransaction();
}

void UC8179_EPD::_writeRepeat(uint8_t value, size_t len) {
  uint8_t chunk[128];
  memset(chunk, value, sizeof(chunk));
  SPI.beginTransaction(_spiSettings);
  digitalWrite(_cs, LOW);
  digitalWrite(_dc, HIGH);
#if defined(ARDUINO_ARCH_ESP32)
  while (len) {
    size_t n = len > sizeof(chunk) ? sizeof(chunk) : len;
    SPI.writeBytes(chunk, n);
    len -= n;
  }
#else
  while (len--) {
    SPI.transfer(value);
  }
#endif
  digitalWrite(_cs, HIGH);
  SPI.endTransaction();
}

void UC8179_EPD::_waitBusy(uint32_t timeoutMs) {
  if (_busy < 0) {
    return;
  }
  uint32_t start = millis();
  while (digitalRead(_busy) == LOW) {
    if (millis() - start > timeoutMs) {
      _timedOut = true;
      return;
    }
    delay(1);
  }
}

bool UC8179_EPD::isBusy() const {
  return _busy >= 0 && digitalRead(_busy) == LOW;
}

// ================================================================ 像素

void UC8179_EPD::_putPixel(int16_t px, int16_t py, bool black) {
  uint16_t idx = (uint16_t)py * ROW_BYTES + (uint16_t)(px >> 3);
  uint8_t mask = (uint8_t)(0x80 >> (px & 7));
  if (black) {
    _buffer[idx] |= mask;
  } else {
    _buffer[idx] &= (uint8_t)~mask;
  }
}

void UC8179_EPD::drawPixel(int16_t x, int16_t y, uint16_t color) {
  if (!_buffer || x < 0 || y < 0 || x >= _width || y >= _height) {
    return;
  }
  int16_t px, py;
  switch (_rotation) {
    case 1:
      px = y;
      py = UC8179_PANEL_HEIGHT - 1 - x;
      break;
    case 2:
      px = UC8179_PANEL_WIDTH - 1 - x;
      py = UC8179_PANEL_HEIGHT - 1 - y;
      break;
    case 3:
      px = UC8179_PANEL_WIDTH - 1 - y;
      py = x;
      break;
    default:
      px = x;
      py = y;
      break;
  }
  bool black = (color == UC8179_BLACK);
  if (_invert) {
    black = !black;
  }
  _putPixel(px, py, black);
}

void UC8179_EPD::fillScreen(uint16_t color) {
  if (!_buffer) {
    return;
  }
  bool black = (color == UC8179_BLACK);
  if (_invert) {
    black = !black;
  }
  memset(_buffer, black ? 0xFF : 0x00, bufferSize());
}

void UC8179_EPD::_fillRowNative(int16_t x0, int16_t x1, int16_t y, bool black) {
  if (x0 > x1) {
    return;
  }
  uint16_t base = (uint16_t)y * ROW_BYTES;
  int16_t b0 = x0 >> 3;
  int16_t b1 = x1 >> 3;
  uint8_t fill = black ? 0xFF : 0x00;

  if (b0 == b1) {
    uint8_t mask = (uint8_t)((0xFF >> (x0 & 7)) & (0xFF << (7 - (x1 & 7))));
    if (black) {
      _buffer[base + b0] |= mask;
    } else {
      _buffer[base + b0] &= (uint8_t)~mask;
    }
    return;
  }

  uint8_t mask = (uint8_t)(0xFF >> (x0 & 7));
  if (black) {
    _buffer[base + b0] |= mask;
  } else {
    _buffer[base + b0] &= (uint8_t)~mask;
  }

  if (b1 > b0 + 1) {
    memset(&_buffer[base + b0 + 1], fill, (size_t)(b1 - b0 - 1));
  }

  mask = (uint8_t)(0xFF << (7 - (x1 & 7)));
  if (black) {
    _buffer[base + b1] |= mask;
  } else {
    _buffer[base + b1] &= (uint8_t)~mask;
  }
}

void UC8179_EPD::fillRect(int16_t x, int16_t y, int16_t w, int16_t h,
                          uint16_t color) {
  if (!_buffer || w <= 0 || h <= 0) {
    return;
  }
  int16_t x1 = x + w - 1;
  int16_t y1 = y + h - 1;
  if (x < 0) x = 0;
  if (y < 0) y = 0;
  if (x1 >= _width) x1 = _width - 1;
  if (y1 >= _height) y1 = _height - 1;
  if (x > x1 || y > y1) {
    return;
  }

  bool black = (color == UC8179_BLACK);
  if (_invert) {
    black = !black;
  }

  if (_rotation == 0) {
    for (int16_t yy = y; yy <= y1; yy++) {
      _fillRowNative(x, x1, yy, black);
    }
  } else {
    for (int16_t yy = y; yy <= y1; yy++) {
      for (int16_t xx = x; xx <= x1; xx++) {
        drawPixel(xx, yy, color);
      }
    }
  }
}

void UC8179_EPD::drawFastHLine(int16_t x, int16_t y, int16_t w, uint16_t color) {
  fillRect(x, y, w, 1, color);
}

void UC8179_EPD::drawFastVLine(int16_t x, int16_t y, int16_t h, uint16_t color) {
  fillRect(x, y, 1, h, color);
}

void UC8179_EPD::drawRect(int16_t x, int16_t y, int16_t w, int16_t h,
                          uint16_t color) {
  if (w <= 0 || h <= 0) {
    return;
  }
  drawFastHLine(x, y, w, color);
  drawFastHLine(x, y + h - 1, w, color);
  drawFastVLine(x, y, h, color);
  drawFastVLine(x + w - 1, y, h, color);
}

void UC8179_EPD::drawLine(int16_t x0, int16_t y0, int16_t x1, int16_t y1,
                          uint16_t color) {
  int16_t dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
  int16_t dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
  int16_t err = dx + dy;
  while (true) {
    drawPixel(x0, y0, color);
    if (x0 == x1 && y0 == y1) {
      break;
    }
    int16_t e2 = 2 * err;
    if (e2 >= dy) {
      err += dy;
      x0 += sx;
    }
    if (e2 <= dx) {
      err += dx;
      y0 += sy;
    }
  }
}

void UC8179_EPD::drawCircle(int16_t x0, int16_t y0, int16_t r,
                            uint16_t color) {
  int16_t f = 1 - r;
  int16_t ddF_x = 1;
  int16_t ddF_y = -2 * r;
  int16_t x = 0;
  int16_t y = r;

  drawPixel(x0, y0 + r, color);
  drawPixel(x0, y0 - r, color);
  drawPixel(x0 + r, y0, color);
  drawPixel(x0 - r, y0, color);

  while (x < y) {
    if (f >= 0) {
      y--;
      ddF_y += 2;
      f += ddF_y;
    }
    x++;
    ddF_x += 2;
    f += ddF_x;

    drawPixel(x0 + x, y0 + y, color);
    drawPixel(x0 - x, y0 + y, color);
    drawPixel(x0 + x, y0 - y, color);
    drawPixel(x0 - x, y0 - y, color);
    drawPixel(x0 + y, y0 + x, color);
    drawPixel(x0 - y, y0 + x, color);
    drawPixel(x0 + y, y0 - x, color);
    drawPixel(x0 - y, y0 - x, color);
  }
}

void UC8179_EPD::fillCircle(int16_t x0, int16_t y0, int16_t r,
                            uint16_t color) {
  drawFastVLine(x0, y0 - r, 2 * r + 1, color);
  int16_t f = 1 - r;
  int16_t ddF_x = 1;
  int16_t ddF_y = -2 * r;
  int16_t x = 0;
  int16_t y = r;

  while (x < y) {
    if (f >= 0) {
      y--;
      ddF_y += 2;
      f += ddF_y;
    }
    x++;
    ddF_x += 2;
    f += ddF_x;

    drawFastVLine(x0 + x, y0 - y, 2 * y + 1, color);
    drawFastVLine(x0 - x, y0 - y, 2 * y + 1, color);
    drawFastVLine(x0 + y, y0 - x, 2 * x + 1, color);
    drawFastVLine(x0 - y, y0 - x, 2 * x + 1, color);
  }
}

void UC8179_EPD::drawBitmap(int16_t x, int16_t y, const uint8_t *bitmap,
                            int16_t w, int16_t h, uint16_t color) {
  if (!_buffer || !bitmap || w <= 0 || h <= 0) {
    return;
  }
  int16_t stride = (w + 7) / 8;
  for (int16_t j = 0; j < h; j++) {
    const uint8_t *row = bitmap + (int32_t)j * stride;
    for (int16_t i = 0; i < w; i++) {
      if (UC8179_PGM_READ(row + (i >> 3)) & (0x80 >> (i & 7))) {
        drawPixel(x + i, y + j, color);
      }
    }
  }
}

int16_t UC8179_EPD::drawChar(int16_t x, int16_t y, char c, uint16_t color,
                             uint8_t size) {
  if (c < 32 || c > 126) {
    c = '?';
  }
  const uint8_t *glyph = UC8179_FONT7X8[(uint8_t)c - 32];
  for (int8_t row = 0; row < UC8179_FONT_H; row++) {
    uint8_t bits = UC8179_PGM_READ(&glyph[row]);
    for (int8_t col = 0; col < UC8179_FONT_W; col++) {
      if (bits & (0x40 >> col)) {
        fillRect(x + col * size, y + row * size, size, size, color);
      }
    }
  }
  return x + (UC8179_FONT_W + 1) * size;
}

int16_t UC8179_EPD::drawString(int16_t x, int16_t y, const char *s,
                               uint16_t color, uint8_t size) {
  int16_t cx = x;
  int16_t cy = y;
  while (s && *s) {
    if (*s == '\n') {
      cx = x;
      cy += (UC8179_FONT_H + 2) * size;
    } else if (*s != '\r') {
      cx = drawChar(cx, cy, *s, color, size);
    }
    s++;
  }
  return cx;
}

void UC8179_EPD::drawStringf(int16_t x, int16_t y, uint16_t color,
                             uint8_t size, const char *fmt, ...) {
  char buf[96];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  drawString(x, y, buf, color, size);
}

// ================================================================ 刷新

void UC8179_EPD::_powerOn() {
  _timedOut = false;
  if (_powered) {
    return;  // 保持供电模式：已经 PON 就不重复上电
  }
  _writeCommand(0x04);
  _waitBusy(5000);
  if (!_timedOut) {
    _powered = true;
    _everPowered = true;
  }
}

bool UC8179_EPD::_sendFrame(const uint8_t *image) {
  _powerOn();

  // DDX=01 是数据极性位（厂商用 1=白，本库用 1=黑），发送时整体取反
  const uint8_t txMask = ((_p.cdi1 & 0x03) == 0x01) ? 0xFF : 0x00;

  if (!singlePlane()) {
    _writeCommand(0x10);
    if (_shadow && _shadowValid) {
      _writeDataMasked(_shadow, bufferSize(), txMask);  // OLD = 上一帧真实内容
    } else if (_oldSame) {
      _writeDataMasked(image, bufferSize(), txMask);  // OLD = NEW
    } else {
      _writeRepeat((uint8_t)(0x00 ^ txMask), bufferSize());  // OLD = 白
    }
  }

  _writeCommand(0x13);
  _writeDataMasked(image, bufferSize(), txMask);

  if (_shadow) {
    for (size_t i = 0; i < bufferSize(); i++) {
      _shadow[i] = UC8179_PGM_READ(image + i);
    }
    _shadowValid = true;
  }

  // 有回读就检查 Data_Flag，确认整帧真的收齐了再触发刷新
  _frameVerified = true;
  if (_verifyFrame && _readOk) {
    uint8_t flg = readStatus();
    _frameVerified = (flg & UC8179_FLG_DATA) != 0;
  }

  _frameLoaded = true;
  return true;
}

void UC8179_EPD::triggerRefresh() {
  if (!_frameLoaded || _refreshing) {
    return;
  }
  _writeCommand(0x12);  // DRF
  _refreshStartMs = millis();
  _refreshing = true;
  _frameLoaded = false;
}

void UC8179_EPD::writeFrame() {
  if (!_buffer || _refreshing) {
    return;
  }
  _sendFrame(_buffer);
}

void UC8179_EPD::startRefresh() {
  if (!_buffer || _refreshing) {
    return;
  }
  _sendFrame(_buffer);
  triggerRefresh();
}

bool UC8179_EPD::display() {
  if (!_buffer) {
    return false;
  }
  uint32_t t0 = millis();
  startRefresh();
  bool ok = waitRefresh();
  _lastDisplayMs = millis() - t0;
  return ok;
}

bool UC8179_EPD::display(const uint8_t *nativeImage) {
  if (!nativeImage || _refreshing) {
    return false;
  }
  uint32_t t0 = millis();
  _sendFrame(nativeImage);
  bool ok = waitRefresh();
  _lastDisplayMs = millis() - t0;
  return ok;
}

bool UC8179_EPD::waitRefresh(uint32_t timeoutMs) {
  if (!_refreshing) {
    return !_timedOut;
  }
  _waitBusy(timeoutMs);
  _lastRefreshMs = millis() - _refreshStartMs;

  if (_powerOffAfter) {
    _writeCommand(0x02);  // POF
    _waitBusy(3000);
    _powered = false;
  }

  _refreshing = false;
  return !_timedOut;
}

void UC8179_EPD::powerOff() {
  _waitBusy(20000);
  _writeCommand(0x02);
  _waitBusy(3000);
  _powered = false;
  _frameLoaded = false;
}

void UC8179_EPD::hibernate() {
  powerOff();
  _writeCommand(0x07);
  _writeData(0xA5);
  delay(2);
  _powered = false;
  digitalWrite(_dc, LOW);
  digitalWrite(_cs, HIGH);
}

// ================================================================ 回读
// 说明：UC8179 的 SDA 是双向线。读的时候先把 MOSI 释放成输入，再用软件
// 时钟逐位读；读完重新 SPI.begin() 把硬件 SPI 接回去。速度不快，但读一次
// 温度/状态只要几毫秒，完全够用，也不挑 MCU。

void UC8179_EPD::_bbWrite(uint8_t v) {
  for (int8_t i = 7; i >= 0; i--) {
    digitalWrite(_mosi, (v >> i) & 1);
    delayMicroseconds(2);
    digitalWrite(_sck, HIGH);
    delayMicroseconds(2);
    digitalWrite(_sck, LOW);
    delayMicroseconds(2);
  }
}

uint8_t UC8179_EPD::_bbRead() {
  uint8_t v = 0;
  for (int8_t i = 7; i >= 0; i--) {
    digitalWrite(_sck, HIGH);
    delayMicroseconds(2);
    v = (uint8_t)((v << 1) | (digitalRead(_readPin) ? 1 : 0));
    digitalWrite(_sck, LOW);
    delayMicroseconds(2);
  }
  return v;
}

bool UC8179_EPD::_readCmd(uint8_t cmd, uint8_t *buf, uint16_t n,
                          uint8_t preDelayMs) {
  if (_readPin < 0 || !buf || n == 0) {
    return false;
  }
  SPI.end();  // 先松开硬件 SPI
  pinMode(_sck, OUTPUT);
  digitalWrite(_sck, LOW);
  pinMode(_cs, OUTPUT);
  digitalWrite(_cs, HIGH);
  pinMode(_dc, OUTPUT);
  digitalWrite(_dc, LOW);
  pinMode(_mosi, OUTPUT);
  digitalWrite(_mosi, HIGH);

  digitalWrite(_cs, LOW);
  _bbWrite(cmd);          // 命令：DC=0
  digitalWrite(_dc, HIGH);  // 数据：DC=1

  if (_readPin != _mosi) {
    pinMode(_mosi, INPUT);  // 两根线时释放 MOSI
  }
  pinMode(_readPin, INPUT);
  delayMicroseconds(10);
  if (preDelayMs) {
    delay(preDelayMs);
  }

  for (uint8_t d = 0; d < _readDummy; d++) {
    (void)_bbRead();  // 校准用：丢弃可能的错位字节
  }
  for (uint16_t i = 0; i < n; i++) {
    buf[i] = _bbRead();
  }

  digitalWrite(_cs, HIGH);
  pinMode(_mosi, OUTPUT);
  digitalWrite(_mosi, HIGH);
#if defined(ARDUINO_ARCH_ESP32)
  SPI.begin(_sck, -1, _mosi, -1);
#else
  SPI.begin();
#endif
  return true;
}

bool UC8179_EPD::_probeRead() {
  // CHIP_REV 固定为 0x0C，用 REV 的最后一个字节判断读时序/字节对齐
  for (uint8_t d = 0; d < 2; d++) {
    _readDummy = d;
    uint8_t rev[7];
    if (_readCmd(0x70, rev, 7) && rev[6] == 0x0C) {
      _readOk = true;
      return true;
    }
  }
  _readDummy = 0;
  return false;
}

bool UC8179_EPD::readRevision(uint8_t out[7]) {
  if (!_readOk) {
    return false;
  }
  return _readCmd(0x70, out, 7);
}

uint32_t UC8179_EPD::productionRevision() {
  uint8_t r[7];
  if (!readRevision(r)) return 0xFFFFFFFFUL;
  return ((uint32_t)r[0] << 16) | ((uint32_t)r[1] << 8) | r[2];
}

uint32_t UC8179_EPD::lutRevision() {
  uint8_t r[7];
  if (!readRevision(r)) return 0xFFFFFFFFUL;
  return ((uint32_t)r[3] << 16) | ((uint32_t)r[4] << 8) | r[5];
}

uint8_t UC8179_EPD::chipRevision() {
  uint8_t r[7];
  if (!readRevision(r)) return 0xFF;
  return r[6];
}

uint8_t UC8179_EPD::readStatus() {
  if (!_readOk) return 0;
  uint8_t v = 0;
  if (_readCmd(0x71, &v, 1)) {
    _lastStatus = v;
  }
  return _lastStatus;
}

int8_t UC8179_EPD::readTemperature() {
  if (!_readOk) return 0;
  uint8_t b[2] = {0, 0};
  if (!_readCmd(0x40, b, 2)) {
    return 0;
  }
  return (int8_t)b[0];
}

int8_t UC8179_EPD::readTemperatureWithOffset() {
  int16_t t = readTemperature();
  int8_t off = (int8_t)(_p.tse & 0x0F);
  if (off >= 8) {
    off -= 16;
  }
  t += off;
  if (t > 127) t = 127;
  if (t < -128) t = -128;
  return (int8_t)t;
}

bool UC8179_EPD::loadOtpProfile() {
  if (!_readOk) {
    return false;
  }
  uint8_t o[0x50];
  if (!readOtp(0x0000, o, sizeof(o))) {
    return false;
  }
  if (o[0] != 0xA5) {
    _otpValid = false;
    _otpProfile = false;
    return false;
  }
  _otpValid = true;

  // 温度分档：0x0B 之后遇到 0x7F 结束
  _tbCount = 11;
  for (uint8_t i = 0; i < 11; i++) {
    _tb[i] = o[1 + i];
    if (o[1 + i] == 0x7F) {
      _tbCount = (uint8_t)(i + 1);
      break;
    }
  }

  // 命令默认值 0x0C~0x1E
  _basePsr = o[0x0D];
  _p.psr = (uint8_t)(_basePsr | UC8179_UD_FLIP_BIT);
  _p.btstA = o[0x0E];
  _p.btstB = o[0x0F];
  _p.btstC1 = o[0x10];
  _p.btstC2 = o[0x11];
  // 数据平面策略：本库全刷用 S1（DDX=10），其余 CDI 位保留 OTP
  _p.cdi1 = (uint8_t)((o[0x13] & (uint8_t)~0x03) | 0x02);
  _p.cdi2 = (uint8_t)(o[0x14] & 0x0F);
  _p.tcon = o[0x15];

  // 分辨率只做核对：帧缓存是编译期固定 648x480
  uint16_t hres = (uint16_t)(o[0x16] * 8);
  uint16_t vres = (uint16_t)(((o[0x17] & 0x03) << 8) | o[0x18]);
  _otpResMatch = (hres == UC8179_PANEL_HRES && vres == UC8179_PANEL_VRES);

  // 扫描起点可以安全采用 OTP 值
  _hst = (int16_t)(o[0x19] * 8);
  _vst = (int16_t)(((o[0x1A] & 0x03) << 8) | o[0x1B]);

  _p.pws = o[0x1C];
  _p.lvsel = (uint8_t)(o[0x1D] & 0x03);

  // TR0 的驱动参数：0x49=帧率/VCEND/VG，0x4A~0x4C=VDH/VDL/VDHR，0x4E=VCOM_DC
  _p.vg = (uint8_t)(o[0x49] & 0x07);
  _p.vdh = (uint8_t)(o[0x4A] & 0x3F);
  _p.vdl = (uint8_t)(o[0x4B] & 0x3F);
  _p.vdhr = (uint8_t)(o[0x4C] & 0x3F);
  _p.vdcs = (uint8_t)(o[0x4E] & 0x7F);

  _otpProfile = true;
  return true;
}

int8_t UC8179_EPD::currentTrGear() {
  if (!_readOk || !_everPowered) {
    return -1;
  }
  int8_t t = readTemperatureWithOffset();
  for (uint8_t i = 0; i < _tbCount && i < 11; i++) {
    if (t <= (int8_t)_tb[i]) {
      return (int8_t)i;
    }
  }
  return (int8_t)(_tbCount - 1);
}

uint8_t UC8179_EPD::readVcom() {
  if (!_readOk) return 0xFF;
  uint8_t v = 0;
  if (!_readCmd(0x81, &v, 1)) {
    return 0xFF;
  }
  return (uint8_t)(v & 0x7F);
}

bool UC8179_EPD::panelCheck() {
  if (!_readOk) return false;
  uint8_t v = 0;
  if (!_readCmd(0x44, &v, 1, 1)) {  // 手册：命令后等 >20us 再读
    return false;
  }
  return (v & 0x01) != 0;
}

bool UC8179_EPD::readOtpBlock(uint8_t *out, uint16_t n) {
  if (!_readOk || !out || n == 0 || n > 0x1800) {
    return false;
  }
  return _readCmd(0xA2, out, n);  // ROTP 从 0x000 顺序读出
}

bool UC8179_EPD::readOtp(uint16_t addr, uint8_t *out, uint8_t n) {
  if (!_readOk || !out || n == 0) {
    return false;
  }
  uint16_t total = (uint16_t)(addr + n);
  if (total > 256) {
    return false;  // 栈缓冲限制；更大范围用 readOtpBlock()
  }
  uint8_t tmp[256];
  if (!_readCmd(0xA2, tmp, total)) {
    return false;
  }
  memcpy(out, tmp + addr, n);
  return true;
}

static void printHexByte(Print &out, uint8_t v) {
  out.print(F("0x"));
  if (v < 0x10) out.print('0');
  out.print(v, HEX);
}

static void printHex2(Print &out, uint8_t v) {
  if (v < 0x10) out.print('0');
  out.print(v, HEX);
}

void UC8179_EPD::printDiagnostics(Print &out) {
  out.print(F("[DIAG] read="));
  out.print(_readOk ? F("ok") : F("off"));
  if (!_readOk) {
    out.println();
    return;
  }
  uint8_t rev[7];
  if (readRevision(rev)) {
    out.print(F(" chip="));
    printHexByte(out, rev[6]);
    out.print(F(" lut="));
    printHexByte(out, rev[3]);
    printHex2(out, rev[4]);
    printHex2(out, rev[5]);
    out.print(F(" prod="));
    printHexByte(out, rev[0]);
    printHex2(out, rev[1]);
    printHex2(out, rev[2]);
  }
  out.print(F(" temp="));
  if (_everPowered) {
    out.print(readTemperature());
    out.print(F("C gear=TR"));
    out.print(currentTrGear());
  } else {
    out.print(F("-- gear=--"));
  }
  out.print(F(" vcom="));
  printHexByte(out, readVcom());
  out.print(F(" FLG="));
  printHexByte(out, readStatus());
  out.print(F(" OTP="));
  out.print(_otpValid ? F("A5") : F("--"));
  out.print(F(" PBC="));
  if (_powered) {
    out.print(panelCheck() ? F("ok") : F("bad"));
  } else {
    out.print(F("n/a"));
  }
  out.println();
}

void UC8179_EPD::printOtpSummary(Print &out) {
  out.print(F("[OTP] "));
  if (!_otpValid) {
    out.println(F("invalid (no 0xA5)"));
    return;
  }
  out.print(_otpProfile ? F("applied") : F("valid"));
  out.print(F(" res="));
  out.print(UC8179_PANEL_HRES);
  out.print('x');
  out.print(UC8179_PANEL_VRES);
  out.print(F(" match="));
  out.print(_otpResMatch ? 1 : 0);
  out.print(F(" HST="));
  out.print(_hst);
  out.print(F(" VST="));
  out.print(_vst);
  out.print(F(" TB="));
  for (uint8_t i = 0; i < _tbCount && i < 11; i++) {
    out.print((int8_t)_tb[i]);
    if (i + 1 < _tbCount) out.print(',');
  }
  out.print(F(" PSR="));
  printHexByte(out, _p.psr);
  out.print(F(" CDI="));
  printHexByte(out, _p.cdi1);
  out.print('/');
  printHexByte(out, _p.cdi2);
  out.print(F(" TCON="));
  printHexByte(out, _p.tcon);
  out.print(F(" VDH="));
  printHexByte(out, _p.vdh);
  out.print(F(" VDL="));
  printHexByte(out, _p.vdl);
  out.print(F(" VCOM="));
  printHexByte(out, _p.vdcs);
  out.println();
}













