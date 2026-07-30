#!/usr/bin/env python3
"""把 .bcfnt 里指定汉字的字形从图集里挖出来,用字符画显示。

为什么要按字看
--------------
「缺笔画」有两个来源,现象一样,修法完全相反:

  A. 纹理里就缺   → mkbcfnt 光栅化时尺寸不对(把 N 个设计像素塞进
                    更少的纹理像素,整行笔画被丢掉),或者这个字号
                    下字体本身就画不全(12px 汉字有物理上限)
  B. 纹理里是全的 → 绘制端有隐藏缩放,NEAREST 采样丢像素

**注意**:不要用「中间灰占比」判断 A。mkbcfnt 若按单色阈值光栅化,
输出永远是纯二值的,缩小丢笔画时也是 0.00% —— 那个指标恒为 0,
什么都没测。只有把字形本身画出来和字典里的笔画数对照才作数。

用法
----
    python3 tools/fontdump.py romfs/font.bcfnt                 # 概览
    python3 tools/fontdump.py romfs/font.bcfnt --char 蒙巍赢
    python3 tools/fontdump.py romfs/font.bcfnt 0 300 128 12    # 原始区域
"""
import sys

FMT_A4, FMT_A8 = 11, 8


def morton(x, y):
    return ((x & 1) | ((y & 1) << 1) | ((x & 2) << 1) | ((y & 2) << 2)
            | ((x & 4) << 2) | ((y & 4) << 3))


class Font:
    def __init__(self, path):
        d = self.d = open(path, "rb").read()
        i = d.find(b"TGLP")
        if i < 0:
            sys.exit("不是合法的 bcfnt(找不到 TGLP)")
        self.i = i
        u8 = lambda o: d[i + o]
        u16 = lambda o: int.from_bytes(d[i + o:i + o + 2], "little")
        u32 = lambda o: int.from_bytes(d[i + o:i + o + 4], "little")
        self.cw, self.ch, self.baseline = u8(8), u8(9), u8(10)
        self.sheet_size, self.n_sheets, self.fmt = u32(12), u16(16), u16(18)
        self.ncols, self.nrows = u16(20), u16(22)
        self.sw, self.sh = u16(24), u16(26)
        self.off = u32(28)
        if self.fmt not in (FMT_A4, FMT_A8):
            sys.exit(f"只支持 A4/A8,当前格式 {self.fmt}")
        # 格子间距是 cell+1(图集按 (cw+1)x(ch+1) 网格排布)
        self.pitch_x, self.pitch_y = self.cw + 1, self.ch + 1
        self.per_sheet = self.ncols * self.nrows

    def px(self, sheet, x, y):
        base = self.off + sheet * self.sheet_size
        tile = (y // 8) * (self.sw // 8) + (x // 8)
        idx = tile * 64 + morton(x % 8, y % 8)
        if self.fmt == FMT_A4:
            b = self.d[base + (idx >> 1)]
            return (b & 0xF) if (idx & 1) == 0 else (b >> 4)
        return self.d[base + idx] >> 4

    def cmap(self):
        """扫全文件里所有 CMAP 块,建 码点 -> 字形序号 表。
        不跟链表指针走 —— bcfnt 的 offset 有 +8 的历史包袱,直接扫更稳。"""
        d, m = self.d, {}
        pos = 0
        while True:
            pos = d.find(b"CMAP", pos)
            if pos < 0:
                break
            u16 = lambda o: int.from_bytes(d[pos + o:pos + o + 2], "little")
            begin, end, method = u16(8), u16(10), u16(12)
            p = pos + 20
            try:
                if method == 0:                       # 直接映射
                    base = u16(20)
                    for c in range(begin, end + 1):
                        m[c] = base + c - begin
                elif method == 1:                     # 查表
                    for k, c in enumerate(range(begin, end + 1)):
                        v = int.from_bytes(d[p + k * 2:p + k * 2 + 2], "little")
                        if v != 0xFFFF:
                            m[c] = v
                elif method == 2:                     # 扫描对
                    n = u16(20)
                    for k in range(n):
                        q = pos + 22 + k * 4
                        c = int.from_bytes(d[q:q + 2], "little")
                        v = int.from_bytes(d[q + 2:q + 4], "little")
                        if v != 0xFFFF:
                            m[c] = v
            except IndexError:
                pass
            pos += 4
        return m

    def draw_cell(self, index, label=""):
        sheet = index // self.per_sheet
        rem = index % self.per_sheet
        row, col = rem // self.ncols, rem % self.ncols
        x0, y0 = col * self.pitch_x, row * self.pitch_y
        if sheet >= self.n_sheets:
            print(f"  {label}: 字形序号 {index} 超出图集范围")
            return
        ramp = ".123456789abcde#"
        print(f"  {label}  字形#{index}  图集{sheet} 第{row}行第{col}列  "
              f"({x0},{y0}) {self.cw}x{self.ch}")
        ink = 0
        for y in range(y0, y0 + self.ch):
            line = []
            for x in range(x0, x0 + self.cw):
                v = self.px(sheet, x, y) if (x < self.sw and y < self.sh) else 0
                ink += v > 0
                line.append(ramp[v])
            print("    |" + "".join(line) + "|")
        print(f"    墨迹像素 {ink}/{self.cw*self.ch}")
        print()


def overview(f):
    print(f"格子      {f.cw} x {f.ch}   基线 {f.baseline}   网格间距 "
          f"{f.pitch_x} x {f.pitch_y}")
    print(f"图集      {f.sw} x {f.sh}  x{f.n_sheets} 张")
    print(f"每张排布  {f.ncols} 列 x {f.nrows} 行 = {f.per_sheet} 字")
    print(f"容量      {f.per_sheet * f.n_sheets} 字形")
    print()
    print("提示:汉字在 12px 附近本来就有物理上限,复杂字会并笔。")
    print("     判断标准不是「好不好看」,而是**和同一个字在纹理里的样子对比** ——")
    print("     屏幕上比纹理里更缺,才是绘制端的问题。")


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    f = Font(sys.argv[1])

    if len(sys.argv) > 2 and sys.argv[2] == "--char":
        chars = sys.argv[3] if len(sys.argv) > 3 else "永国蒙"
        overview(f)
        print("-" * 60)
        cm = f.cmap()
        print(f"CMAP 收录 {len(cm)} 个码点")
        print()
        for c in chars:
            code = ord(c)
            if code > 0xFFFF:
                print(f"  {c}: 超出 BMP,bcfnt 存不下"); continue
            if code not in cm:
                print(f"  {c}: **字体里没有这个字**(subset 时漏了?)"); continue
            f.draw_cell(cm[code], c)
        return

    overview(f)
    x0 = int(sys.argv[2]) if len(sys.argv) > 2 else 0
    y0 = int(sys.argv[3]) if len(sys.argv) > 3 else 0
    w = int(sys.argv[4]) if len(sys.argv) > 4 else min(f.sw - x0, 128)
    h = int(sys.argv[5]) if len(sys.argv) > 5 else min(f.sh - y0, f.pitch_y * 2)
    ramp = ".123456789abcde#"
    print(f"图集0 ({x0},{y0}) 起 {w}x{h}:")
    for y in range(y0, min(y0 + h, f.sh)):
        print("  " + "".join(ramp[f.px(0, x, y)]
                             for x in range(x0, min(x0 + w, f.sw))))


if __name__ == "__main__":
    main()
