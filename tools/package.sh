#!/usr/bin/env bash
# 全量重编 + 打发布包。在项目根目录运行:  ./tools/package.sh
#
# 产出两个 zip:
#   3danmu-<版本>-<日期>.zip      给使用者:3dsx / cia / 许可 / 说明
#   3danmu-<版本>-<日期>-src.zip  给所有人:完整源码
#
# **两个必须一起发。** FFmpeg 是静态链接的 LGPL 库,只发二进制不发源码
# 不合规(见 THIRD-PARTY-NOTICES.md 第 1 节)。
set -euo pipefail

cd "$(dirname "$0")/.."
ROOT=$(pwd)

# ---------- 环境 ----------
# devkitPro 的 /etc/profile.d 在 macOS 图形终端里不一定被加载,这里补一次。
export DEVKITPRO=${DEVKITPRO:-/opt/devkitpro}
export DEVKITARM=${DEVKITARM:-$DEVKITPRO/devkitARM}
[ -d "$DEVKITARM" ] || { echo "找不到 devkitARM:$DEVKITARM"; exit 1; }

TARGET=3danmu
VER=$(sed -n 's/.*APP_VERSION "\(.*\)".*/\1/p' source/main.c | head -1)
DATE=$(date +%Y%m%d)
[ -n "$VER" ] || { echo "从 source/main.c 读不到 APP_VERSION"; exit 1; }
echo "==> 3Danmu v$VER  ($DATE)"

# ---------- 发布前检查 ----------
# 这几样漏了不会报错,只会让发布包悄悄地不合规或者跑不起来。
[ -f romfs/font.bcfnt ] || { echo "缺 romfs/font.bcfnt —— 界面会满屏 ?"; exit 1; }
[ -f romfs/pinyin.dic ] || { echo "缺 romfs/pinyin.dic —— 拼音输入法会退化"; exit 1; }
for f in LGPL-2.1.txt OFL-1.1.txt jieba-LICENSE pypinyin-LICENSE \
         jsmn-LICENSE qrcodegen-LICENSE libctru-LICENSE; do
    [ -f "licenses/$f" ] || { echo "缺 licenses/$f —— 见 THIRD-PARTY-NOTICES.md"; exit 1; }
done
grep -q -- '--enable-gpl\|--enable-nonfree' build-ffmpeg-3ds.sh && {
    echo "!! build-ffmpeg-3ds.sh 里出现了 --enable-gpl / --enable-nonfree"
    echo "   那会让整个二进制变成 GPL,与本项目的 MIT 声明冲突。先确认这是有意的。"
    exit 1
} || true

# ---------- 全量重编 ----------
# clean 之后 make:改过 romfs 或 TARGET 名时,增量构建会拿旧产物糊弄你。
echo "==> make clean && make"
make clean >/dev/null
make
echo "==> make cia"
make cia

[ -f "$TARGET.3dsx" ] && [ -f "$TARGET.cia" ] || { echo "构建产物不齐"; exit 1; }

# ---------- 组装发布包 ----------
OUT="release/$TARGET-$VER-$DATE"
rm -rf release && mkdir -p "$OUT"
cp "$TARGET.3dsx" "$TARGET.cia" "$OUT/"
# DEVELOPING.md 也放进去:README 里有十几处链到它,不带上就是一堆死链。
# 一个文本文件而已,不差这点体积。
cp README.md TESTING.md DEVELOPING.md LICENSE THIRD-PARTY-NOTICES.md "$OUT/"
# licenses/ 整个目录必须进包 —— LGPL 和 OFL 要求的是「随二进制附上」,
# 只放在源码仓库里不算数。
cp -r licenses "$OUT/"

# 注意:字体和词库**不单独放进包**,它们已经被 romfs 编进 .3dsx/.cia 了。
# 但许可义务照样成立,所以上面那个 licenses/ 不能省。

cat > "$OUT/安装说明.txt" <<'TXT'
3Danmu —— 非官方视频客户端(Nintendo 3DS 自制程序)

【两种装法,任选其一】
  .3dsx  放到 SD 卡的 /3ds/ 目录,从 Homebrew Launcher 启动
  .cia   用 FBI 安装,之后从主菜单直接进(推荐)

【没声音?】
  需要先从自己的主机导出 DSP 固件(受版权保护,不能随程序分发):
  Luma3DS 里按 L + 下 + SELECT → Miscellaneous options
  → Dump DSP firmware,会生成 /3ds/dspfirm.cdc。做一次就够。

【详细说明】README.md
【帮忙测试】TESTING.md
【许可与第三方组件】LICENSE、THIRD-PARTY-NOTICES.md、licenses/

本项目与哔哩哔哩、任天堂均无任何关联,未获其授权或认可。
仅供学习交流,严禁用于商业用途。
TXT

BIN_ZIP="$ROOT/$TARGET-$VER-$DATE.zip"
( cd release && zip -qr "$BIN_ZIP" "$(basename "$OUT")" )

# ---------- 源码包 ----------
# ffmpeg 源码树不打进去(100MB+),build-ffmpeg-3ds.sh 会重新下载。
# LGPL 要求的是「能重新链接」,附构建脚本 + 版本号即满足。
SRC_ZIP="$ROOT/$TARGET-$VER-$DATE-src.zip"
rm -f "$SRC_ZIP"
( cd .. && zip -qr "$SRC_ZIP" "$(basename "$ROOT")" \
    -x "$(basename "$ROOT")/build/*" \
       "$(basename "$ROOT")/release/*" \
       "$(basename "$ROOT")/.git/*" \
       "$(basename "$ROOT")/ffmpeg-6.1.2/*" \
       "$(basename "$ROOT")/ffmpeg-6.1.2.tar.xz" \
       '*.3dsx' '*.elf' '*.smdh' '*.cia' '*.zip' '*.DS_Store' )

# ---------- 出包自检 ----------
# 排除规则是「黑名单」,写错一条不会报错,只会让包里悄悄少东西。
# 这里正面确认几个必须在的,少了就是坏包。
echo
echo "==> 自检"
fail=0
chk() {  # chk <zip> <关键字> <说明>
    if unzip -l "$1" | grep -q "$2"; then
        echo "   ok   $3"
    else
        echo "   缺!  $3   ($2 不在 $(basename "$1") 里)"
        fail=1
    fi
}
chk "$SRC_ZIP" 'romfs/font.bcfnt'  '源码包含字体(别人 clone 下来才编得出可用的程序)'
chk "$SRC_ZIP" 'romfs/pinyin.dic'  '源码包含词库'
chk "$SRC_ZIP" 'build-ffmpeg-3ds.sh' '源码包含 ffmpeg 构建脚本(LGPL 重新链接的前提)'
chk "$SRC_ZIP" 'licenses/LGPL-2.1'  '源码包含 licenses/'
chk "$BIN_ZIP" "$TARGET.cia"        '二进制包含 cia'
chk "$BIN_ZIP" "$TARGET.3dsx"       '二进制包含 3dsx'
chk "$BIN_ZIP" 'licenses/LGPL-2.1'  '二进制包含 licenses/(LGPL 要求随二进制附上)'
[ "$fail" = 0 ] || { echo; echo "自检没过,别发。"; exit 1; }

echo
echo "==> 完成"
ls -lh "$BIN_ZIP" "$SRC_ZIP"
echo
echo "别忘了留一份 $TARGET.elf —— 用户发来崩溃页时,"
echo "addr2line 需要**和发布版本完全一致**的 elf 才能定位到源码行。"
cp "$TARGET.elf" "release/$TARGET-$VER-$DATE.elf"
echo "已存到 release/$TARGET-$VER-$DATE.elf"
