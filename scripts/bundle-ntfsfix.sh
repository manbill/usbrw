#!/bin/bash
#
# bundle-ntfsfix.sh — 把静态链接的 ntfsprogs 复制并签名进 app 资源目录
#
# SPDX-License-Identifier: GPL-2.0-or-later
#
# 产物:src/app/resources/{ntfsfix,mkntfs}(随 app 打包;验证/修复/
# 格式化功能用)。依赖:先跑 scripts/build-ntfsprogs.sh。
# 二进制不进版本库。
#
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$ROOT/src/app/resources"

IDENTITY=$(security find-identity -v -p codesigning 2>/dev/null \
    | grep "Apple Development: pan zhang" | head -1 \
    | sed -E 's/.*"(.*)"/\1/')

mkdir -p "$OUT"
for tool in ntfsfix mkntfs; do
    SRC="$ROOT/build/tools/sbin/$tool"
    [ -x "$SRC" ] || { echo "错误:缺少 $SRC(先跑 build-ntfsprogs.sh)" >&2; exit 1; }
    cp -f "$SRC" "$OUT/$tool"
    chmod +x "$OUT/$tool"
    if [ -n "$IDENTITY" ]; then
        codesign --force --sign "$IDENTITY" --options runtime "$OUT/$tool"
        echo "==> $OUT/$tool(签名: $IDENTITY)"
    else
        echo "警告:找不到开发证书,$tool 未签名" >&2
    fi
done
