#!/usr/bin/env python3
# 生成 romfs:/pinyin.dic —— 触屏拼音输入法的词库
#
# 数据源:jieba 的词频表(35万词)+ pypinyin 标注拼音
# 格式(小端):
#   "PYD1" | u32 count | u32 offsets[count] | blob
#   blob 条目:u8 pylen | u8 wlen | py[pylen] | word[wlen](UTF-8)
# 条目按 (拼音升序, 词频降序) 排列 → 运行期二分找前缀区间即得按频序候选
import os, re, struct, sys
import jieba
from pypinyin import lazy_pinyin, Style

MAX_WORDS   = 42000   # 词条上限(不含单字)
MAX_WLEN    = 4       # 最长 4 字词
HANZI       = re.compile(r'^[一-鿿]+$')

dict_path = os.path.join(os.path.dirname(jieba.__file__), 'dict.txt')
rows = []
for line in open(dict_path, encoding='utf-8'):
    parts = line.split()
    if len(parts) < 2: continue
    w, freq = parts[0], int(parts[1])
    if not HANZI.match(w) or len(w) > MAX_WLEN: continue
    rows.append((w, freq))

rows.sort(key=lambda r: -r[1])
singles = [(w, f) for w, f in rows if len(w) == 1]
words   = [(w, f) for w, f in rows if len(w) > 1][:MAX_WORDS]
# 单字全收(常用字都在 jieba 表里,约 1 万,频序保留)
chosen = singles + words
print(f'singles={len(singles)} words={len(words)} total={len(chosen)}')

entries = []
for w, f in chosen:
    py = ''.join(lazy_pinyin(w, style=Style.NORMAL, v_to_u=False,
                             errors='ignore'))
    if not py or not py.isascii() or len(py) > 24: continue
    wb = w.encode('utf-8')
    if len(wb) > 24: continue
    entries.append((py.encode(), -f, wb))

entries.sort(key=lambda e: (e[0], e[1]))
blob = bytearray(); offsets = []
for py, nf, wb in entries:
    offsets.append(len(blob))
    blob += bytes((len(py), len(wb))) + py + wb

out = sys.argv[1] if len(sys.argv) > 1 else 'romfs/pinyin.dic'
with open(out, 'wb') as fp:
    fp.write(b'PYD1' + struct.pack('<I', len(offsets)))
    fp.write(struct.pack(f'<{len(offsets)}I', *offsets))
    fp.write(blob)
print(f'{out}: {len(entries)} entries, {os.path.getsize(out)} bytes')
