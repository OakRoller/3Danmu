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
if [ ! -f "ffmpeg-$FFVER.tar.xz" ]; then
	echo "==> 下载 ffmpeg-$FFVER 源码..."
	curl -LO "https://ffmpeg.org/releases/ffmpeg-$FFVER.tar.xz"
fi
rm -rf "ffmpeg-$FFVER"
tar xf "ffmpeg-$FFVER.tar.xz"
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

echo "==> 安装到 $DEVKITPRO/portlibs/3ds(需要 sudo)..."
sudo make install

echo "==> 完成。回到工程目录 make 即可。"
