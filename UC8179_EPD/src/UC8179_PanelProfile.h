// 由 uc8179_otp_parser.py 自动生成，请勿手改
// 面板: HINK-E0583A51-A0
// 数据来源: 板载 UC8179 OTP 转储
#pragma once

#define UC8179_PANEL_NAME      "HINK-E0583A51-A0"
#define UC8179_PANEL_WIDTH     648
#define UC8179_PANEL_HEIGHT    480
#define UC8179_PANEL_HRES      648
#define UC8179_PANEL_VRES      480
#define UC8179_PANEL_HST       16
#define UC8179_PANEL_VST       0

// OTP 0x0D: PSR，bit5 REG=0 -> 使用 OTP 内部 LUT
#define UC8179_PROFILE_PSR     0x17
// OTP 0x13/0x14: CDI (BDZ/BDV/N2OCP/DDX + CDI)
#define UC8179_PROFILE_CDI1    0x10
#define UC8179_PROFILE_CDI2    0x07
// OTP 0x15: TCON (S2G/G2S 非交叠)
#define UC8179_PROFILE_TCON    0x22
// OTP 0x0E~0x11: Booster 软启动
#define UC8179_PROFILE_BTST1   0xCF
#define UC8179_PROFILE_BTST2   0xCF
#define UC8179_PROFILE_BTST3   0x26
#define UC8179_PROFILE_BTST4   0x17

// TR0 (常温档) 的驱动电压，来自 OTP 0x0049~0x004F
#define UC8179_PROFILE_VDH     0x3F   // 15.0 V
#define UC8179_PROFILE_VDL     0x3F   // -15.0 V
#define UC8179_PROFILE_VDHR    0x12   // 6.0 V
#define UC8179_PROFILE_VDCS    0x12   // -1.00 V
#define UC8179_PROFILE_VG      0x07   // VGH/VGL = ±20 V

// 温度分档 (有符号 °C)；0x7F 为结束标记
#define UC8179_PROFILE_TB_COUNT 8
#define UC8179_PROFILE_TB0     5
#define UC8179_PROFILE_TB1     10
#define UC8179_PROFILE_TB2     15
#define UC8179_PROFILE_TB3     20
#define UC8179_PROFILE_TB4     30
#define UC8179_PROFILE_TB5     80
#define UC8179_PROFILE_TB6     90
#define UC8179_PROFILE_TB7     127
#define UC8179_PROFILE_TB8     -1
#define UC8179_PROFILE_TB9     -1
#define UC8179_PROFILE_TB10     -1

