/*
 * UC8179_EPD — UC8179 墨水屏 Arduino 驱动库（HINK-E0583A51-A0 / 648x480）
 * ==================================================================
 * 设计依据：UC8179 数据手册（Rev 0.6）+ 本屏板载 OTP 数据
 *   - OTP 已烧好波形/温度分档/命令默认值，默认 PSR.REG=0 直接用 OTP LUT
 *   - 1bpp，1=黑、0=白；每行 81 字节，整帧 38880 字节
 *
 * 除了正常显示，库把寄存器层面可调的项都暴露成了 setter（见 PanelParams），
 * 方便逐项做 A/B 对比：TR 波形档、VCOM、VDH/VDL/VDHR、VGH/VGL、帧率、
 * CDI、TCON、EVS、BTST、PWS、LVSEL、温度偏移、单/双平面等。
 */
#ifndef UC8179_EPD_H
#define UC8179_EPD_H

#include <Arduino.h>
#include <SPI.h>

#include "UC8179_PanelProfile.h"
#include "UC8179_Font7x8.h"

// 面板像素极性：1 = 黑，0 = 白
#define UC8179_BLACK 1
#define UC8179_WHITE 0

// FLG (0x71) 位定义（依据手册 + 实测默认值 0x13/0x15/0x1D 推断）
#define UC8179_FLG_BUSY 0x01  // BUSY_N 引脚电平，1=空闲
#define UC8179_FLG_POF 0x02   // 掉电状态
#define UC8179_FLG_PON 0x04   // 上电状态
#define UC8179_FLG_DATA 0x08  // Data_Flag：整帧数据已收齐

#ifndef UC8179_SPI_DEFAULT_HZ
#define UC8179_SPI_DEFAULT_HZ 4000000UL
#endif

/*
 * 默认 PSR。OTP 里是 0x17（UD=0），但实机点亮发现画面上下颠倒，
 * 这里默认把 UD 取反（bit3）修正为 0x1F。
 * 想完全照 OTP 原始值，编译时加 -DUC8179_UD_FLIP=0。
 */
#ifndef UC8179_UD_FLIP
#define UC8179_UD_FLIP 1
#endif
#if UC8179_UD_FLIP
#define UC8179_UD_FLIP_BIT 0x08
#else
#define UC8179_UD_FLIP_BIT 0x00
#endif

#ifndef UC8179_DEFAULT_PSR
#define UC8179_DEFAULT_PSR (UC8179_PROFILE_PSR | UC8179_UD_FLIP_BIT)
#endif

#if defined(__AVR__)
#define UC8179_PGM_READ(p) pgm_read_byte(p)
#else
#define UC8179_PGM_READ(p) (*(const uint8_t *)(p))
#endif

// 当前生效的全部寄存器参数（可直接读出来做对比记录）
struct UC8179_Params {
  uint8_t psr;    // 0x00: REG, KW/R, UD, SHL, SHD_N, RST_N
  uint8_t vg;     // 0x01: VG_LVL 0..7 -> 9/10/11/12/17/18/19/20V
  uint8_t vdh;    // 0x01: VDH 0..63 -> 2.4..15.0V
  uint8_t vdl;    // 0x01: VDL 0..63 -> -2.4..-15.0V
  uint8_t vdhr;   // 0x01: VDHR 0..63 -> 2.4..15.0V（KW 模式基本不用）
  uint8_t btstA, btstB, btstC1, btstC2;  // 0x06
  uint8_t frs;    // 0x30: 0..15 -> 5..200Hz
  uint8_t cdi1;   // 0x50: BDZ, BDV, N2OCP, DDX
  uint8_t cdi2;   // 0x50: CDI 间隔 2..17 hsync
  uint8_t tcon;   // 0x60: S2G/G2S 非交叠
  uint8_t evs;    // 0x52: VCEND, BDEND
  uint8_t vdcs;   // 0x82: VCOM_DC
  uint8_t pws;    // 0xE3: VCOM_W, SD_W
  uint8_t lvsel;  // 0xE4: LVD_SEL
  uint8_t tse;    // 0x41: bit7 TSE, bit3:0 TO 偏移
  uint8_t tsfix;  // 0xE0: 1=温度用 TSSET 强制
  uint8_t tsset;  // 0xE5: 强制温度值
};

class UC8179_EPD {
 public:
  // 刷新波形档位（便捷接口，本质是设置 TSFIX/TSSET）
  enum RefreshMode : uint8_t {
    REFRESH_AUTO = 0,     // 用 OTP 温度分档自动选 TR0~TR7（默认）
    REFRESH_FAST = 1,     // 强制 TR4（0x1E）
    REFRESH_QUALITY = 2,  // 强制 TR7（0x7F）
    REFRESH_CUSTOM = 3,   // 通过 setWaveformGear()/setTemperatureForce() 指定
  };

  UC8179_EPD(int16_t cs, int16_t dc, int16_t rst, int16_t busy);
  ~UC8179_EPD();

