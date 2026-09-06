#!/bin/bash
#
# build-ntfsprogs.sh — 构建宿主端 NTFS 工具(mkntfs/ntfsfix/ntfsls/…)
#
# SPDX-License-Identifier: GPL-2.0-or-later
#
# 这些工具不进产品,只用于:
#   1. 生成/校验测试镜像(mkntfs)
#   2. 差分测试的 oracle(ntfsls/ntfsinfo/ntfsclone/ntfsfix)
#
# 产物:build/tools/{bin,sbin,lib}
#
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SRC="$ROOT/refs/ntfs-3g"
OUT="$ROOT/build/tools"

if [ ! -d "$SRC" ]; then
    echo "错误:缺少 $SRC" >&2
    exit 1
fi

command -v glibtoolize >/dev/null || { echo "错误:缺少 libtool" >&2; exit 1; }

cd "$SRC"

if [ ! -f configure ]; then
    [ -f m4/libgcrypt.m4 ] || printf 'AC_DEFUN([AM_PATH_LIBGCRYPT],[:])\n' > m4/libgcrypt.m4
    glibtoolize --force --copy --install
    automake --add-missing --copy --foreign 2>/dev/null || true
    autoheader
    LIBTOOLIZE=glibtoolize autoreconf -fi -I m4
fi

echo "==> configure (host tools, ntfsprogs enabled)"
./configure \
    --disable-shared --enable-static \
    --disable-ntfs-3g --enable-ntfsprogs \
    --disable-crypto \
    --prefix="$OUT" \
    CC=clang \
    CFLAGS="-arch arm64 -isysroot $(xcrun --show-sdk-path) -mmacosx-version-min=13.0 -O2 -g"

echo "==> make ntfsprogs"
make -j"$(sysctl -n hw.ncpu)"

echo "==> 采集产物到 $OUT"
# 不用 make install:libntfs-3g 的 install-exec-hook 在纯静态构建下必挂
mkdir -p "$OUT/sbin" "$OUT/lib"
for t in mkntfs ntfsfix ntfsls ntfsinfo ntfsclone ntfscmp ntfslabel \
         ntfscluster ntfscp ntfsresize ntfsundelete ntfscat ntfswipe \
         ntfstruncate; do
    [ -x "ntfsprogs/$t" ] && cp -f "ntfsprogs/$t" "$OUT/sbin/"
done
cp -f libntfs-3g/.libs/libntfs-3g.a "$OUT/lib/" 2>/dev/null || true

echo "==> 完成:"
ls "$OUT/sbin" 2>/dev/null | head -20
