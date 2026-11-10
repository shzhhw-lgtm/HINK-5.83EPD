# -*- coding: utf-8 -*-
"""
UC8179 OTP 解析器 / 面板 Profile 生成器
=========================================
输入: 逻辑分析仪或 MCU 导出的 "Read OTP (0xA2)" 十六进制转储文本。
输出:
  1. 终端打印完整解码结果（校验码、温度分档、命令默认值、12 组 TR 波形参数）
  2. 可选的 C 头文件（面板 profile），直接放进 Arduino 库使用

用法:
  python uc8179_otp_parser.py HINK-E0583A51-A0_OTP.txt
  python uc8179_otp_parser.py dump.txt --header UC8179_PanelProfile.h

OTP 地址映射见 UC8179 手册 "OTP ADDRESS MAPPING" (6KB, Bank0 0x0000, Bank1 0x0C00)
"""
import argparse
import re
import sys

try:  # Windows 控制台优先用 UTF-8，避免中文乱码
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
except Exception:
    pass

OTP_SIZE = 0x1800
BANK1 = 0x0C00
TR_BASE = 0x0049      # TR0 起始地址
TR_SIZE = 0x00F7      # 每组 TR 247 字节
BORDER_BASE = 0x001F  # Border LUT 42 字节
BORDER_SIZE = 42

TR_NAME = ["TR0", "TR1", "TR2", "TR3", "TR4", "TR5",
           "TR6", "TR7", "TR8", "TR9", "TR10", "TR11"]

FRS_HZ = {0: 5, 1: 10, 2: 15, 3: 20, 4: 30, 5: 40, 6: 50, 7: 60,
          8: 70, 9: 80, 10: 90, 11: 100, 12: 110, 13: 130, 14: 150, 15: 200}

# 电压码表：VDH/VDL/VDHR 2.4V + 0.2V*n
def vdh(code):
    return 2.4 + 0.2 * (code & 0x3F)

def vdl(code):
    return -(2.4 + 0.2 * (code & 0x3F))

def vcom(code):
    return -(0.10 + 0.05 * (code & 0x7F))

# VG_LVL -> VGH/VGL
VG_TABLE = {0: 9, 1: 10, 2: 11, 3: 12, 4: 17, 5: 18, 6: 19, 7: 20}


def parse_dump(path):
    """解析 '0000: A5 05 ...' 格式，返回 6144 字节 bytearray"""
    mem = bytearray(b"\xff" * OTP_SIZE)
    pat = re.compile(r"^\s*([0-9A-Fa-f]{4})\s*:\s*((?:[0-9A-Fa-f]{2}\s*)+)")
    found = 0
    with open(path, "r", encoding="utf-8", errors="ignore") as f:
        for line in f:
            m = pat.match(line)
            if not m:
                continue
            addr = int(m.group(1), 16)
            data = bytes(int(b, 16) for b in m.group(2).split())
            if addr + len(data) > OTP_SIZE:
                data = data[:OTP_SIZE - addr]
            mem[addr:addr + len(data)] = data
            found += len(data)
    if found == 0:
        raise SystemExit("没有解析到任何十六进制数据行（应形如 '0000: A5 05 0A ...'）")
    return mem, found


def s8(v):
    return v - 256 if v >= 128 else v


def analyse(mem):
    out = {}
    out["bank0_check"] = mem[0x0000]
    out["bank1_check"] = mem[BANK1]
    out["bank"] = 0 if mem[0] == 0xA5 else (1 if mem[BANK1] == 0xA5 else -1)
    base = 0 if out["bank"] == 0 else BANK1
    out["base"] = base

    # 温度分档 (11 个边界, 0x7F 结束)
    tb = [mem[base + 1 + i] for i in range(11)]
    out["tb_raw"] = tb
    out["tb"] = [s8(x) for x in tb]

    # 命令默认值 0x000C ~ 0x001E
    cd = mem[base + 0x0C: base + 0x0C + 19]
    out["cmd_defaults"] = cd

    # Border LUT
    out["border"] = mem[base + BORDER_BASE: base + BORDER_BASE + BORDER_SIZE]

    # TR
    trs = []
    for n in range(12):
        off = base + TR_BASE + n * TR_SIZE
        blk = mem[off: off + TR_SIZE]
        tr = {"n": n, "raw": blk}
        b0, b1, b2, b3, b4, b5, b6 = blk[0:7]
        tr["frs"] = b0 >> 4
        tr["hz"] = FRS_HZ.get(b0 >> 4, 0)
        tr["vcend"] = (b0 >> 3) & 1
        tr["vg"] = b0 & 7
        tr["vg_v"] = VG_TABLE[tr["vg"]]
        tr["bdend"] = b1 >> 6
        tr["vdh"] = b1 & 0x3F
        tr["vdh_v"] = vdh(tr["vdh"])
        tr["xon_hi"] = b2 >> 6
        tr["vdl"] = b2 & 0x3F
        tr["vdl_v"] = vdl(tr["vdl"])
        tr["kwe_hi"] = b3 >> 6
        tr["vdhr"] = b3 & 0x3F
        tr["vdhr_v"] = vdh(tr["vdhr"])
        tr["xon"] = (tr["xon_hi"] << 8) | b4
        tr["vcom_dc"] = b5 & 0x7F
        tr["vcom_v"] = vcom(tr["vcom_dc"])
        tr["kwe"] = (tr["kwe_hi"] << 8) | b6
        tr["blank"] = all(b == 0xFF for b in blk)
        trs.append(tr)
    out["trs"] = trs

    # 生产版本 / LUT 版本
    out["prod_rev"] = mem[base + 0x0BDD: base + 0x0BE0]
    out["lut_rev"] = mem[base + 0x0BE0: base + 0x0BE3]

    # 分辨率（从 OTP 命令默认值推断）
    hres9_3 = cd[0x0016 - 0x0C]
    vres_hi = cd[0x0017 - 0x0C] & 0x03
    vres_lo = cd[0x0018 - 0x0C]
    hst9_3 = cd[0x0019 - 0x0C]
    vst_hi = cd[0x001A - 0x0C] & 0x03
    vst_lo = cd[0x001B - 0x0C]
    out["hres"] = hres9_3 * 8
    out["vres"] = (vres_hi << 8) | vres_lo
    out["hst"] = hst9_3 * 8
    out["vst"] = (vst_hi << 8) | vst_lo
    return out


