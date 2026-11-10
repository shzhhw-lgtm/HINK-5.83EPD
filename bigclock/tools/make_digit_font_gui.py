# -*- coding: utf-8 -*-
"""
墨水屏时钟 数字字体生成器 GUI
====================================================================
把 Windows 系统字体或任意 TTF/OTF 文件渲染成 1bpp 点阵，生成墨水屏时钟
用的 C 头文件（DIGITS[] 数组），也可导出 1-bit BMP 供 PCtoLCD2002 /
Image2Lcd 二次取模处理。

界面只需设置: 字体文件 / 字符集 / 留边% / 显示方式(白底黑字或黑底白字)。
其余工程参数(视窗480x648、内容区内缩8px、MSB位序、点阵极性等)见下方
"固定设计参数"常量, 改代码即改设计。

依赖:   pip install pillow
运行:   python make_digit_font_gui.py          （GUI 模式）
        python make_digit_font_gui.py --selftest （无界面自测，直接生成头文件）
打包exe: pyinstaller -F -w make_digit_font_gui.py

输出格式（与 test_program/test_GxEPD2/digit_font.h 完全一致）:
  - MSB first / 行主序 / 每行按字节对齐
  - bit=1 = 笔画。白底黑字时笔画画黑色; 黑底白字时同一份点阵画白色
    （头文件里的 DIGIT_INVERT 宏告知固件, 数据本身不变）
  - const 数组存于 ESP32 flash，不占 RAM
"""

import glob
import os
import sys
import tkinter as tk
from tkinter import filedialog, messagebox, ttk

from PIL import Image, ImageDraw, ImageFont, ImageTk

FONT_DIRS = [
    r"C:\Windows\Fonts",
    os.path.expandvars(r"%LOCALAPPDATA%\Microsoft\Windows\Fonts"),
]
FONT_EXTS = ("*.ttf", "*.otf", "*.TTF", "*.OTF")
DEFAULT_FONT = r"C:\Windows\Fonts\impact.ttf"

# ---- 固定设计参数(改动请编辑这里, 不再做成界面选项) ----
BOX_W, BOX_H = 480, 648   # 竖屏视窗: 面板旋转90°安装后的可视分辨率
CONTENT_INSET = 0         # 内容区四边内缩px(0=内容区=整个显示区480x648)
                          # 如需避开AA边缘可改回8(内容区464x632); 留边%在内容区内起作用
STROKE_BIT = 1            # 点阵极性: bit=1=笔画(与固件配对, 勿改)
MSB_FIRST = True          # 位序: MSB高位在左(与固件drawBitmapRotated配对, 勿改)
STROKE_PX = 0             # 描边加粗像素
SUPERSAMPLE = 4           # 超采样倍数(字形边缘更平滑)

# LSB-first 用: 256字节查表
_BIT_REV = bytes(int(f"{i:08b}"[::-1], 2) for i in range(256))


def scan_fonts():
    """扫描系统字体目录下的所有 ttf/otf 文件"""
    files = []
    for d in FONT_DIRS:
        if os.path.isdir(d):
            for ext in FONT_EXTS:
                files += glob.glob(os.path.join(d, ext))
    return sorted(set(files), key=str.lower)


