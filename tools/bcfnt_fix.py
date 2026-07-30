#!/usr/bin/env python3
"""修 mkbcfnt 的一个 bug:FINF 里的「默认字宽」字段没初始化。

这个字段(FINF+12 起 3 字节:left / glyphWidth / charWidth)是**字体里没有
的字**回落到替代字形时用的宽度。mkbcfnt 不填它,留下的是栈上的脏数据,
于是缺字的地方宽度乱跳、和相邻字重叠或者留下大洞。

正确的值就是替代字形(FINF+10 的 alterCharIndex)自己的宽度,从 CWDH 表里
抄过来即可。转完字体跑一遍这个脚本。

    python3 tools/bcfnt_fix.py romfs/font.bcfnt

(这个问题是从 ClouDS-Music 的 tools/normalize_bcfnt.py 学来的,该项目 MIT。
 这里是按同一思路独立重写的实现。)
"""
import struct
import sys
from pathlib import Path


def fix(data: bytearray):
    if len(data) < 52 or data[:4] != b"CFNT":
        raise ValueError("不是 bcfnt 文件")
    header_size = struct.unpack_from("<H", data, 6)[0]
    file_size = struct.unpack_from("<I", data, 12)[0]
    if header_size + 32 > len(data):
        raise ValueError("bcfnt 头部长度不合法")
    if file_size != len(data):
        raise ValueError(f"文件长度对不上:头里写 {file_size},实际 {len(data)}")

    finf = header_size
    if data[finf:finf + 4] != b"FINF":
        raise ValueError("找不到 FINF 块")

    alter = struct.unpack_from("<H", data, finf + 10)[0]   # 替代字形序号
    default_at = finf + 12                                 # 待修的 3 字节
    cwdh = struct.unpack_from("<I", data, finf + 20)[0]

    # CWDH 是链表,每块覆盖一段连续的字形序号。
    # 注意 bcfnt 的块指针指向「块头之后」,所以块头在 cwdh-8。
    while cwdh:
        if cwdh < 8 or cwdh + 8 > len(data) or data[cwdh - 8:cwdh - 4] != b"CWDH":
            raise ValueError("CWDH 指针不合法")
        start, end, nxt = struct.unpack_from("<HHI", data, cwdh)
        if end < start or cwdh + 8 + (end - start + 1) * 3 > len(data):
            raise ValueError("CWDH 区间越界")
        if start <= alter <= end:
            src = cwdh + 8 + (alter - start) * 3
            old = bytes(data[default_at:default_at + 3])
            new = bytes(data[src:src + 3])
            data[default_at:default_at + 3] = new
            return alter, old, new
        cwdh = nxt
    raise ValueError(f"CWDH 里找不到替代字形 #{alter} 的宽度")


def main() -> int:
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    path = Path(sys.argv[1])
    data = bytearray(path.read_bytes())
    alter, old, new = fix(data)
    if old == new:
        print(f"{path}: 默认字宽已经是对的 {tuple(new)},没改动")
        return 0
    path.write_bytes(bytes(data))
    print(f"{path}: 替代字形 #{alter},默认字宽 {tuple(old)} -> {tuple(new)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