def pretty(a):
    p = print
    p("=" * 66)
    p(" UC8179 OTP 解析结果")
    p("=" * 66)
    p(f" Bank0 校验码 0x0000 = 0x{a['bank0_check']:02X}   "
      f"Bank1 校验码 0x0C00 = 0x{a['bank1_check']:02X}")
    p(f" 使用 Bank{a['bank']} (基址 0x{a['base']:04X})")
    if a["bank"] < 0:
        p(" !! 两个 bank 都没有 0xA5 校验码，这块 OTP 未烧写或读取有误")
        return

    p("\n-- 温度分档 (0x7F = 结束) " + "-" * 34)
    p(" 索引  原始   有符号   含义")
    for i, (raw, val) in enumerate(zip(a["tb_raw"], a["tb"])):
        if raw == 0x7F:
            note = "结束标记（此档及以下靠 TR%d，更高温度回落到最后有效档）" % i
        elif raw == 0xFF:
            note = "未使用（已被 0x7F 提前结束）"
        else:
            note = "≤ 此温度使用 " + TR_NAME[i]
        p(f" TB{i:<2}  0x{raw:02X}   {val:4d}°C   {note}")

    p("\n-- 分辨率 / 扫描起点 " + "-" * 40)
    p(f" HRES = {a['hres']} px, VRES = {a['vres']} px")
    p(f" 可见区 {a['hres']} x {a['vres']}, 1bpp 帧缓存 = "
      f"{a['hres'] * a['vres'] // 8} 字节")
    p(f" HST (source start) = {a['hst']}, VST (gate start) = {a['vst']}")

    cd = a["cmd_defaults"]
    p("\n-- OTP 命令默认值 0x000C~0x001E " + "-" * 33)
    p(f" 0x0C 键值     0x{cd[0]:02X}   (0xA5 = 默认值功能已启用)")
    p(f" 0x0D PSR      0x{cd[1]:02X}   (bit5 REG: 0=OTP LUT, 1=寄存器 LUT)")
    p(f" 0x0E BT_PHA   0x{cd[2]:02X}")
    p(f" 0x0F BT_PHB   0x{cd[3]:02X}")
    p(f" 0x10 BT_PHC1  0x{cd[4]:02X}")
    p(f" 0x11 BT_PHC2  0x{cd[5]:02X}")
    p(f" 0x12 KWOPT    0x{cd[6]:02X}")
    p(f" 0x13 CDI1     0x{cd[7]:02X}")
    p(f" 0x14 CDI2     0x{cd[8]:02X}")
    p(f" 0x15 TCON     0x{cd[9]:02X}")
    p(f" 0x16 HRES[9:3] 0x{cd[10]:02X} -> {cd[10] * 8}")
    p(f" 0x17 VRES[9:8] 0x{cd[11]:02X}")
    p(f" 0x18 VRES[7:0] 0x{cd[12]:02X}")
    p(f" 0x19 HST[9:3]  0x{cd[13]:02X} -> {cd[13] * 8}")
    p(f" 0x1A VST[9:8]  0x{cd[14]:02X}")
    p(f" 0x1B VST[7:0]  0x{cd[15]:02X}")
    p(f" 0x1C PWS      0x{cd[16]:02X}")
    p(f" 0x1D LVSEL    0x{cd[17]:02X}")
    p(f" 0x1E TSBDRY   0x{cd[18]:02X}")

    p("\n-- 12 组温度-波形表 (TR) " + "-" * 38)
    p(" 名称   帧率  VDH     VDL     VDHR    VCOM_DC  VGH/VGL  XON  KWE")
    for t in a["trs"]:
        if t["blank"]:
            p(f" {TR_NAME[t['n']]:<5}  (空，0xFF)")
            continue
        p(f" {TR_NAME[t['n']]:<5}  {t['hz']:>3}Hz  "
          f"{t['vdh_v']:5.1f}V  {t['vdl_v']:6.1f}V  {t['vdhr_v']:5.1f}V  "
          f"{t['vcom_v']:7.2f}V  ±{t['vg_v']}V    "
          f"0x{t['xon']:03X} 0x{t['kwe']:03X}")

    p("\n-- 版本 " + "-" * 57)
    pr = a["prod_rev"]
    lr = a["lut_rev"]
    p(f" 生产版本 0x{pr[0]:02X}{pr[1]:02X}{pr[2]:02X}   "
      f"LUT 版本 0x{lr[0]:02X}{lr[1]:02X}{lr[2]:02X}")


