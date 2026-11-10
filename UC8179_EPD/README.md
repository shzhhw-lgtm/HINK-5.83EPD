# UC8179_EPD —— UC8179 墨水屏 Arduino 驱动库

适用于 **HINK-E0583A51-A0**（5.83 英寸，648×480，黑白，UC8179，4-wire SPI）。
库是照着 **UC8179 数据手册 + 这块屏的板载 OTP 数据**从头写的，不依赖任何第三方显示库，
只用到 Arduino 自带的 `SPI`。已在 ESP32-C3（arduino-esp32 3.3.10）+ arduino-cli 下编译通过。

```
MCU ──SPI──> UC8179 ──> 648×480 电子墨水屏
                ↑
         板载 OTP：波形表 / 电压 / 分辨率
```

---

## 1. 没有资料，为什么也能驱动

UC8179 是通用驱动 IC，手册（`datasheet/UC8179.pdf`）里有完整的指令集；
真正缺的是“这块玻璃怎么接、用什么波形”。这些参数厂家烧在了 **OTP** 里，
你手上的 `HINK-E0583A51-A0_OTP.txt` 就是它。解析结果：

| OTP 地址 | 含义 | 本屏的值 |
|---|---|---|
| 0x0000 | Bank0 校验码 | `0xA5`（已烧写） |
| 0x0001~0x000B | 温度分档 TB0~TB10 | 5 / 10 / 15 / 20 / 30 / 80 / 90 / 127(结束) |
| 0x000C | 命令默认值开关 | `0xA5`（已启用） |
| 0x000D | PSR | `0x17`：LUT 来自 OTP、KW 黑白模式（库默认写 `0x1F`，见下） |
| 0x0013/14 | CDI | `0x10 / 0x07`（双平面 NEW/OLD） |
| 0x0015 | TCON | `0x22` |
| 0x0016~18 | 分辨率 | **648 × 480** |
| 0x0019~1B | 扫描起点 | HST=16、VST=0（**写成 0 会整体左移 16 像素**） |
| 0x001F~0x0048 | Border LUT | 42 字节 |
| 0x0049~0x013F | TR0 波形（≤5°C） | 50Hz，VDH/VDL=±15V，VCOM=-1.00V |
| 0x0140~… | TR1~TR7 | 50/50/50/60/70/50/50Hz，TR6 的 VCOM=-2.00V |
| 0x0801~0x0BDC | TR8~TR11 | 全 `0xFF`（未烧写） |

几条对驱动很关键的结论：

1. **OTP 自带完整波形**，所以 `PSR.REG=0`，刷新时芯片按当前温度自动选 TR0~TR7，
   不需要 MCU 存 LUT，也不怕温度变化。
2. **分辨率 648×480**：一行 81 字节，整帧 `648×480/8 = 38880` 字节。
3. **HST=16**：源极起始位置不是 0，库已按 OTP 写入 `GSST=0x0010`。
4. **驱动电压**：VDH=+15V、VDL=-15V、VCOM_DC=-1.0V，库从 OTP 的 TR0 取值写进 `PWR`。
5. **KW 黑白模式**：没有红色像素，1bpp 就够。
6. **方向修正**：实机点亮发现 OTP 的 `UD=0` 会让画面上下颠倒，所以库默认写入
   `PSR=0x1F`（UD 取反）。想完全照 OTP 原始值，编译加 `-DUC8179_DEFAULT_PSR=0x17`。

> 想自己复核 OTP？用 `extras/uc8179_otp_parser.py`（见第 10 节）。

---

## 2. 接线

### 单屏（先点亮一片时）

| 屏引脚 | ESP32-C3 | 说明 |
|---|---:|---|
| CS | GPIO10 | 片选，低有效 |
| DC | GPIO3 | 命令/数据 |
| BUSY | GPIO2 | **高=空闲**，低=忙 |
| SCK | GPIO6 | SPI 时钟 |
| SDA | GPIO7 | SPI MOSI |
| RST | — | 若接板载 RESET/EN，构造时传 `-1` |
| BS | GND | 固定 4-wire SPI |
| VCC | 3.3V | 不要接 5V |
| GND | GND | 必须和 MCU 共地 |

**可选回读**：把 SDA 再接到一个可输入引脚（如 GPIO5），`begin(6, 5, 7, ...)` 的
`miso` 传 5；若 SDA 只有一根线，就传 `begin(6, 7, 7, ...)`。不接回读也能正常显示。