def render_digit(ch, font_path, fit_w, fit_h, canvas_w, canvas_h,
                 margin_pct, supersample, stroke_px):
    """把单个字符渲染成 canvas_w x canvas_h 的 'L' 模式灰度图。
    字形缩放居中于 fit_w x fit_h 内容区（=面板尺寸-内缩），再整体居中到画布。
    255=白底, <128=笔画"""
    ss = supersample if supersample > 1 else 1
    W, H = canvas_w * ss, canvas_h * ss
    FW, FH = fit_w * ss, fit_h * ss
    margin = margin_pct / 100.0

    # 用数字"0"测宽高，等比缩放到留边后的可用区域
    ref = 200 * ss
    font = ImageFont.truetype(font_path, ref)
    bb = font.getbbox("0")
    w0, h0 = bb[2] - bb[0], bb[3] - bb[1]
    if w0 <= 0 or h0 <= 0:
        raise ValueError("字体测量失败，换一个字体文件试试")
    scale = min(FW * (1 - margin) / w0, FH * (1 - margin) / h0)
    font = ImageFont.truetype(font_path, max(8, int(ref * scale)))

    img = Image.new("L", (W, H), 255)
    d = ImageDraw.Draw(img)
    bb = d.textbbox((0, 0), ch, font=font)
    w, h = bb[2] - bb[0], bb[3] - bb[1]
    d.text(((W - w) / 2 - bb[0], (H - h) / 2 - bb[1]), ch, font=font,
           fill=0, stroke_width=max(0, int(stroke_px * ss)), stroke_fill=0)

    if ss > 1:
        img = img.resize((canvas_w, canvas_h), Image.LANCZOS)
    return img


def pack_bits(img, msb_first=True, stroke_bit=1):
    """灰度图 -> 1bpp 打包字节。bit=1 表示笔画（stroke_bit=0 时整字节取反）"""
    # 笔画映射为255，Pillow convert("1") 打包规则: MSB first、行主序、255=bit置1
    bits = img.point(lambda p: 255 if p < 128 else 0).convert("1").tobytes()
    if not msb_first:
        bits = bytes(_BIT_REV[b] for b in bits)
    if stroke_bit == 0:
        bits = bytes(b ^ 0xFF for b in bits)
    return bits


