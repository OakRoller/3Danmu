# 第三方组件与许可

本项目自身的代码按 MIT 发布(见 `LICENSE`)。但**发布出去的 `.3dsx` / `.cia`
里静态链接了第三方库、也打包了第三方数据**,它们各自带着自己的义务。

下面按「你需要做什么」而不是「它是什么许可」来组织 —— 前者才是会出问题的地方。

---

## 1. FFmpeg —— LGPL 2.1 或更高

**用途**:H.264 / AAC / MP3 / MJPEG 解码、解封装、重采样、缩放。
**版本**:6.1.2(见 `build-ffmpeg-3ds.sh`)
**版权**:FFmpeg developers
**主页**:https://ffmpeg.org/

**当前配置是 LGPL,不是 GPL。** `build-ffmpeg-3ds.sh` 里**没有**
`--enable-gpl` 也没有 `--enable-nonfree`:

```
--disable-everything
--enable-decoder=h264,aac,aac_latm,mp3,mjpeg
```

> ⚠️ **改构建脚本时注意**:一旦加上 `--enable-gpl`(例如为了 x264、
> 某些滤镜)或 `--enable-nonfree`,**整个二进制就会变成 GPL / 不可分发**,
> 本项目的 MIT 声明也会随之失效。这是最容易在无意中踩到的一步。

**分发二进制时必须做到**:

1. 声明使用了 FFmpeg 并说明它是 LGPL 2.1+(本文件即为该声明);
2. 随附 LGPL 2.1 全文;
3. 因为是**静态链接**,要让使用者有能力换一个 FFmpeg 重新链接
   (LGPL 2.1 第 6 条)。本项目开源 + 附带 `build-ffmpeg-3ds.sh`
   已经满足这一点 —— **但前提是发布二进制的同时也发布对应版本的源码**。
   只丢一个 `.cia` 出去而不给源码,是不合规的。

LGPL 2.1 全文已随仓库分发:**[`licenses/LGPL-2.1.txt`](licenses/LGPL-2.1.txt)**
(取自 FFmpeg 自己仓库里的 `COPYING.LGPLv2.1`)。

---

## 2. 中文字体 —— SIL Open Font License 1.1

**用途**:`romfs/font.bcfnt`,界面全部中文显示。
**来源**:由 Noto Sans CJK SC 子集化(`pyftsubset`)后用 `mkbcfnt` 转换。

**Noto Sans CJK 的许可是 SIL Open Font License 1.1。** OFL 的要求:

- 分发(含嵌入到软件里)时**必须随附 OFL 许可全文和版权声明**;
- **不得单独售卖字体本身**;
- 衍生字体不得使用 Reserved Font Name(如果原字体声明了的话)。

子集化 + 转成 `.bcfnt` 属于**衍生作品**,义务照样成立。

OFL 1.1 全文已随仓库分发:**[`licenses/OFL-1.1.txt`](licenses/OFL-1.1.txt)**
(取自 notofonts/noto-cjk 的 `Sans/LICENSE`)。

**版权声明(从原字体 name 表 nameID 0 读出,已核对):**

```
Copyright © 2014-2021 Adobe (http://www.adobe.com/).
```

原字体的 nameID 13/14 也确认指向 SIL OFL 1.1 与 http://scripts.sil.org/OFL,
所以许可类型无疑义(不是有些版本用过的 Apache 2.0)。

> 为什么要单独抄这一行:Noto CJK 官方的 `LICENSE` 文件里**本身不含版权行**
> (少见但确实如此),版权只写在字体二进制的 name 表里 ——
> 而 `.bcfnt` 转换会把 name 表整个丢掉。所以必须以文本形式补上,
> 否则 OFL 第 2 条的「版权声明 + 本许可」就只满足了一半。
> 这一行已同时写进 [`licenses/OFL-1.1.txt`](licenses/OFL-1.1.txt) 的开头。

---

## 3. 拼音词库 —— 需要确认来源后补齐

**用途**:`romfs/pinyin.dic`,触屏拼音输入法。
**生成方式**:`tools/gen_pinyin_dict.py`,数据源是 **jieba 的词频表**
(`jieba/dict.txt`)+ **pypinyin** 标注读音。

- jieba —— MIT License,https://github.com/fxsjy/jieba
- pypinyin —— MIT License,https://github.com/mozillazg/python-pinyin

MIT 要求**保留版权声明和许可声明**。`pinyin.dic` 是从 jieba 词库派生的数据,
所以随二进制分发时应当带上 jieba 的版权行。

两份 MIT 全文已随仓库分发:
**[`licenses/jieba-LICENSE`](licenses/jieba-LICENSE)**(© 2013 Sun Junyi)、
**[`licenses/pypinyin-LICENSE`](licenses/pypinyin-LICENSE)**
(© 2016 mozillazg, 闲耘)。

---