`RST` 的两种接法：
- **接到板载 RESET/EN**：MCU 一复位屏也复位，但固件**不能**拉低它（会把自己复位）。
  构造时 `rst = -1`，库改用 `PSR.RST_N=0` 软复位。
- **接到普通 GPIO**（如 GPIO4）：可以硬复位；多片共享 RST 时，只有第一片
  `begin(..., doReset=true)`，其余传 `false`。

### 四屏（bigclock 目标形态）

`SCK / SDA / DC` 共用，`CS / BUSY` 每片独立：

| 屏 | CS | BUSY |
|---|---:|---:|
| 时十 | GPIO10 | GPIO2 |
| 时个 | GPIO0 | GPIO1 |
| 分十 | GPIO5 | GPIO8 |
| 分个 | GPIO18 | GPIO19 |

共用的 `SCK=6, SDA=7, DC=3`。四片 RST 若都接板载 RESET，则全部传 `-1`。

---

## 3. 安装

1. 把整个 `UC8179_EPD` 文件夹复制到 `文档/Arduino/libraries/`
   （或 Arduino IDE：**项目 → 加载库 → 添加 .ZIP 库**，先压成 zip）。
2. 开发板选择 **ESP32C3 Dev Module**（esp32 core 3.x）。
3. 打开示例 `文件 → 示例 → UC8179 E-Paper → 01_HelloWorld`。

库没有第三方依赖；`architectures=*`，但只有 ESP32 的硬件 SPI 路径做过编译验证，
AVR 等平台会走逐字节发送（且 38KB 帧缓存放不下 Uno）。

---

## 4. 第一个程序

```cpp
#include <UC8179_EPD.h>

#define PIN_CS 10
#define PIN_DC 3
#define PIN_BUSY 2
#define PIN_SCK 6
#define PIN_MOSI 7
#define PIN_RST -1   // 接板载 RESET 时写 -1

UC8179_EPD epd(PIN_CS, PIN_DC, PIN_RST, PIN_BUSY);

void setup() {
  Serial.begin(115200);
  if (!epd.begin(PIN_SCK, -1, PIN_MOSI, 4000000)) {
    Serial.println("初始化失败");
    while (true) delay(1000);
  }
  epd.fillScreen(UC8179_WHITE);
  epd.drawString(40, 60, "Hello, e-paper!", UC8179_BLACK, 3);
  epd.drawRect(10, 10, epd.width() - 20, epd.height() - 20, UC8179_BLACK);
  epd.display();          // 上电 → 送 38880 字节 → DRF → 等 BUSY → POF
}

void loop() {}
```

`begin()` 参数：

```cpp
bool begin(int8_t sck = -1, int8_t miso = -1, int8_t mosi = -1,
           uint32_t spiHz = 4000000, bool doReset = true);
```

- `spiHz`：手册上限 10MHz，默认 4MHz 更稳（排线长、4 片并联时尤其）。
- `doReset`：RST 共享时只有第一片传 `true`。
- 返回 `false` 只表示帧缓存分配失败。

---

## 5. 上机调试对照表（重要）

这块屏的 **方向/极性没有公开资料**，库把相关开关都做成了运行时可调。
先跑 `01_HelloWorld`，串口按单键，看着屏调：

| 现象 | 处理 |
|---|---|
| 白底变成黑底、黑字变白 | `epd.setInverted(true)`（例程按 `i`） |
| 上下颠倒 | 默认已修正（PSR 的 UD 取反）；若仍颠倒，按 `y` 再翻一次 |
| 左右镜像（字是反的） | `epd.setMirror(true, false)`（按 `x`） |
| 竖装（480×648） | `epd.setRotation(1)` 或 `3`（按 `r`） |
| 整幅图左边被切/右移 16 像素 | 检查 `GSST`：本屏 HST=16，不能写 0（库已处理） |
| 刷新一直超时 | BUSY 接错、没共地，或 RST 接在 EN 上却传了 GPIO 号 |
| 画面很淡/发灰 | 按 `q` 用 TR7 高质量波形；还淡就试 `epd.setVcomDc(0x12)`（-1.0V，OTP TR0 的值） |
| 残影明显 | 先按 `q` 跑一次 TR7；日常用 `REFRESH_QUALITY` |
| 单/双平面对比 | 按 `s` 切换；双平面会多发一帧全白 OLD，用于 A/B 对比 |
| 全黑/全白测试 | 按 `b` / `w`，先确认极性再调图形 |

