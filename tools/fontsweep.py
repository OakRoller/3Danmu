#!/usr/bin/env python3
"""mkbcfnt -s 档位扫描:哪一档是「像素完美」的?

背景
----
点阵字体(Fusion Pixel / Ark Pixel 这类)每个字形本来就是黑白方格。
mkbcfnt 是把 TTF **重新光栅化**成纹理,`-s` 给的是磅值,和字体的
设计像素没有直接关系。只有当 `-s` 恰好让一个设计像素落在一个纹理
像素上时,输出才是纯黑白;否则一笔会被摊到两个像素上,变成半透明灰
—— 屏幕上就是「发虚」「糊」,而且**这是纹理里就已经糊了,后面
怎么画都救不回来**。

判据:「中间灰占比」
------------------
A4 格式每个像素 4 bit alpha。统计既不是 0(全透)也不是 15(全不透)
的像素占比:
    0.00%  → 光栅化正好落在像素格上,纹理是纯二值的,完美
    >0     → 有多少比例的像素是灰的,越大越糊

用法
----
    python3 tools/fontsweep.py 字体.ttf [起始档 结束档]

实测规律(供参考,仍以本脚本实际输出为准):
    需要的 -s ≈ 设计像素 x 0.75
    12px → -s 9 (可以)   10px → -s 7.5 (不存在,所以 10px 无解)   8px → -s 6
"""
import os
import subprocess
import sys
import tempfile

# A4 = 4bit alpha,BCFNT 里 sheetImageFormat 的编号
FMT_A4 = 11
FMT_A8 = 8
FMT_NAME = {FMT_A4: "A4", FMT_A8: "A8"}


def find_mkbcfnt():
    dkp = os.environ.get("DEVKITPRO", "/opt/devkitpro")
    for p in (os.path.join(dkp, "tools", "bin", "mkbcfnt"), "mkbcfnt"):
        if p == "mkbcfnt" or os.path.exists(p):
            return p
    sys.exit("找不到 mkbcfnt,请设置 $DEVKITPRO")


def parse(path):
    """返回 (cellW, cellH, nSheets, sheetSize, fmt, 中间灰占比 or None)"""
    d = open(path, "rb").read()
    i = d.find(b"TGLP")
    if i < 0:
        return None
    u8 = lambda o: d[i + o]
    u16 = lambda o: int.from_bytes(d[i + o:i + o + 2], "little")
    u32 = lambda o: int.from_bytes(d[i + o:i + o + 4], "little")

    cell_w, cell_h = u8(8), u8(9)
    sheet_size, n_sheets, fmt = u32(12), u16(16), u16(18)
    off = u32(28)

    gray = None
    if fmt in (FMT_A4, FMT_A8):
        blob = d[off:off + sheet_size * n_sheets]
        mid = tot = 0
        if fmt == FMT_A4:
            for b in blob:
                for v in (b & 0xF, b >> 4):
                    tot += 1
                    if v not in (0, 15):
                        mid += 1
        else:
            for v in blob:
                tot += 1
                if v not in (0, 255):
                    mid += 1
        gray = 100.0 * mid / tot if tot else 0.0
    return cell_w, cell_h, n_sheets, sheet_size, fmt, gray


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    ttf = sys.argv[1]
    lo = int(sys.argv[2]) if len(sys.argv) > 2 else 4
    hi = int(sys.argv[3]) if len(sys.argv) > 3 else 12
    mk = find_mkbcfnt()

    print(f"字体: {ttf}")
    print(f"{'-s':>3}  {'格子':>7}  {'图集':>4}  {'总大小':>8}  {'格式':>4}  {'中间灰':>7}")
    print("-" * 48)
    best = []
    with tempfile.TemporaryDirectory() as td:
        for s in range(lo, hi + 1):
            out = os.path.join(td, f"t{s}.bcfnt")
            r = subprocess.run([mk, "-s", str(s), "-o", out, ttf],
                               capture_output=True)
            if r.returncode != 0 or not os.path.exists(out):
                print(f"{s:>3}  转换失败: {r.stderr.decode(errors='replace').strip()[:40]}")
                continue
            info = parse(out)
            if not info:
                print(f"{s:>3}  解析失败(没找到 TGLP)")
                continue
            cw, ch, ns, ss, fmt, gray = info
            total = os.path.getsize(out)
            g = "n/a" if gray is None else f"{gray:5.2f}%"
            mark = "  <= 完美" if gray is not None and gray < 0.005 else ""
            print(f"{s:>3}  {cw:>3}x{ch:<3}  {ns:>4}  {total/1048576:>6.2f}MB  "
                  f"{FMT_NAME.get(fmt, fmt):>4}  {g:>7}{mark}")
            if gray is not None and gray < 0.005:
                best.append((s, cw, ch))

    print()
    if best:
        print("像素完美的档位:")
        for s, cw, ch in best:
            print(f"  -s {s}  格子 {cw}x{ch}  → 屏幕上汉字约 {ch} 像素高")
        print("\n挑一个大小顺眼的,代码里字号常量保持 1.0 不要动。")
    else:
        print("**这个字体没有干净的档位** —— 说明它的设计像素数和 mkbcfnt 的")
        print("磅值对不上(例如 10px 需要 -s 7.5,不存在)。换一个设计像素数,")
        print("别在这里硬挑「看起来最小」的那档,那是糊的。")


if __name__ == "__main__":
    main()