## 4. devkitPro 工具链与 3DS 库

**用途**:libctru / citro2d / citro3d / devkitARM / newlib。
**许可**:libctru、citro2d、citro3d 为 zlib 许可;newlib 为 BSD 系列多许可。
**主页**:https://devkitpro.org/

zlib 许可对二进制分发**不要求**附带声明(不像 BSD 3-Clause),
但列出来是良好实践,全文已随仓库分发:
**[`licenses/libctru-LICENSE`](licenses/libctru-LICENSE)**。
newlib 的部分组件要求保留版权声明。

---

## 4b. 直接编进源码树的第三方代码(vendored)

这两个在 `source/vendor/`,**代码本身带着完整的版权头**(源码分发已合规),
但 MIT 要求「所有副本或实质部分」都保留声明 —— **二进制也算副本**,
所以许可全文同样随包分发:

| 组件 | 用途 | 许可 |
|---|---|---|
| [jsmn](https://github.com/zserge/jsmn)(© 2010 Serge A. Zaitsev) | JSON 分词 | MIT,[`licenses/jsmn-LICENSE`](licenses/jsmn-LICENSE) |
| [QR-Code-generator](https://github.com/nayuki/QR-Code-generator)(© Project Nayuki) | 登录二维码 | MIT,[`licenses/qrcodegen-LICENSE`](licenses/qrcodegen-LICENSE) |

> 改动过 vendored 代码的话,按惯例要在文件头注明「本文件已被修改」,
> 免得把自己的 bug 算到原作者头上。

---

## 5. 与任天堂的关系

**本项目与任天堂株式会社(Nintendo)无任何关联,未获其授权、认可或赞助。**

- 「Nintendo」「Nintendo 3DS」「New Nintendo 3DS」等名称与标识为任天堂所有。
  本项目仅在描述运行平台时提及,不作为自己的名称或标识使用。
- 本项目**不包含任何任天堂的代码、密钥、固件或美术资源**。
- 音频需要的 `dspfirm.cdc` 是 3DS 的 DSP 固件,**受版权保护、不随本项目
  分发**,必须由使用者从自己的主机导出(README 的测试指南里有步骤)。
  同理不要打包 `boot9.bin`、系统固件或系统字体。
- 运行本项目需要自制固件(CFW)。安装 CFW 可能违反任天堂的用户协议、
  并使保修失效,后果由使用者自行承担。

---

## 6. 参考过的项目(未使用其代码)

- **ClouDS-Music**(MIT)—— `tools/bcfnt_fix.py` 修正 mkbcfnt 默认字宽的
  思路来自其 `normalize_bcfnt.py`。本项目的实现是按同一思路独立重写的,
  没有复制代码。出于礼貌在此致谢。
- **Video_player_for_3DS**(Core-2-Extreme)—— CIA 下 MVD 硬解所需的
  exheader 配置(`Dependency: mvd`)是参考其 `app.rsf` 定位的。属于事实性
  配置信息,不构成著作权客体。

---

## 7. 与哔哩哔哩的关系

**本项目与上海宽娱数码科技有限公司(哔哩哔哩)无任何关联,未获其授权、
认可或赞助。**

- 「哔哩哔哩」「bilibili」「小电视」等名称与标识为其权利人所有。
  本项目**不使用**这些商标作为自己的名称或图标。
- 本项目**不分发任何哔哩哔哩的内容**;所有视频、弹幕、评论均由用户自己的
  账号从官方接口实时获取,数据不经过第三方服务器。
- 本项目**不绕过**任何付费、会员或地区限制,**不去除广告**,
  **不提供批量下载**。
- 使用者需自行遵守哔哩哔哩的用户协议。**使用非官方客户端可能导致账号
  受限**,风险由使用者自行承担。

**仅供学习交流,严禁用于商业用途。**

---

## 待办清单

发布正式版本前需要补齐:

- [x] `licenses/LGPL-2.1.txt`(FFmpeg)
- [x] `licenses/OFL-1.1.txt`(字体许可正文)
- [x] `licenses/jieba-LICENSE` + `licenses/pypinyin-LICENSE`(词库)
- [x] 确认字体许可确为 OFL 1.1(非 Apache 2.0)
- [x] `licenses/jsmn-LICENSE` + `licenses/qrcodegen-LICENSE`(vendored 代码,MIT)
- [x] `licenses/libctru-LICENSE`(devkitPro,zlib)
- [x] **Noto 的版权行**(© 2014-2021 Adobe)—— 已写进第 2 节和
      `licenses/OFL-1.1.txt` 开头
- [ ] 发布二进制时同步发布对应版本源码(LGPL 静态链接的前提)
- [ ] 发布包里带上整个 `licenses/` 目录

> 本文件是按公开的许可条款整理的技术性说明,**不是法律意见**。
> 涉及实际分发时,建议自行核对各许可原文或咨询专业人士。
