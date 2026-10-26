#!/bin/bash
# SparkCap FFmpeg 静态裁剪构建脚本（docs/building-ffmpeg.md §4）
# 用法：在 x64 Native Tools 环境下执行
#   C:\msys64\msys2_shell.cmd -defterm -no-start -use-full-path <本脚本> [Release|Debug]
# 前置：src/ 已就位（n9.0.2 源码）、构建工具已装（make/diffutils/nasm/yasm）
set -e

CONFIG="${1:-Release}"
ROOT="$(cd "$(dirname "$0")" && pwd)"
cd "$ROOT/src"

case "$CONFIG" in
  Release) CRT="-MT" ;;
  Debug)   CRT="-MTd" ;;
  *) echo "usage: $0 [Release|Debug]"; exit 1 ;;
esac

echo "== 构建 $CONFIG（CRT $CRT），工具链："
which cl make nasm

./configure \
  --toolchain=msvc \
  --target-os=win32 \
  --arch=x86_64 \
  --enable-static \
  --disable-shared \
  --disable-programs \
  --disable-doc \
  --disable-avdevice \
  --disable-avfilter \
  --disable-swscale \
  --disable-swresample \
  --disable-everything \
  --enable-network \
  --enable-protocol=rtsp,rtp,tcp,udp \
  --enable-demuxer=rtsp,h264,hevc \
  --enable-parser=h264,hevc \
  --enable-decoder=h264,hevc \
  --enable-hwaccel=h264_d3d11va,h264_d3d11va2,hevc_d3d11va,hevc_d3d11va2 \
  --extra-cflags="$CRT" \
  --prefix="$ROOT/install/$CONFIG"

make -j"$(nproc)"
make install

# 产物就位：主工程 CMake 消费 third_party/ffmpeg/include 与 lib/<Config>
#（全部不入库，本地产出本机使用）
mkdir -p "$ROOT/lib/$CONFIG"
cp -f "$ROOT/install/$CONFIG/lib/"*.lib "$ROOT/lib/$CONFIG/"
if [ ! -d "$ROOT/include" ]; then
  cp -r "$ROOT/install/$CONFIG/include" "$ROOT/include"
fi

echo "== 产物："
ls -la "$ROOT/lib/$CONFIG/"
echo "== BUILD $CONFIG DONE"
