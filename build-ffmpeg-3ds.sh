#!/bin/bash
# 为 3DS 交叉编译精简版 ffmpeg(只保留 h264/aac 解码 + mp4/flv 解封装),
# 安装到 $DEVKITPRO/portlibs/3ds,之后工程 make 即可链接。
# 用法:./build-ffmpeg-3ds.sh
# 若下载 ffmpeg 源码失败,手动下载 ffmpeg-6.1.2.tar.xz 放到本目录再跑一遍:
#   https://ffmpeg.org/releases/ffmpeg-6.1.2.tar.xz
set -e

: "${DEVKITPRO:=/opt/devkitpro}"
: "${DEVKITARM:=$DEVKITPRO/devkitARM}"
export PATH="$DEVKITARM/bin:$PATH"

if ! command -v arm-none-eabi-gcc >/dev/null; then
	echo "找不到 arm-none-eabi-gcc,请先装 3ds-dev 并确认 DEVKITARM=$DEVKITARM"
	exit 1
fi

FFVER=6.1.2
# 【为什么有两个源】ffmpeg.org 在国内经常**连都连不上**(实测 443 端口
# 直接 connect timeout,不是慢,是不通),而这一步失败会让整个脚本白跑。
# GitHub 的 tag 归档是同一份源码,只是顶层目录叫 FFmpeg-nX.Y.Z,
# 解包后要改个名。codeload 域名比 github.com 少一跳 302。
if [ ! -f "ffmpeg-$FFVER.tar.xz" ] && [ ! -f "ffmpeg-$FFVER.tar.gz" ]; then
	echo "==> 下载 ffmpeg-$FFVER 源码..."
	curl -fLO --connect-timeout 15 "https://ffmpeg.org/releases/ffmpeg-$FFVER.tar.xz" || \
	curl -fL --connect-timeout 15 -o "ffmpeg-$FFVER.tar.gz" \
	     "https://codeload.github.com/FFmpeg/FFmpeg/tar.gz/refs/tags/n$FFVER" || {
		echo "两个源都下不动。手动下载 ffmpeg-$FFVER.tar.xz 放到本目录再跑一遍:"
		echo "  https://ffmpeg.org/releases/ffmpeg-$FFVER.tar.xz"
		exit 1
	}
fi
rm -rf "ffmpeg-$FFVER" "FFmpeg-n$FFVER"
if [ -f "ffmpeg-$FFVER.tar.xz" ]; then
	tar xf "ffmpeg-$FFVER.tar.xz"
else
	tar xf "ffmpeg-$FFVER.tar.gz"
	mv "FFmpeg-n$FFVER" "ffmpeg-$FFVER"
fi
cd "ffmpeg-$FFVER"

echo "==> configure..."
./configure \
	--enable-cross-compile \
	--cross-prefix=arm-none-eabi- \
	--prefix="$DEVKITPRO/portlibs/3ds" \
	--arch=arm --cpu=mpcore --target-os=none \
	--enable-static --disable-shared \
	--disable-runtime-cpudetect --disable-autodetect \
	--disable-programs --disable-doc --disable-debug \
	--disable-avdevice --disable-postproc --disable-avfilter \
	--disable-network --disable-pthreads --disable-w32threads \
	--disable-zlib --disable-bzlib --disable-lzma --disable-iconv \
	--disable-everything \
	--enable-decoder=h264,aac,aac_latm,mp3,mjpeg \
	--enable-demuxer=mov,mp3,flv \
	--enable-parser=h264,aac \
	--enable-bsf=h264_mp4toannexb,extract_extradata \
	--enable-protocol=file \
	--enable-swresample --enable-swscale \
	--disable-neon \
	--extra-cflags="-march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft -O2 -ffunction-sections -Wno-error=incompatible-pointer-types -Wno-error=int-conversion -Wno-error=implicit-function-declaration" \
	--extra-ldflags="-mfloat-abi=hard"

echo "==> make(几分钟)..."
make -j"$(sysctl -n hw.ncpu 2>/dev/null || nproc)"

# 【判据是「装的地方能不能写」,不是「有没有 sudo」】
# 写死 sudo 的话 Windows(devkitPro 自带的 MSYS2)上直接
# "sudo: command not found",前面几分钟的编译全白等。
# 但改成「有 sudo 就用」也是错的:**Windows 11 自带一个默认禁用的
# sudo.exe**,`command -v sudo` 会命中它,然后倒在
# "Sudo 已在此计算机上禁用" —— 而那个目录其实本来就可写。
# 直接问文件系统:找到 prefix 最近的一个已存在的祖先目录,看它可不可写。
PREFIX="$DEVKITPRO/portlibs/3ds"
probe="$PREFIX"
while [ -n "$probe" ] && [ "$probe" != "/" ] && [ ! -e "$probe" ]; do
	probe="$(dirname "$probe")"
done
SUDO=""
if [ ! -w "$probe" ] && command -v sudo >/dev/null; then
	SUDO="sudo"
fi
echo "==> 安装到 $PREFIX${SUDO:+(需要 sudo)}..."
$SUDO make install

echo "==> 完成。回到工程目录 make 即可。"