def emit_header(a, path, name):
    """生成 C 头文件（含面板 profile 与默认寄存器值）"""
    t0 = a["trs"][0]
    cd = a["cmd_defaults"]
    lines = []
    lines.append("// 由 uc8179_otp_parser.py 自动生成，请勿手改")
    lines.append(f"// 面板: {name}")
    lines.append("// 数据来源: 板载 UC8179 OTP 转储")
    lines.append("#pragma once")
    lines.append("")
    lines.append("#define UC8179_PANEL_NAME      \"%s\"" % name)
    lines.append("#define UC8179_PANEL_WIDTH     %d" % a["hres"])
    lines.append("#define UC8179_PANEL_HEIGHT    %d" % a["vres"])
    lines.append("#define UC8179_PANEL_HRES      %d" % a["hres"])
    lines.append("#define UC8179_PANEL_VRES      %d" % a["vres"])
    lines.append("#define UC8179_PANEL_HST       %d" % a["hst"])
    lines.append("#define UC8179_PANEL_VST       %d" % a["vst"])
    lines.append("")
    lines.append("// OTP 0x0D: PSR，bit5 REG=0 -> 使用 OTP 内部 LUT")
    lines.append("#define UC8179_PROFILE_PSR     0x%02X" % cd[1])
    lines.append("// OTP 0x13/0x14: CDI (BDZ/BDV/N2OCP/DDX + CDI)")
    lines.append("#define UC8179_PROFILE_CDI1    0x%02X" % cd[7])
    lines.append("#define UC8179_PROFILE_CDI2    0x%02X" % cd[8])
    lines.append("// OTP 0x15: TCON (S2G/G2S 非交叠)")
    lines.append("#define UC8179_PROFILE_TCON    0x%02X" % cd[9])
    lines.append("// OTP 0x0E~0x11: Booster 软启动")
    lines.append("#define UC8179_PROFILE_BTST1   0x%02X" % cd[2])
    lines.append("#define UC8179_PROFILE_BTST2   0x%02X" % cd[3])
    lines.append("#define UC8179_PROFILE_BTST3   0x%02X" % cd[4])
    lines.append("#define UC8179_PROFILE_BTST4   0x%02X" % cd[5])
    lines.append("")
    lines.append("// TR0 (常温档) 的驱动电压，来自 OTP 0x0049~0x004F")
    lines.append("#define UC8179_PROFILE_VDH     0x%02X   // %.1f V" % (t0["vdh"], t0["vdh_v"]))
    lines.append("#define UC8179_PROFILE_VDL     0x%02X   // %.1f V" % (t0["vdl"], t0["vdl_v"]))
    lines.append("#define UC8179_PROFILE_VDHR    0x%02X   // %.1f V" % (t0["vdhr"], t0["vdhr_v"]))
    lines.append("#define UC8179_PROFILE_VDCS    0x%02X   // %.2f V" % (t0["vcom_dc"], t0["vcom_v"]))
    lines.append("#define UC8179_PROFILE_VG      0x%02X   // VGH/VGL = ±%d V" % (t0["vg"], t0["vg_v"]))
    lines.append("")
    lines.append("// 温度分档 (有符号 °C)；0x7F 为结束标记")
    tb_cnt = next((i + 1 for i, x in enumerate(a["tb_raw"]) if x == 0x7F), 12)
    lines.append("#define UC8179_PROFILE_TB_COUNT %d" % tb_cnt)
    for i, (raw, val) in enumerate(zip(a["tb_raw"], a["tb"])):
        lines.append("#define UC8179_PROFILE_TB%d     %d" % (i, val))
    lines.append("")
    with open(path, "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")
    print(f"\n已生成 C 头文件: {path}")


def main():
    ap = argparse.ArgumentParser(description="UC8179 OTP 解析器")
    ap.add_argument("dump", help="OTP 十六进制转储文本")
    ap.add_argument("--header", help="生成 C 头文件路径")
    ap.add_argument("--name", default="HINK-E0583A51-A0", help="面板型号")
    args = ap.parse_args()

    mem, n = parse_dump(args.dump)
    print(f"读取 {n} 字节 (OTP 共 {OTP_SIZE} 字节)")
    a = analyse(mem)
    pretty(a)
    if args.header:
        emit_header(a, args.header, args.name)


if __name__ == "__main__":
    main()