调好后把这几个值写死进 `setup()`：

```cpp
epd.setRotation(1);
epd.setMirror(false, false);
epd.setInverted(false);
epd.setRefreshMode(UC8179_EPD::REFRESH_QUALITY);
```

> 说明：默认值取 `1=黑 / 0=白`、`PSR=0x1F`（UD 已按实测翻转）、单平面。
> 若你的屏批次不同，上面的按键组合一定能调出正确画面，不需要改库。

---

## 6. API 速查

### 几何 / 方向

| 方法 | 说明 |
|---|---|
| `width()` / `height()` | 当前逻辑画布尺寸（随旋转变化） |
| `setRotation(0..3)` | 0=648×480，1/3=480×648，2=倒置 |
| `setMirror(x, y)` | 硬件镜像：改 PSR 的 SHL/UD，不耗 CPU |
| `setInverted(bool)` | 整屏取反 |
| `buffer()` / `setBuffer(p)` | 取/换帧缓存（`setBuffer` 可多片共用） |
| `bufferSize()` | 固定 38880 |

### 绘图

| 方法 | 说明 |
|---|---|
| `fillScreen(color)` | 整屏 |
| `drawPixel / drawLine / drawRect / fillRect / drawCircle / fillCircle` | 基本图形 |
| `drawFastHLine / drawFastVLine` | 快速横/竖线 |
| `drawBitmap(x,y,bmp,w,h,color)` | 1bpp、行优先、MSB 在左、bit=1 画 `color` |
| `drawChar / drawString / drawStringf` | 内置 7×8 点阵，`size` 整数放大 |
| `drawStringf(x,y,color,size,fmt,...)` | 带格式化的文字（底层 `vsnprintf`） |

颜色常量：`UC8179_BLACK`（=1）、`UC8179_WHITE`（=0），与面板 SRAM 位一致。

### 刷新

| 方法 | 说明 |
|---|---|
| `display()` | 阻塞整帧刷新：PON → 数据 → DRF → 等 BUSY（默认保持 PON，不 POF） |
| `display(nativeImage)` | 直接刷 flash 里的 38880 字节整帧（不经过缓存、不旋转） |
| `startRefresh()` | PON + 送数 + DRF，立即返回（多屏流水线用） |\n| `writeFrame()` | 只 PON + 送数，不触发 DRF（:55 预写用） |\n| `triggerRefresh()` | 只发 DRF（配合 writeFrame 做 :00 齐刷） |
| `waitRefresh(timeout)` | 等本屏 BUSY 结束并 POF，返回 `false` 表示超时 |
| `powerOff()` / `hibernate()` | 断电 / 断电+DSLP |
| `isBusy()` | 读 BUSY 引脚 |

### 波形 / 面板调节（调试用）

完整清单和取值表见 [PARAMETERS.md](PARAMETERS.md)，调参思路见 [TUNING.md](TUNING.md)，这里只列常用项：

| 方法 | 说明 |
|---|---|
| `setWaveformGear(0..7)` | 直接选 OTP 的 TR0~TR7 |
| `setRefreshMode(mode)` | `REFRESH_AUTO` / `FAST`(TR4) / `QUALITY`(TR7) |
| `setTemperatureForce(code)` | 原始 `TSFIX=1 + TSSET=code` |
| `setTemperatureAuto()` | 回到 OTP 自动分档 |
| `setTemperatureOffset(-8..7)` | 平移自动分档（不改波形，只骗温度） |
| `setVcomDc(code)` / `clearVcomDc()` | 写/不写 `VDCS`（0x12 = -1.00V） |
| `setSourceVoltage(vdh, vdl)` | 0..63 → 2.4..15.0V |
| `setGateVoltage(0..7)` | VGH/VGL = 9..20V |
| `setFrameRateCode(0..15)` | 5..200Hz |
| `setCdiInterval(0..15)` / `setBorderLut(0..3)` / `setTcon()` | 时序/边框 |
| `setBoosterSoftStart()` / `setPowerSaving()` / `setLvsel()` | 电源相关 |
| `setSinglePlane(bool)` | 单平面(0x12) / 双平面(0x10) |\n| `setPowerOffAfterRefresh(bool)` | 刷新后是否 POF：默认 false（保持 PON，白底更干净） |
| `enableShadow(bool)` / `invalidateShadow()` | 影子帧真差分（额外 38880 字节，H 命令） |
| `printConfig(Serial)` | 一行打印当前全部参数，便于记录 |
| `lastRefreshMs()` / `lastDisplayMs()` | 上次 DRF 耗时 / 整次显示耗时 |

