#!/bin/bash
# build.sh — 在依赖已就绪的 openEuler 24.03 SP3 aarch64 上构建 OpenDCDiag
# 并做最小功能验证。
#
# 用法:
#   ./build.sh [源码根目录] [构建目录]
set -euo pipefail

SRC="${1:-$(git rev-parse --show-toplevel 2>/dev/null || echo .)}"
BUILD="${2:-$SRC/builddir}"

cd "$SRC"

echo "==> 源码目录: $SRC"
echo "==> 构建目录: $BUILD"

# aarch64 路径: eigen5 仓库自带, 无需系统 eigen3, 也无需 PKG_CONFIG_PATH.
# 保留 PKG_CONFIG_PATH 仅为兼容 x86 路径(若在该分支上构建). 无害.
echo "==> meson setup..."
if [ ! -d "$BUILD" ]; then
    PKG_CONFIG_PATH="$SRC/third-part/eigen5" meson setup "$BUILD" --buildtype=release
else
    PKG_CONFIG_PATH="$SRC/third-part/eigen5" meson setup --reconfigure "$BUILD" --buildtype=release
fi

echo "==> ninja..."
ninja -C "$BUILD"

BIN="$BUILD/opendcdiag"
echo "==> 产物: $(ls -la "$BIN")"

echo "==> 功能验证: 列出测试数"
N=$("$BIN" --list-tests 2>/dev/null | wc -l)
echo "    测试数: $N"
[ "$N" -gt 100 ] || { echo "错误: 测试数异常偏少 ($N), 检查 isal/zlib/zstd 是否装上" >&2; exit 1; }

echo "==> 功能验证: zstd19 单线程"
"$BIN" -e zstd19 -t 2000 -n 1 2>&1 | grep -iE "exit|result" | head -3

echo "==> 完成。二进制: $BIN"
