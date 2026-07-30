# licenses/

第三方组件的许可原文。**发布二进制(`.3dsx` / `.cia`)时这个目录要一起带上** ——
其中 FFmpeg 的 LGPL 和字体的 OFL 都明确要求随附许可全文。

| 文件 | 对应组件 | 为什么必须 |
|---|---|---|
| `LGPL-2.1.txt` | FFmpeg 6.1.2 | LGPL 2.1 第 1 条:分发时必须附许可副本 |
| `OFL-1.1.txt` | `romfs/font.bcfnt`(Noto Sans CJK 子集) | OFL 第 2 条:每份副本都要含版权声明和本许可 |
| `jieba-LICENSE` | `romfs/pinyin.dic` 的词频数据 | MIT:保留版权声明 |
| `pypinyin-LICENSE` | `romfs/pinyin.dic` 的拼音标注 | MIT:保留版权声明 |
| `jsmn-LICENSE` | `source/vendor/jsmn.h` | MIT:保留版权声明 |
| `qrcodegen-LICENSE` | `source/vendor/qrcodegen.*` | MIT:保留版权声明 |
| `libctru-LICENSE` | libctru / citro2d / citro3d | zlib:不强制,列出是良好实践 |

来源都是各项目自己分发的那一份(FFmpeg 仓库的 `COPYING.LGPLv2.1`、
notofonts/noto-cjk 的 `Sans/LICENSE`、jieba / pypinyin / jsmn 的 `LICENSE`、
QR-Code-generator 的 Readme 许可段、libctru README 的许可段),
不是从第三方转载的版本。

义务的完整说明见上一级的 [THIRD-PARTY-NOTICES.md](../THIRD-PARTY-NOTICES.md)。

---

## 字体版权行(已补齐)

`OFL-1.1.txt` 开头已加上从原字体 name 表(nameID 0)读出的版权行:

```
Copyright © 2014-2021 Adobe (http://www.adobe.com/).
```

**为什么要手工补**:Noto CJK 官方的 `LICENSE` 文件本身不含版权行,
版权只存在于字体二进制的 name 表里,而 `.bcfnt` 转换会把 name 表丢掉。
OFL 第 2 条要的是「版权声明 **+** 本许可」两样,只有许可正文不够。

日后换字体时同样要走这一步:

```bash
pip install fonttools
python3 - <<'EOF'
from fontTools.ttLib import TTFont
f = TTFont("你的字体.otf")
for r in f["name"].names:
    if r.nameID in (0, 13, 14):        # 0=版权 13=许可描述 14=许可URL
        print(r.nameID, "|", r.toUnicode()[:200])
EOF
```

nameID 13 若指向 Apache 2.0 而非 OFL,要换掉整个 `OFL-1.1.txt`,不是补版权行。
