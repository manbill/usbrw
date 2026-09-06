#!/bin/bash
#
# build-ext-journal.sh — 编译 ext 日志重放静态库(arm64+x86_64)
#
# SPDX-License-Identifier: GPL-2.0-or-later
#
# 产物:build/libext2fs/lib/libextjournal.a(与 libext2fs 同目录,扩展链接)
# 来源:refs/e2fsprogs 的 debugfs/journal.c + e2fsck/{recovery,revoke}.c
#       (上游 debugfs 同款链接组合,-DDEBUGFS 分支)
#
# 为什么独立成库而不进 Xcode 目标:e2fsprogs 头路径(refs/*/lib)含
# uuid/uuid.h,全局 -I 会在系统头解析时顶替 SDK 的同名头,Darwin
# 模块(hfs_format.h 的 uuid_string_t)直接崩。脚本内 -I 是编译期
# 局部的,零污染。
#
set -euo pipefail
ROOT="$(cd "$(dirname "$0")" && pwd)/.." ; ROOT="$(cd "$ROOT" && pwd)"
SRC="$ROOT/src/ntfs3g/bridge/ext_journal_replay.c"
REFS="$ROOT/refs/e2fsprogs"
OUT="$ROOT/build/libext2fs"
SDK="$(xcrun --show-sdk-path)"

[ -f "$REFS/lib/config.h" ] || { echo "错误:缺少 $REFS/lib/config.h(先跑 scripts/build-e2fsprogs.sh)" >&2; exit 1; }

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

for ARCH in arm64 x86_64; do
    echo "==> ext_journal_replay($ARCH)"
    clang -std=gnu11 -O2 -arch "$ARCH" \
        -isysroot "$SDK" -mmacosx-version-min=15.4 \
        -DHAVE_CONFIG_H=1 \
        -I"$REFS/lib" -I"$REFS/e2fsck" -I"$OUT/include" \
        -c "$SRC" -o "$TMP/ext_journal_replay-$ARCH.o"
done

lipo -create "$TMP"/ext_journal_replay-*.o -output "$OUT/lib/libextjournal.a"
echo "==> $OUT/lib/libextjournal.a"
lipo -info "$OUT/lib/libextjournal.a"