TR 档位与温度的对应：`≤5°C → TR0`，`≤10 → TR1`，`≤15 → TR2`，`≤20 → TR3`，
`≤30 → TR4`，`≤80 → TR5`，`≤90 → TR6`，`≤127 → TR7`。

### 回读 / 自检（可选）

把 SDA 接到一个可输入引脚，并把该引脚号传给 `begin()` 的 `miso` 参数即可启用：

```cpp
epd.begin(6, 7, 7, 4000000);   // SDA=7 同时作为回读引脚（3-wire 单线）
epd.begin(6, 5, 7, 4000000);   // 若另有一根 MISO=5 接到 SDA
```

库会用软件时钟逐位读（读前 `SPI.end()`、读完 `SPI.begin()` 还原），并用 REV 的
`CHIP_REV=0x0C` 自动校准字节对齐；不接回读则自动退化为只写。

| 方法 | 说明 |
|---|---|
| `readEnabled()` / `readPin()` / `setReadPin(pin)` | 回读是否可用 / 当前引脚 / 运行时换引脚重校准 |
| `readRevision(rev[7])` / `chipRevision()` / `lutRevision()` | REV 0x70 |
| `readStatus()` | FLG 0x71（`UC8179_FLG_*`） |
| `readTemperature()` / `readTemperatureWithOffset()` | TSC 0x40，内部传感器 °C |
| `currentTrGear()` | 按温度+偏移推算当前会用的 TR0~TR7 |
| `readVcom()` | VV 0x81，7 位码 |
| `panelCheck()` | PBC 0x44，面板是否完好 |
| `readOtp(addr, buf, n)` / `readOtpBlock(buf, n)` | ROTP 0xA2（从 0x000 顺序读） |
| `loadOtpProfile()` / `otpProfileApplied()` | 读 OTP 0x00~0x4F 并套用面板参数 |
| `printOtpSummary(Serial)` | 打印 OTP 解析出的分辨率/温度分档/PSR/CDI/电压 |
| `otpValid()` / `frameVerified()` | OTP 校验码 / 整帧 Data_Flag |
| `printDiagnostics(Serial)` | 一行打印芯片版本、温度、档位、VCOM、FLG、OTP、PBC |

`begin()` 在回读可用时会自动读 OTP 0x00~0x4F，并用 OTP 里的
PSR/CDI/TCON/BTST/PWS/LVSEL/HST/VST/TR0 电压覆盖编译期 profile；分辨率只做核对
（`otpResolutionMatch()`），因为帧缓存是编译期固定的。`miso=-1` 时完全退化为只写、
用编译期 profile。

`_sendFrame()` 在回读可用时默认会读一次 FLG，确认 `Data_Flag` 置位后再发 DRF；
可用 `setVerifyFrame(false)` 关掉。

注意：温度/档位要至少上电（PON）一次后才有效，未上电时诊断显示 `n/a`；
`readVcom()` 读的是 `VDCS` 寄存器值，不是 OTP TR 里用于波形的 VCOM。

---

## 7. 四屏时钟（`02_FourPanelClock`）

四片屏共用 SPI，关键有两点：

**① 共用一块帧缓存**

```cpp
static uint8_t frame[UC8179_EPD::bufferSize()];   // 38.8KB

for (i = 0; i < 4; i++) {
  panels[i]->setBuffer(frame);    // 必须在 begin 前绑定
  panels[i]->begin(PIN_SCK, -1, PIN_MOSI, 4000000, i == 0);
  panels[i]->setRotation(ROTATION);
}
```

画一片 → `startRefresh()`（数据已经时钟出去，缓存可以马上复用）→ 画下一片。
4 片只占 **1 份** 缓存，而不是 155KB。

**② 刷新流水线**

```cpp
for (i = 0; i < 4; i++) {
  panels[i]->fillScreen(UC8179_WHITE);
  drawDigit(*panels[i], digits[i]);
  panels[i]->startRefresh();      // PON + 送数 + DRF，立即返回
}
for (i = 0; i < 4; i++) {
  panels[i]->waitRefresh(20000);  // 四片并行刷，总时间 ≈ 一片
}
```

