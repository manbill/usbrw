#!/bin/bash
#
# build-e2fsprogs.sh — 构建 libext2fs 通用静态库(arm64+x86_64)
#
# SPDX-License-Identifier: GPL-2.0-or-later
#
# 产物:build/libext2fs/lib/libext2fs.a(+libcom_err/libblkid/libuuid)
#       build/libext2fs/include/(ext2fs/ et al)
# 来源:refs/e2fsprogs(v1.47.4 tarball;git clone 被墙时用 sourceforge)
# 布局对齐 build-libntfs.sh(双 pass + lipo)。
#
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SRC="$ROOT/refs/e2fsprogs"
OUT="$ROOT/build/libext2fs"
ARCHS=(arm64 x86_64)

[ -f "$SRC/configure" ] || { echo "错误:缺少 $SRC(下载 v1.47.4 tarball 解压到 refs/e2fsprogs)" >&2; exit 1; }

cd "$SRC"

echo "==> host pass(生成 compile_et/ext2_err.h 等构建期产物)"
# e2fsprogs 的 ext2_err.h 由 compile_et(宿主工具)生成——必须先完整
# 跑一遍本机构建;后续 per-arch 重编时头文件已在源码目录,clean 不删。
./configure --disable-debugfs --disable-e2fsck --disable-resize2fs \
    --disable-imager --disable-defrag --disable-fuse2fs \
    --enable-static --disable-shared \
    --without-libiconv-prefix CC=clang \
    CFLAGS="-I$PWD/lib -isysroot $(xcrun --show-sdk-path) -O2" 2>&1 | tail -2
make -C lib/et -j"$(sysctl -n hw.ncpu)"
make -C lib/ext2fs -j"$(sysctl -n hw.ncpu)"

for ARCH in "${ARCHS[@]}"; do
    echo "==> make libext2fs($ARCH)"
    ./configure --disable-debugfs --disable-e2fsck --disable-resize2fs \
        --disable-imager --disable-defrag --disable-fuse2fs \
        --enable-static --disable-shared \
        --without-libiconv-prefix \
        CC=clang \
        CFLAGS="-I$PWD/lib -arch $ARCH -isysroot $(xcrun --show-sdk-path) -mmacosx-version-min=13.0 -O2 -g -fno-common" \
        LDFLAGS="-arch $ARCH" > /dev/null
    make -C lib/et -j"$(sysctl -n hw.ncpu)"
    make -C lib/ext2fs -j"$(sysctl -n hw.ncpu)"
    make -C lib/uuid libuuid.a -j"$(sysctl -n hw.ncpu)"
    make -C lib/blkid libblkid.a -j"$(sysctl -n hw.ncpu)"
    mkdir -p "$OUT/lib-$ARCH"
    cp -f lib/ext2fs/libext2fs.a "$OUT/lib-$ARCH/"
    cp -f lib/et/libcom_err.a "$OUT/lib-$ARCH/"
    cp -f lib/uuid/libuuid.a "$OUT/lib-$ARCH/"
    cp -f lib/blkid/libblkid.a "$OUT/lib-$ARCH/"
done

echo "==> lipo 合成通用库"
mkdir -p "$OUT/lib" "$OUT/include"
for name in libext2fs.a libcom_err.a libuuid.a libblkid.a; do
    lipo -create -output "$OUT/lib/$name" \
        "$OUT/lib-arm64/$name" "$OUT/lib-x86_64/$name"
done
rm -rf "$OUT/lib-arm64" "$OUT/lib-x86_64"

echo "==> 采集头文件(独立目录,避免源码路径污染系统头)"
mkdir -p "$OUT/include/ext2fs" "$OUT/include/et" "$OUT/include/uuid"
cp -f "$SRC"/lib/ext2fs/*.h "$OUT/include/ext2fs/"
cp -f "$SRC"/lib/et/*.h "$OUT/include/et/"
cp -f "$SRC"/lib/uuid/*.h "$OUT/include/uuid/"
# config.h(e2fsprogs 顶层生成)
cp -f "$SRC/config.h" "$OUT/include/e2fsprogs-config.h"

lipo -info "$OUT/lib/libext2fs.a"