def _pad_w8(img):
    """宽度补齐到8的倍数（竖屏旋转后字形高度不一定是8的倍数），补白=非笔画"""
    w, h = img.size
    if w % 8 == 0:
        return img
    out = Image.new(img.mode, ((w // 8 + 1) * 8, h), 0 if img.mode == "1" else 255)
    out.paste(img, (0, 0))
    return out


def render_set(chars, font_path, box_w, box_h, margin_pct, supersample, stroke_px,
               inset_px=0, crop=True, portrait=False, rot="顺时针",
               msb_first=True, stroke_bit=1):
    """渲染一组字符。
    返回 (view_imgs, final_imgs, packed, counts):
      view_imgs  正立的视窗图（预览用; 字形已在内容区内缩居中）
      final_imgs 最终存储图（竖屏时已旋转90°并补齐宽度; 横屏=视窗图或裁剪图）
      packed     打包字节; counts 黑色像素数
    inset_px: 内容区相对面板四边各内缩的像素（字形只在内缩后的区域里缩放）
    crop=True 时所有字形取公共外接框裁剪（保证数组等长且相对位置不变，
    去掉大片空白背景，显著省flash）
    """
    fit_w, fit_h = box_w - 2 * inset_px, box_h - 2 * inset_px
    if fit_w <= 0 or fit_h <= 0:
        raise ValueError(f"内缩{inset_px}px过大, 内容区({fit_w}x{fit_h})为负")
    view = [render_digit(ch, font_path, fit_w, fit_h, box_w, box_h,
                         margin_pct, supersample, stroke_px) for ch in chars]
    final = list(view)
    if crop:
        boxes = [im.point(lambda p: 255 if p < 128 else 0).getbbox() for im in final]
        ux0 = min(b[0] for b in boxes)
        uy0 = min(b[1] for b in boxes)
        ux1 = max(b[2] for b in boxes)
        uy1 = max(b[3] for b in boxes)
        final = [im.crop((ux0, uy0, ux1, uy1)) for im in final]
        final = [_pad_w8(im) for im in final]   # 行打包要求宽度为8的倍数
    if portrait:
        r = Image.ROTATE_90 if rot == "逆时针" else Image.ROTATE_270
        final = [_pad_w8(im.transpose(r)) for im in final]
    packed, counts = [], []
    for im in final:
        packed.append(pack_bits(im, msb_first, stroke_bit))
        hist = im.histogram()          # 0~255灰度直方图, <128为笔画
        counts.append(sum(hist[:128]))
    return view, final, packed, counts


def draw_screen_mockup(view_imgs, chars, box_w, box_h, inset_px, n_panels=4):
    """整机显示效果示意: n_panels 片竖屏面板横拼。
    深灰=面板玻璃边框, 白=显示区(AA), 虚线框=内容区内缩边界。
    view_imgs 为 render_set 返回的视窗图（已含内缩居中放置）。
    返回原始分辨率的 PIL 'L' 图。"""
    bez = 14          # 示意用面板边框厚度
    gap = 8           # 面板间距
    W, H = box_w, box_h
    total_w = n_panels * (W + 2 * bez) + (n_panels - 1) * gap
    total_h = H + 2 * bez
    out = Image.new("L", (total_w, total_h), 110)
    for i in range(n_panels):
        x0 = i * (W + 2 * bez + gap) + bez
        out.paste(view_imgs[i % len(view_imgs)], (x0, bez))
        d = ImageDraw.Draw(out)
        # 内容区内缩边界(虚线); 内缩为0时不画(边界就是AA边缘)
        if inset_px > 0:
            ix0, iy0 = x0 + inset_px, bez + inset_px
            ix1, iy1 = x0 + W - inset_px, bez + H - inset_px
            for k in range(0, W - 2 * inset_px, 14):
                d.line((ix0 + k, iy0, ix0 + k + 6, iy0), fill=150)
                d.line((ix0 + k, iy1, ix0 + k + 6, iy1), fill=150)
            for k in range(0, H - 2 * inset_px, 14):
                d.line((ix0, iy0 + k, ix0, iy0 + k + 6), fill=150)
                d.line((ix1, iy0 + k, ix1, iy0 + k + 6), fill=150)
    return out


def build_header(chars, packed_list, store_w, store_h, font_path, invert):
    """打包数据 -> C 头文件文本（store_w/h 为最终存储点阵的宽高）"""
    row_bytes = store_w // 8
    lines = []
    lines.append("#pragma once")
    lines.append("// Auto-generated by make_digit_font_gui.py")
    lines.append(f"// font : {os.path.basename(font_path)}")
    lines.append(f"// box  : 存储 {store_w}x{store_h} (正立视窗方向, 固件运行时旋转)")
    lines.append(f"// chars: {''.join(chars)}")
    lines.append("// bpp  : 1, MSB first, row-major, bit=1=stroke")
    lines.append(f"#define DIGIT_W        {store_w}")
    lines.append(f"#define DIGIT_H        {store_h}")
    lines.append(f"#define DIGIT_ROW_BYTES {row_bytes}")
    lines.append(f"#define DIGIT_COUNT    {len(chars)}")
    lines.append(f"#define DIGIT_INVERT   {1 if invert else 0}   // 显示方式: 0=白底黑字 1=黑底白字")
    lines.append("#include <pgmspace.h>")
    lines.append("static const uint8_t DIGITS[DIGIT_COUNT][DIGIT_ROW_BYTES * DIGIT_H] PROGMEM = {")
    for ch, data in zip(chars, packed_list):
        lines.append(f"  /* '{ch}' */ {{")
        for r in range(store_h):
            row = data[r * row_bytes:(r + 1) * row_bytes]
            hexs = ",".join(f"0x{b:02X}" for b in row)
            tail = "," if r < store_h - 1 else ""
            lines.append(hexs + tail)
        lines.append("  },")
    lines.append("};")
    return "\n".join(lines) + "\n"


class App(tk.Tk):
    def __init__(self):
        super().__init__()
        self.title("墨水屏数字字体生成器 — e-ink bigclock")
        self.geometry("1000x720")
        self._photos = []          # 防止预览图被GC
        self._font_files = scan_fonts()
        self._build_ui()

    # ---------- UI ----------
    def _build_ui(self):
        top = ttk.Frame(self, padding=8)
        top.pack(fill="x")

        # 字体选择
        ttk.Label(top, text="字体文件:").grid(row=0, column=0, sticky="w")
        self.font_var = tk.StringVar(value=DEFAULT_FONT if os.path.exists(DEFAULT_FONT)
                                     else (self._font_files[0] if self._font_files else ""))
        self.font_cb = ttk.Combobox(top, textvariable=self.font_var, width=52,
                                    values=self._font_files)
        self.font_cb.grid(row=0, column=1, padx=4)
        ttk.Button(top, text="浏览...", command=self._browse_font).grid(row=0, column=2)
        ttk.Button(top, text="重扫系统字体", command=self._rescan).grid(row=0, column=3, padx=4)

        # 字符集
        ttk.Label(top, text="字符集:").grid(row=1, column=0, sticky="w")
        self.chars_var = tk.StringVar(value="0123456789")
        ttk.Entry(top, textvariable=self.chars_var, width=16).grid(row=1, column=1,
                                                                   sticky="w", padx=4)
        # 留边%（字形相对内容区464x632的缩放留白）
        ttk.Label(top, text="留边%:").grid(row=1, column=2, sticky="e")
        self.margin_var = tk.IntVar(value=8)
        ttk.Spinbox(top, from_=0, to=40, width=4,
                    textvariable=self.margin_var).grid(row=1, column=3, sticky="w")

        # 显示方式
        ttk.Label(top, text="显示方式:").grid(row=1, column=4, sticky="e")
        self.style_var = tk.StringVar(value="白底黑字")
        ttk.Combobox(top, textvariable=self.style_var, width=10, state="readonly",
                     values=["白底黑字", "黑底白字"]).grid(row=1, column=5, sticky="w")

        self.crop_var = tk.BooleanVar(value=True)
        ttk.Checkbutton(top, text="紧凑裁剪(省flash)",
                        variable=self.crop_var).grid(row=1, column=6, columnspan=2, sticky="w")

        # 动作按钮
        btns = ttk.Frame(top)
        btns.grid(row=2, column=0, columnspan=8, pady=8, sticky="w")
        ttk.Button(btns, text="刷新预览", command=self.refresh_preview).pack(side="left", padx=4)
        ttk.Button(btns, text="生成C头文件...", command=self.gen_header).pack(side="left", padx=4)
        ttk.Button(btns, text="导出10张1bit BMP...", command=self.export_bmps).pack(side="left", padx=4)

        # 预览区（自上而下: 字形一行文字 + 屏幕整机效果, 无需切换标签页）
        self.canvas = tk.Canvas(self, bg="#e0e0e0", highlightthickness=0)
        self.canvas.pack(fill="both", expand=True, padx=8, pady=(4, 0))

        # 日志
        self.log = tk.Text(self, height=7, state="disabled", font=("Consolas", 9))
        self.log.pack(fill="x", padx=8, pady=(4, 8))

        self.refresh_preview()

    # ---------- 工具 ----------
    def _log(self, msg):
        self.log.configure(state="normal")
        self.log.insert("end", msg + "\n")
        self.log.see("end")
        self.log.configure(state="disabled")

    def _browse_font(self):
        p = filedialog.askopenfilename(title="选择字体文件",
                                       filetypes=[("字体文件", "*.ttf *.otf"), ("所有文件", "*.*")])
        if p:
            self.font_var.set(p)

    def _rescan(self):
        self._font_files = scan_fonts()
        self.font_cb["values"] = self._font_files
        self._log(f"扫描到 {len(self._font_files)} 个系统字体文件")

    def _settings(self):
        chars = list(self.chars_var.get().strip()) or list("0123456789")
        return dict(
            font_path=self.font_var.get().strip(),
            chars=chars, box_w=BOX_W, box_h=BOX_H,
            margin=int(self.margin_var.get()),
            style=self.style_var.get(),
            crop=self.crop_var.get(),
        )

    def _render_all(self, s):
        return render_set(s["chars"], s["font_path"], s["box_w"], s["box_h"],
                          s["margin"], SUPERSAMPLE, STROKE_PX,
                          inset_px=CONTENT_INSET,
                          crop=s["crop"], portrait=False,
                          msb_first=MSB_FIRST, stroke_bit=STROKE_BIT)

    # ---------- 动作 ----------
    def refresh_preview(self):
        try:
            s = self._settings()
            view, final, _, counts = self._render_all(s)
        except Exception as e:
            messagebox.showerror("渲染失败", str(e))
            return
        # 黑底白字: 预览反相显示(点阵数据本身不变)
        inv = s["style"] == "黑底白字"
        final_d = [im.point(lambda p: 255 - p) for im in final] if inv else final
        view_d = [im.point(lambda p: 255 - p) for im in view] if inv else view
        self.canvas.delete("all")
        self._photos.clear()
        cw = max(self.canvas.winfo_width(), 940)
        ch = max(self.canvas.winfo_height(), 430)

        # --- 上: 字形预览(一行文字; final已按公共外接框裁剪, 横排即为文字行) ---
        strip_h = 100
        gap = 8
        scaled = []
        for im in final_d:
            r = strip_h / im.height
            scaled.append(im.resize((max(1, int(im.width * r)), strip_h)))
        strip_w = sum(im.width for im in scaled) + gap * (len(scaled) - 1)
        strip = Image.new("L", (strip_w, strip_h), 0 if inv else 255)
        x = 0
        for im in scaled:
            strip.paste(im, (x, 0))
            x += im.width + gap
        ph = ImageTk.PhotoImage(strip)
        self._photos.append(ph)
        sx = max(20, (cw - strip_w) // 2)
        y = 14
        self.canvas.create_image(sx, y, anchor="nw", image=ph)
        self.canvas.create_text(sx + strip_w / 2, y + strip_h + 16,
                                text=f"字形预览 ({s['style']})", font=("Microsoft YaHei", 9))

        # --- 下: 屏幕效果预览(单屏示意 + 4屏整机横拼) ---
        y2 = y + strip_h + 44
        single = draw_screen_mockup(view_d, s["chars"], s["box_w"], s["box_h"],
                                    CONTENT_INSET, n_panels=1)
        four = draw_screen_mockup(view_d, s["chars"], s["box_w"], s["box_h"],
                                  CONTENT_INSET, n_panels=4)
        k = min(0.5, (cw - 60) / (single.width + four.width + 24),
                (ch - y2 - 46) / single.height)
        k = max(k, 0.1)
        sw, sh = int(single.width * k), int(single.height * k)
        fw, fh = int(four.width * k), int(four.height * k)
        p1 = ImageTk.PhotoImage(single.resize((sw, sh)))
        p2 = ImageTk.PhotoImage(four.resize((fw, fh)))
        self._photos += [p1, p2]
        self.canvas.create_image(24, y2, anchor="nw", image=p1)
        self.canvas.create_text(24 + sw / 2, y2 + sh + 14,
                                text=f"单屏(竖装)  内容区{s['box_w']-2*CONTENT_INSET}"
                                     f"x{s['box_h']-2*CONTENT_INSET}",
                                font=("Microsoft YaHei", 9))
        x2 = 24 + sw + 24
        self.canvas.create_image(x2, y2, anchor="nw", image=p2)
        self.canvas.create_text(x2 + fw / 2, y2 + fh + 14,
                                text=f"整机 4片竖拼  {s['box_w']*4}x{s['box_h']}px"
                                     f" ≈ {s['box_w']*4*0.1833:.0f}x{s['box_h']*0.1833:.0f}mm",
                                font=("Microsoft YaHei", 9))

        sw2, sh2 = final[0].size
        self._log(f"预览完成: {len(final)} 个字符, 视窗 {s['box_w']}x{s['box_h']}, "
                  f"存储点阵 {sw2}x{sh2}, 显示方式: {s['style']} (旋转在固件运行时做)")

    def gen_header(self):
        try:
            s = self._settings()
            _, final, packed, _ = self._render_all(s)
        except Exception as e:
            messagebox.showerror("生成失败", str(e))
            return
        store_w, store_h = final[0].size
        default_dir = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                                    "..", "test_program", "test_GxEPD2"))
        path = filedialog.asksaveasfilename(
            title="保存C头文件", defaultextension=".h",
            initialfile="digit_font.h", initialdir=default_dir,
            filetypes=[("C头文件", "*.h")])
        if not path:
            return
        text = build_header(s["chars"], packed, store_w, store_h,
                            s["font_path"], invert=(s["style"] == "黑底白字"))
        with open(path, "w", encoding="utf-8", newline="\n") as f:
            f.write(text)
        kb = len(packed[0]) * len(packed) / 1024
        self._log(f"已生成 {path}  ({kb:.0f} KB flash / {len(s['chars'])} 个字符, RAM零开销,"
                  f" {s['style']})")
        messagebox.showinfo("完成", f"已生成:\n{path}\n\n直接替换 sketch 目录下的"
                                    f" digit_font.h 重新编译即可。")

    def export_bmps(self):
        try:
            s = self._settings()
            _, final, _, _ = self._render_all(s)
        except Exception as e:
            messagebox.showerror("导出失败", str(e))
            return
        d = filedialog.askdirectory(title="选择BMP导出目录")
        if not d:
            return
        for ch, img in zip(s["chars"], final):
            # 与显示方式一致的 1bit BMP（笔画=黑/白按风格），
            # 可直接被 PCtoLCD2002/Image2Lcd 打开做二次处理
            g = img.point(lambda p: 255 if p < 128 else 0)
            if s["style"] == "黑底白字":
                g = g.point(lambda p: 255 - p)   # 黑底白字
            out = g.convert("1")
            out.save(os.path.join(d, f"digit_{ch}.bmp"))
        self._log(f"已导出 {len(final)} 张 BMP 到 {d} ({s['style']})")
        messagebox.showinfo("完成", f"已导出 {len(final)} 张 1-bit BMP")


def selftest():
    """无界面自测: 用固定设计参数(竖屏480x648, 内缩8px, 留边8%, Impact)生成头文件"""
    # 点阵按"视窗方向"(正立)存储, 旋转由固件运行时完成(见 test_GxEPD2.ino 的
    # DIGIT_ROT / drawBitmapRotated), 这样安装方向可随时在固件里改
    # 打包自检: 8x1 图, 最左像素(x=0)为笔画
    # MSB first: x=0 -> bit7 = 0x80;  LSB first: x=0 -> bit0 = 0x01
    img = Image.new("L", (8, 1), 255)
    img.putpixel((0, 0), 0)
    assert pack_bits(img) == b"\x80", "MSB打包自检失败"
    assert pack_bits(img, msb_first=False) == b"\x01", "LSB打包自检失败"

    chars = list("0123456789")
    out = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                        "..", "test_program", "test_GxEPD2", "digit_font.h"))
    view, final, packed, counts = render_set(
        chars, DEFAULT_FONT, BOX_W, BOX_H, 8, SUPERSAMPLE, STROKE_PX,
        inset_px=CONTENT_INSET, crop=True, portrait=False)
    store_w, store_h = final[0].size
    assert store_w % 8 == 0, "存储宽度未对齐到8"
    text = build_header(chars, packed, store_w, store_h, DEFAULT_FONT, invert=False)
    assert "#define DIGIT_INVERT   0" in text, "显示方式宏缺失"
    with open(out, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)
    print("打包自检通过")
    print(f"视窗 {BOX_W}x{BOX_H}, 内容区 {BOX_W-2*CONTENT_INSET}x{BOX_H-2*CONTENT_INSET}"
          f" -> 正立存储点阵 {store_w}x{store_h} (旋转在固件运行时做)")
    print("黑色像素数:", dict(zip(chars, counts)))
    print(f"OK -> {out}  ({len(packed[0]) * 10 / 1024:.0f} KB flash / 10 字符)")
    # 合理性: '1' 的笔画应明显少于 '8'
    assert counts[1] < counts[8] * 0.75, "渲染结果异常"
    print("渲染合理性自检通过")


if __name__ == "__main__":
    if "--selftest" in sys.argv:
        selftest()
    else:
        App().mainloop()