因为 DRF 之后芯片自己刷新，SPI 总线空出来可以继续给下一片上电/送数；
四片最后一起等 BUSY，肉眼看就是“同时翻页”。

实测参考（4MHz、TR7）：单片 PON ≈ 190ms、送 38880 字节 ≈ 80ms、
TR7 刷新 ≈ 2.9s；四片流水线一轮约 4s，剩下 56 秒全断电。

---

## 8. 用你自己的 TTF 做大数字

`tools/make_digit_font_gui.py` 生成的 `digit_font.h` 格式正好是
`drawBitmap()` 要的：行优先、MSB 在左、bit=1 为笔画。

```cpp
#include "digit_font.h"   // DIGITS[10][DIGIT_ROW_BYTES*DIGIT_H]

epd.fillScreen(UC8179_WHITE);
epd.drawBitmap(0, 0, DIGITS[digit], DIGIT_W, DIGIT_H, UC8179_BLACK);
epd.display();
```

- `DIGIT_W` 必须是 8 的倍数（工具已补齐）。
- 如果字模是“竖屏”方向，先 `setRotation(1/3)` 再画，坐标会自动旋转。
- 想省 RAM：字模在 flash 里，直接 `epd.display(DIGITS[d])` 可以完全不占帧缓存
  （前提是整帧 648×480、按控制器顺序存放，且不旋转）。

---

## 9. 性能与已知限制

| 项目 | 数值 |
|---|---|
| 帧缓存 | 38,880 字节（1bpp） |
| 单帧 SPI 传输 @4MHz | ≈ 78ms |
| 4MHz 下单字节开销 | 2µs |
| TR7 刷新 | ≈ 2.9s（随温度档变化） |
| 四片一轮 | ≈ 4s |

限制：

- **没有局部刷新**：UC8179 支持 PTL/PTIN/PTOUT，但 OTP 波形没有专门的局刷 LUT，
  这块屏上直接局刷容易发灰。当前版本只做全刷。
- **回读是可选的**：SDA 单向时库自动退化为只写；把 SDA 同时接到一个可输入引脚
  （传给 `begin()` 的 `miso` 参数）就能读 REV/FLG/温度/VCOM/OTP/PBC。详见第 6 节。
- **深睡唤醒需要硬复位**：`hibernate()` 后只能靠 RST 下降沿唤醒；
  如果 RST 接的是板载 EN，就只能断电重启。
- **默认参数需要一次上机确认**：方向/极性用第 5 节的按键调一次即可。

---

## 10. OTP 解析工具

```bash
python extras/uc8179_otp_parser.py extras/HINK-E0583A51-A0_OTP.txt
```

输出校验码、温度分档、命令默认值、12 组 TR 波形（帧率/VDH/VDL/VDHR/VCOM/VGH），
还能直接生成面板 profile：

```bash
python extras/uc8179_otp_parser.py extras/HINK-E0583A51-A0_OTP.txt \
       --header src/UC8179_PanelProfile.h --name HINK-E0583A51-A0
```

换一块 UC8179 屏时，只要重新读 OTP、重新生成 `UC8179_PanelProfile.h`，
库的主体不用改。

---

## 11. 目录

```
UC8179_EPD/
├── library.properties
├── keywords.txt
├── README.md
├── src/
│   ├── UC8179_EPD.h/.cpp       驱动核心
│   ├── UC8179_PanelProfile.h   面板参数（由 OTP 生成，可重生成）
│   └── UC8179_Font7x8.h        7×8 ASCII 点阵
└── examples/
    ├── 01_HelloWorld/          点亮 + 串口调试面板
    ├── 02_FourPanelClock/      四屏 HH:MM 时钟（共享缓存+流水线刷新）
    ├── 03_BitmapDemo/          drawBitmap / PROGMEM 位图
    └── 04_ParamLab/            可控性探测 + TR/电压/时序对比实验台
```

另有 [PARAMETERS.md](PARAMETERS.md)：UC8179 全部可调参数总表、TR 明细、测试顺序和记录模板；
[TUNING.md](TUNING.md)：从 OTP 出发的调参方案（可控性探测、基线配置、TR 选择、测试矩阵）；\n[REFERENCE.md](REFERENCE.md)：与佳显 GDEY0583T81、微雪 EPD_5in83_V2、GxEPD2 的对比与可 A/B 的移植方案。