  bool begin(int8_t sck = -1, int8_t miso = -1, int8_t mosi = -1,
             uint32_t spiHz = UC8179_SPI_DEFAULT_HZ, bool doReset = true);

  bool ready() const { return _buffer != nullptr; }

  // ---------------- 几何 / 方向 ----------------
  int16_t width() const { return _width; }
  int16_t height() const { return _height; }
  void setRotation(uint8_t r);
  uint8_t getRotation() const { return _rotation; }
  void setMirror(bool mirrorX, bool mirrorY);
  void setInverted(bool inv) { _invert = inv; }
  bool inverted() const { return _invert; }

  // ---------------- 帧缓存 ----------------
  uint8_t *buffer() { return _buffer; }
  static constexpr size_t bufferSize() {
    return (size_t)UC8179_PANEL_WIDTH * UC8179_PANEL_HEIGHT / 8;
  }
  bool setBuffer(uint8_t *buf);

  // ---------------- 绘图 ----------------
  void fillScreen(uint16_t color);
  void drawPixel(int16_t x, int16_t y, uint16_t color);
  void drawLine(int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint16_t color);
  void drawFastHLine(int16_t x, int16_t y, int16_t w, uint16_t color);
  void drawFastVLine(int16_t x, int16_t y, int16_t h, uint16_t color);
  void drawRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color);
  void fillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color);
  void drawCircle(int16_t x0, int16_t y0, int16_t r, uint16_t color);
  void fillCircle(int16_t x0, int16_t y0, int16_t r, uint16_t color);
  void drawBitmap(int16_t x, int16_t y, const uint8_t *bitmap, int16_t w,
                  int16_t h, uint16_t color = UC8179_BLACK);
  int16_t drawChar(int16_t x, int16_t y, char c, uint16_t color, uint8_t size);
  int16_t drawString(int16_t x, int16_t y, const char *s,
                     uint16_t color = UC8179_BLACK, uint8_t size = 1);
  void drawStringf(int16_t x, int16_t y, uint16_t color, uint8_t size,
                   const char *fmt, ...);

  // ---------------- 刷新 ----------------
  bool display();
  bool display(const uint8_t *nativeImage);
  void startRefresh();
  // 预写帧（PON + 数据，不触发 DRF），配合 triggerRefresh() 做多屏流水线
  void writeFrame();
  void triggerRefresh();
  bool waitRefresh(uint32_t timeoutMs = 20000UL);
  void powerOff();
  void hibernate();
  bool isBusy() const;
  bool timedOut() const { return _timedOut; }
  bool isPowered() const { return _powered; }
  uint32_t lastRefreshMs() const { return _lastRefreshMs; }
  uint32_t lastDisplayMs() const { return _lastDisplayMs; }

  // ---------------- 读取 / 自检（需要 SDA 可回读） ----------------
  // 接线：begin() 的 miso 参数填“能读回 SDA 的引脚”。
  //   - SDA 与 MISO 是两根线：miso = MISO 引脚
  //   - SDA 只有一根线（3-wire / MOSI=MISO）：miso = mosi 的引脚号
  //   - 不接回读：miso = -1，库退化为只写
  bool readEnabled() const { return _readOk; }
  int8_t readPin() const { return (int8_t)_readPin; }
  bool setReadPin(int8_t pin);  // 运行时换回读引脚并重新校准（调试用）
  bool readRevision(uint8_t out[7]);   // 0x70: PROD[3], LUT[3], CHIP
  uint32_t productionRevision();
  uint32_t lutRevision();
  uint8_t chipRevision();
  uint8_t readStatus();                // 0x71 FLG
  int8_t readTemperature();            // 0x40 TSC，内部传感器，°C
  int8_t readTemperatureWithOffset();  // 叠加 TSE 的 TO 偏移
  int8_t currentTrGear();              // 由温度推算当前会用 TR0~TR7，失败 -1
  uint8_t readVcom();                  // 0x81 VV，7 位码
  bool panelCheck();                   // 0x44 PBC，true=面板完好
  bool readOtp(uint16_t addr, uint8_t *out, uint8_t n);  // addr+n <= 256
  bool readOtpBlock(uint8_t *out, uint16_t n);           // 从 0x000 连续读
  bool otpValid() const { return _otpValid; }
  bool otpProfileApplied() const { return _otpProfile; }
  bool otpResolutionMatch() const { return _otpResMatch; }
  bool loadOtpProfile();               // 读 OTP 0x00~0x4F 并套用面板参数
  void printOtpSummary(Print &out);    // 打印 OTP 解析出的关键参数
  bool frameVerified() const { return _frameVerified; }
  void setVerifyFrame(bool on) { _verifyFrame = on; }
  void printDiagnostics(Print &out);

  // ---------------- 参数读取 ----------------
  const UC8179_Params &params() const { return _p; }
  void printConfig(Print &out);
  void resetParams();  // 恢复成 OTP/实测默认值

  // ---------------- 波形档 TR ----------------
  // gear 0..7 对应 OTP 的 TR0..TR7（≤5/10/15/20/30/80/90/127°C）
  void setWaveformGear(uint8_t gear);
  void setTemperatureForce(uint8_t tempCode);  // 原始 TSSET 值
  void setTemperatureAuto();                   // 关闭 TSFIX
  void setTemperatureOffset(int8_t deg);       // -8..+7，平移自动分档
  void setRefreshMode(RefreshMode mode);
  RefreshMode refreshMode() const { return _refreshMode; }

  // ---------------- 电压 ----------------
  void setVcomDc(uint8_t code);  // 0x00..0x4F -> -0.1..-4.05V
  // 刷新后是否 POF（厂商例程是保持 PON，直到 deep sleep 才 POF）
  // 默认保持 PON（厂商做法）：每次刷新后 POF 会让白底/边框发灰
  void setPowerOffAfterRefresh(bool on) { _powerOffAfter = on; }
  bool powerOffAfterRefresh() const { return _powerOffAfter; }
  void clearVcomDc();            // 不再写 VDCS，交回 OTP
  void setSourceVoltage(uint8_t vdh, uint8_t vdl);  // 0..63
  void setVdhr(uint8_t code);
  void setGateVoltage(uint8_t vg);  // 0..7

  // ---------------- 时序 / 其它 ----------------
  void setFrameRateCode(uint8_t frs);  // 0..15
  void setCdiInterval(uint8_t cdi);    // 0..15
  void setCdiRegister(uint8_t cdi1, uint8_t cdi2);
  void setBorderLut(uint8_t bdv);  // 0..3
  void setBorderHiZ(bool on);
  void setN2Ocp(bool on);
  void setTcon(uint8_t tcon);
  void setBorderEnd(uint8_t evs);
  void setBoosterSoftStart(uint8_t a, uint8_t b, uint8_t c1, uint8_t c2);
  void setPowerSaving(uint8_t pws);
  void setLvsel(uint8_t lvsel);
  void setSinglePlane(bool on);  // true: CDI=0x12 只发 NEW；false: 双平面
  // 双平面时 OLD 帧内容：false=全白，true=与 NEW 相同（GxEPD2 厂商序列）
  void setOldFrameSame(bool on) { _oldSame = on; }
  bool oldFrameSame() const { return _oldSame; }
  // 影子帧：OLD = 上一帧实际内容，做真差分（需要额外 38880 字节）
  bool enableShadow(bool on);
  bool shadowEnabled() const { return _shadow != nullptr; }
  void invalidateShadow() { _shadowValid = false; }
  bool singlePlane() const;

  // ---------------- 复位 ----------------
  void hardwareReset();
  void softReset();

 private:
  void _loadDefaults();
  void _config();
  void _writePsr();
  void _writePwr();
  void _writeBtst();
  void _writePll();
  void _writeCdi();
  void _writeTcon();
  void _writeTres();
  void _writeGsst();
  void _writeEvs();
  void _writeVdcs();
  void _writePws();
  void _writeLvsel();
  void _writeTse();
  void _writeTemp();
  void _powerOn();
  void _writeCommand(uint8_t cmd);
  void _writeData(uint8_t data);
  void _writeData(const uint8_t *data, size_t len);
  void _writeDataMasked(const uint8_t *data, size_t len, uint8_t mask);
  void _writeRepeat(uint8_t value, size_t len);
  void _waitBusy(uint32_t timeoutMs);
  void _putPixel(int16_t px, int16_t py, bool black);
  void _fillRowNative(int16_t x0, int16_t x1, int16_t y, bool black);
  bool _sendFrame(const uint8_t *image);
  // 位翻转读（读之前会 SPI.end()，读完 SPI.begin() 还原）
  bool _readCmd(uint8_t cmd, uint8_t *buf, uint16_t n, uint8_t preDelayMs = 0);
  void _bbWrite(uint8_t v);
  uint8_t _bbRead();
  bool _probeRead();

  int16_t _cs, _dc, _rst, _busy;
  int16_t _sck, _mosi, _readPin;
  uint32_t _spiHz;
  SPISettings _spiSettings;
  uint8_t _rotation;
  int16_t _width, _height;
  uint8_t *_buffer;
  bool _ownBuffer;
  bool _invert;
  bool _refreshing;
  bool _timedOut;
  bool _powered;
  bool _everPowered;
  bool _vdcsValid;
  bool _powerOffAfter;
  bool _oldSame;
  uint8_t *_shadow;
  bool _ownShadow;
  bool _shadowValid;
  bool _readOk;
  bool _readProbed;
  uint8_t _readDummy;
  bool _otpValid;
  bool _otpProfile;
  bool _otpResMatch;
  bool _verifyFrame;
  bool _frameVerified;
  bool _frameLoaded;
  uint8_t _lastStatus;
  uint8_t _basePsr;
  uint8_t _tb[11];
  uint8_t _tbCount;
  int16_t _hst, _vst;
  uint32_t _refreshStartMs;
  uint32_t _lastRefreshMs;
  uint32_t _lastDisplayMs;
  UC8179_Params _p;
  RefreshMode _refreshMode;

  static bool _spiBegun;
};

#endif  // UC8179_EPD_H







