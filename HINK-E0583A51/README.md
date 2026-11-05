# UC8179 Arduino 驱动库

适用于 HINK-E0583A51-A0、UC8179、648×480、KW 黑白墨水屏和 ESP32-C3。

## 最终实测结论

- 接线：SCK=6、SDA=7、CS=10、DC=3、RST=4、BUSY=5。
- 像素：`1=黑`、`0=白`，每行81字节，整帧38,880字节；普通图片发送时库自动处理Y方向。
- 生产刷新默认使用 **S1单平面 + DRF**，只发送DTM2，不上传OLD影子帧。
- **TR4=`0x1E`**：快速临时刷新，刷新后保持供电，建议最多连续2次。
- **TR7=`0x7F`**：质量刷新，清除TR4残影，适合作为最终画面和POF收尾。
- `displayManaged()` 自动执行两次TR4、一次TR7。
- TR4后不要直接POF；停止更新、进入长等待或退出前先执行TR7，再POF。
- 局部刷新仅支持已验证用法：S1+TR4、`PT_SCAN=0`、同一固定窗口重复更新，或将多个变化区域合并为一个窗口。S1+TR7局刷会发灰，多个分离窗口依次刷新会使旧窗口变淡。
- 4×4 Bayer高频抖动在复杂图案中会引起图案相关的局部发灰；数字和文字应使用纯黑白图形。

## 硬件连接

| 信号 | ESP32-C3 |
|---|---:|
| SCL/SCK | GPIO6 |
| SDA/MOSI | GPIO7 |
| CS | GPIO10 |
| DC | GPIO3 |
| RST_N | GPIO4 |
| BUSY_N | GPIO5 |
| BS | GND |

库在ESP32上直接管理SPI2，sketch不需要再调用`SPI.begin()`。

## 快速开始

```cpp
#include <UC8179.h>

UC8179 epd;

void setup() {
    Serial.begin(115200);
    epd.setDebug(&Serial);
    epd.begin();

    epd.fillScreen(UC8179::INK_WHITE);
    epd.setTextColor(UC8179::INK_BLACK);
    epd.setTextSize(4);
    epd.setCursor(40, 80);
    epd.print("Hello, e-paper!");
    epd.displayQuality();       // S1 + TR7，完成后POF
}

void loop() {}
```

## 正式刷新接口

### `displayFast()`

使用S1+TR4，只发送DTM2。默认刷新后持续PON；测试刷新并断电时可切换策略。建议最多连续调用两次。

### `displayQuality(bool powerOffAfter=true)`

使用S1+TR7，恢复最佳对比度。默认刷新完成后POF，适合最终画面。

### `displayManaged(bool powerOffAfterQuality=true)`

默认策略下自动执行：

```text
第1次：S1 + TR4，POF
第2次：S1 + TR4，POF
第3次：S1 + TR7，默认POF并清零计数
```

### POF策略

| 调用 | 行为 |
|---|---|
| `displayFast()` | 默认TR4刷新后持续PON；可配置为刷新后立即断电 |
| `displayQuality()` | TR7刷新，默认随后POF |
| `displayQuality(false)` | TR7刷新但保持供电 |
| `powerOff()` | 立即POF，不负责清除TR4残影 |
| `deepSleep()` | POF后进入DSLP；唤醒必须重新`begin()` |

### PON/POF策略配置位置

默认生产策略是 **S1+TR4+每次刷新后持续PON**。在sketch的`setup()`中修改：

```cpp
epd.setFastPowerPolicy(UC8179::FAST_KEEP_POWERED);
// 测试立即POF：epd.setFastPowerPolicy(UC8179::FAST_POF_AFTER_REFRESH);
```

## 局部刷新限制

已验证：

```cpp
epd.setDtmStrategy(UC8179::DTM_S1_SINGLE);
epd.setWaveformMode(UC8179::WAVEFORM_FAST_TR4);
epd.setPartialWindow(x, y, w, h, 0); // PT_SCAN=0
epd.partialIn();
epd.writeNewFrameRaw(data, epd.partialFrameBytes());
epd.refreshByDRF(false);
epd.partialOut();
```

限制：

- 只使用TR4，不使用TR7局刷；
- 默认`PT_SCAN=0`；
- 同一固定窗口可重复更新；
- 多个分离窗口不要依次刷新，否则较早窗口会变淡；
- 多个变化区域应合并成一个包围窗口一次发送；
- 已验证居中320×160窗口和四角寻址；其他尺寸仍需应用侧验证；
- 局刷完成后，最终静止或POF前使用全屏`displayQuality()`恢复。

## S0兼容接口

`DTM_S0_SHADOW`、`writeOldFrame()`和DSP接口仍保留用于诊断及特殊实验。选择S0时才按需分配约38,880字节影子帧；生产接口不使用S0。

S1下不要使用DSP作为刷新入口：S1只发送DTM2，Data_Flag不会满足DSP的双平面条件，应使用DRF。

## 内存

生产S1模式只需当前帧约38,880字节和DMA暂存区。S0影子帧在显式选择S0时才分配。

四屏应用建议后续使用共享SPI、共享帧缓冲、独立CS/BUSY，并行启动各屏DRF；四屏数字时钟不建议使用局部刷新。

## 已验证功能

- 首次上电S1+TR4；
- 首次上电S1+TR7；
- 两次快速刷新后TR7恢复；
- `displayManaged()`连续周期；
- 断电重启恢复；
- 深睡后硬复位恢复；
- REV/FLG重复读取；
- 648×480边界、四角、末行和末字节；
- 复杂图形的基本显示；
- `maintenanceClear()`恢复；
- 局部窗口的上述限定用法。

## 目录

```text
HINK-E0583A51/
├── library.properties
├── README.md
├── src/
│   ├── UC8179.h
│   ├── UC8179.cpp
│   └── uc8179_font5x7.h
└── examples/
    ├── HelloWorld/
    ├── TestPattern/
    ├── SerialTest/
    └── DifferenceTest/
```
