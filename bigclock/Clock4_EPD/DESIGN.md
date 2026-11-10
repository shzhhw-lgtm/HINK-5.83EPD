# 四屏时钟逻辑设计（从零版）

> 不考虑现有实现，只按“4 片 UC8179 + ESP32-C3 + WiFi/NTP”这个硬件前提重新设计。

## 0. 设计目标

1. 每分钟只更新真正变化的数字，不整屏齐刷；
2. 24 小时校时一次，其余时间单一时基走时；
3. 任何网络抖动、SNTP 微调、循环阻塞都不能引发重复刷新；
4. 长期运行不漂移、不积残影、不挂死；
5. 内存留给 WiFi，不搞 4 份全屏缓存。

---

## 1. 时间：单一时基，而不是“每轮读 NTP 比对”

核心是一个**锚点**：

```cpp
int64_t anchorUtc;   // 锚点时刻的 UTC 秒
int64_t anchorUs;    // 锚点时刻的 esp_timer_get_time()

int64_t nowUtc() {
    return anchorUtc + (esp_timer_get_time() - anchorUs) / 1000000;
}
```

- `esp_timer_get_time()` 是 64 位微秒计数，不存在 `millis()` 49 天回绕；
- SNTP 只在**回调**里触发一次“重新锚定”，主循环从不拿 NTP 时间做逐秒校正；
- SNTP 的秒级抖动被吸收在锚点里，不会变成“时间跳变 → 强制重绘”。

```cpp
void onSntpSync() { syncFlag = true; }   // 回调只置标志

void reAnchor() {
    time_t sys; time(&sys);
    int64_t oldMinute = nowUtc() / 60;
    anchorUtc = (int64_t)sys;
    anchorUs  = esp_timer_get_time();
    if (oldMinute != anchorUtc / 60) {
        forceShowCurrentMinute();   // 只有“分钟真的变了”才补一次
    }
    target = (anchorUtc / 60 + 1) * 60;
    prepped = false;
}
```

时区用 `localtime_r()`，锚点存 UTC；中国无夏令时，但逻辑上支持 DST。

---

## 2. 调度：绝对分钟边界 + 状态机

不要“检测到分钟变了再刷”，而是**提前知道下一个边界**：

```cpp
int64_t target;      // 下一个整分钟的 UTC 秒
bool prepped;

void loop() {
    if (syncFlag) { syncFlag = false; reAnchor(); }
    int64_t now = nowUtc();
    if (!target) target = (now / 60 + 1) * 60;

    if (!prepped && now >= target - 5) prep(target);   // T-5s 预写
    if (now >= target) {
        if (now - target > 3) showCurrentMinute();     // 被阻塞，错过边界
        else                  fire();                  // 正常齐刷
        prepped = false;
        target = (now / 60 + 1) * 60;                  // 重新算，不 +=60
    }
    servicePof();
}
```

关键点：

- `target` 每次都从当前 UTC 重新算，**不会累积漂移**；
- 循环被刷新/WiFi 阻塞几秒，最多触发一次“补刷”，不会追赶式连刷；
- `reAnchor()` 只在分钟变化时补一次，秒级调整完全不碰屏幕。

---

## 3. 刷新：只刷变化的那一片

这是和“四屏齐刷”最大的区别。

```cpp
uint8_t shown[4] = {0xFF,0xFF,0xFF,0xFF};
uint8_t tgt[4];

void prep(int64_t t) {
    localDigits(t, tgt);                 // 目标 HH:MM
    for (int i = 0; i < 4; i++) {
        if (tgt[i] == shown[i]) continue;
        renderDigit(i, tgt[i]);          // 画进共享缓冲
        panel[i].writeFrame();           // PON + 送数，不 DRF
        pending |= 1 << i;
    }
}

void fire() {
    for (i in pending) panel[i].triggerRefresh();
    for (i in pending) panel[i].waitRefresh(12000);
    for (i in pending) shown[i] = tgt[i];
    schedulePof(2000);
}
```

效果：

- 分钟个位每分钟必变 → 通常**只刷 1 片**；
- 分钟十位每 10 分钟变一次 → 偶尔 2 片；
- 小时位很少变；
- 用户看到的是“一个数字眨一下”，不是四屏同时黑白闪。

`TR7` 每次刷新自带电荷清理，不需要额外的定期“深度清理”。

---

## 4. 内存：一份共享帧缓存，而不是四份

4 × 38,880 = 155KB 对 C3 + WiFi 太奢侈。做法：

- 只分配 **一块 38,880 字节**；
- 四片都 `setBuffer(same)`；
- 每次只渲染“当前要刷的那一位”，`writeFrame()` 把数据送进面板 RAM 后，缓冲立刻可以复用；
- 每片只保留 1 字节 `shown[i]`。

代价是渲染要逐片串行（4 位全变时约 4 次绘制），但一分钟才一次，完全可接受。

如果还要再省，可以把字库从 336×596 缩到 300×520 左右，flash 能从 97% 降下来。

---

## 5. 网络：一次校时 + 事件驱动

- 开机：连 WiFi → `configTime()` → SNTP 立即同步一次；
- `sntp_set_sync_interval(24h)`，之后每 24 小时后台同步；
- `sntp_set_time_sync_notification_cb(onSntpSync)`，用回调而不是轮询；
- 无网：`T####` 手动设锚点，或保持空白；
- 配网热点不阻塞时钟：AP 模式下时钟继续按锚点走，配网完成再回 STA。

原则：**网络只负责“偶尔修正锚点”，永远不直接驱动刷新。**

---

## 6. 异常与恢复

- 单片 `waitRefresh` 超时：标记该片 `shown=0xFF`，下一分钟整片重绘，不阻塞其他片；
- 连续 N 次失败：记录日志，必要时重启；
- WiFi 断线：指数退避重连，不影响走时；
- 看门狗：`loop()` 每轮喂狗，刷新等待有上限；
- 掉电：锚点丢，开机重新校时；可选 NVS 存上次时间仅作诊断，不拿来走时。

---

## 7. 功耗

市电/USB：

```
T-5s  PON + 预写
T     DRF
T+3s  等 BUSY
T+2s  POF
其余 ~50s 全断电
```

平均占空比约 10/60，四片平均电流远低于常供电。

电池版可以进一步：

- 每分钟 RTC 定时唤醒，只开 10 秒窗口；
- 每 24h 提前唤醒连 WiFi 校时；
- 深睡会复位 `esp_timer`，需要把锚点存 RTC memory，并注意 RST/EN 的上拉，避免唤醒时复位面板。

---

## 8. 与现有实现的关键差异

| 维度 | 常见做法 | 本设计 |
|---|---|---|
| 时间源 | 每轮读 NTP 比对 | 单一时基 + 回调重锚 |
| 调度 | 检测分钟变化 | 绝对边界 deadline |
| 刷新 | 四片一起刷 | 只刷变化片 |
| 缓存 | 4×38.8KB | 1×38.8KB 共享 |
| 校时 | 随时校正、随时重绘 | 24h 一次，秒级不碰屏 |
| 波形 | 快刷/质量混用 | 生产固定 TR7，TR4 仅预览 |
| 断电 | 刷完立刻 POF | 延迟 2s POF |

---

## 9. 一句话总结

**把“时间”做成一个只被偶尔校准的单调时钟，把“刷新”做成只针对变化数字的绝对边界任务；网络和显示彻底解耦。**
