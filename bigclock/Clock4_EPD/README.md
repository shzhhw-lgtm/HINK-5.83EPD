# Clock4_EPD v2 — 四屏时钟

按“单一时基 + 绝对边界 + 只刷变化片”重构。旧的 v1 备份在 `Clock4_EPD.ino.v1.bak`。

## 设计

| 模块 | 做法 |
|---|---|
| 时间 | `UTC 锚点 + esp_timer_get_time()`（64 位单调） |
| 校时 | SNTP 回调重锚，24h 一次；主循环不比对 NTP |
| 调度 | 绝对分钟边界：T-5s 预写，T 触发，错过≤3s 补一次 |
| 刷新 | 只刷数字变化的屏，通常每分钟 1 片 |
| 缓存 | 四片共享 38,880 字节 |
| 波形 | 默认 TR7，`f` 切 TR4 |
| 断电 | 刷完延迟 2s POF |

**旧代码“时间快进、四屏齐刷”的根因**：v1 每轮循环拿 NTP/手动软时钟做秒级追赶和漂移校正，阻塞几秒后一次性追很多秒，连续触发 `minuteRolled` → 反复 `forceRedraw()`。
v2 没有任何“追秒”循环，时间只由锚点 + 单调计时器计算，刷新只由绝对边界触发。

## 接线

| 信号 | GPIO |
|---|---:|
| 共享 SCK | 6 |
| 共享 SDA | 7 |
| 共享 DC | 3 |
| 共享 RST | 4（接 EN 则改 -1） |
| 时十 CS/BUSY | 10 / 2 |
| 时个 CS/BUSY | 0 / 1 |
| 分十 CS/BUSY | 5 / 8 |
| 分个 CS/BUSY | 18 / 19 |

分十/分个用 `setRotation(1)`，等价旧库 `ROTATION_270`。

## 串口

```
T####        手动设时间
W|SSID|密码  存 WiFi
c            深度清理（5 轮黑白）
f            快刷 TR4 / 质量刷 TR7
q            质量刷 TR7
d            状态（时间、target、pending、各片供电）
h            帮助
```

## 日志

```
[SYNC] 12:34        SNTP 重锚且分钟变化
[prep] mask=8       T-5s，只有分个位变化
[fire] mask=8 2870 ms
```

`mask` 的 bit0~3 对应 时十/时个/分十/分个。正常情况下每分钟只出现一个 bit。

## 编译

- Flash：1,281,405 字节（97%）
- RAM：全局 77KB（含共享帧缓存 38.8KB），剩余 250KB
- 4 片不再各占 38.8KB

## 配置

```cpp
#define DEFAULT_QUALITY   1     // 1=TR7, 0=TR4
#define POF_DELAY_MS      2000  // 0=保持 PON
#define PREP_LEAD_SEC     5
#define SYNC_INTERVAL_MS  (86400UL * 1000)
#define BLANK_LEADING_ZERO 0
```

