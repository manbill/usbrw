#!/bin/bash
#
# build-libntfs.sh — 构建 libntfs-3g 静态库(arm64,供 FSKit 扩展链接)
#
# SPDX-License-Identifier: GPL-2.0-or-later
#
# 产物:
#   build/libntfs/lib/libntfs-3g.a
#   build/libntfs/include/ntfs-3g/*.h(+生成的 config.h)
#
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SRC="$ROOT/refs/ntfs-3g"
OUT="$ROOT/build/libntfs"

if [ ! -d "$SRC" ]; then
    echo "错误:缺少 $SRC(先 git clone ntfs-3g 到 refs/,见 README)" >&2
    exit 1
fi

command -v glibtoolize >/dev/null || { echo "错误:缺少 libtool(brew install libtool)" >&2; exit 1; }
command -v autoreconf  >/dev/null || { echo "错误:缺少 autoconf/automake" >&2; exit 1; }

cd "$SRC"

echo "==> 应用本地补丁(patches/*.patch,幂等)"
# ntfs-3g-readdir-resume:修复 ntfs_readdir 断点续枚举的 bmp_pos 失同步
# (FSKit 分页枚举必须;详见 patches/ 内注释)
for p in "$ROOT"/patches/*.patch; do
    [ -e "$p" ] || continue
    if git apply --check "$p" 2>/dev/null; then
        git apply "$p"
        echo "    applied: $(basename "$p")"
    elif git apply --reverse --check "$p" 2>/dev/null; then
        echo "    already applied: $(basename "$p")"
    else
        echo "错误:补丁 $(basename "$p") 无法应用(上游源码已变?)" >&2
        exit 1
    fi
done

echo "==> autoreconf"
if [ ! -f configure ]; then
    # autoconf 2.73 需要先安装 libtool 宏;AM_PATH_LIBGCRYPT 宏缺失会报错,补 stub
    [ -f m4/libgcrypt.m4 ] || printf 'AC_DEFUN([AM_PATH_LIBGCRYPT],[:])\n' > m4/libgcrypt.m4
    glibtoolize --force --copy --install
    automake --add-missing --copy --foreign 2>/dev/null || true
    autoheader
    LIBTOOLIZE=glibtoolize autoreconf -fi -I m4
fi

echo "==> configure + make(arm64)"
mkdir -p "$OUT"
./configure \
    --disable-shared --enable-static \
    --disable-ntfs-3g --disable-ntfsprogs \
    --disable-crypto --disable-nls \
    --prefix="$OUT" \
    CC=clang \
    CFLAGS="-arch arm64 -isysroot $(xcrun --show-sdk-path) -mmacosx-version-min=13.0 -O2 -g"
make -C libntfs-3g -j"$(sysctl -n hw.ncpu)"
cp -f "$SRC/libntfs-3g/.libs/libntfs-3g.a" "$OUT/lib/libntfs-3g-arm64.a"

echo "==> configure + make(x86_64)"
make -C libntfs-3g clean 2>/dev/null || true
./configure \
    --disable-shared --enable-static \
    --disable-ntfs-3g --disable-ntfsprogs \
    --disable-crypto --disable-nls \
    --prefix="$OUT" \
    CC=clang \
    CFLAGS="-arch x86_64 -isysroot $(xcrun --show-sdk-path) -mmacosx-version-min=13.0 -O2 -g"
make -C libntfs-3g -j"$(sysctl -n hw.ncpu)"
cp -f "$SRC/libntfs-3g/.libs/libntfs-3g.a" "$OUT/lib/libntfs-3g-x86_64.a"

echo "==> lipo 合成通用库"
mkdir -p "$OUT/lib" "$OUT/include/ntfs-3g"
lipo -create -output "$OUT/lib/libntfs-3g.a" \
    "$OUT/lib/libntfs-3g-arm64.a" "$OUT/lib/libntfs-3g-x86_64.a"
rm -f "$OUT/lib/libntfs-3g-arm64.a" "$OUT/lib/libntfs-3g-x86_64.a"
cp -f "$SRC"/include/ntfs-3g/*.h "$OUT/include/ntfs-3g/"
cp -f "$SRC/config.h" "$OUT/include/ntfs-3g/config.h"

# ntfs-3g 设备层额外需要的内部头(若安装未覆盖)
for h in device_io.h types.h support.h param.h layout.h; do
    [ -f "$OUT/include/ntfs-3g/$h" ] || cp -f "$SRC/include/ntfs-3g/$h" "$OUT/include/ntfs-3g/" 2>/dev/null || true
done

echo "==> 完成:"
ls -la "$OUT/lib" "$OUT/include/ntfs-3g" | head -30
