#!/bin/bash
# download-deps.sh — 在一台有网的 openEuler 24.03 SP3 aarch64 上下载 OpenDCDiag
# 构建所需的全部 RPM（含依赖树），用于拷到无网环境离线安装。
#
# 用法:
#   ./download-deps.sh [输出目录]
#
# 默认输出目录: ./opendcdiag-rpms
set -euo pipefail

OUTDIR="${1:-$(pwd)/opendcdiag-rpms}"
mkdir -p "$OUTDIR"

echo "==> 输出目录: $OUTDIR"

# 必装包（默认 CPU 构建, aarch64）
REQUIRED=(
    gcc
    gcc-c++
    meson
    ninja-build
    perl
    python3
    pkgconf
    binutils
    boost-devel
    zlib-devel
    zstd-devel
    libatomic
    git
)
# isal: 优先 EPOL 的 libisa-l-devel, 不可用则回退 everything 的 libisal-devel
if dnf info libisa-l-devel >/dev/null 2>&1; then
    REQUIRED+=(libisa-l-devel)
else
    REQUIRED+=(libisal-devel)
fi
# 可选包（默认注释掉, 按需启用）
OPTIONAL=(
    openssl-devel     # -Dssl_link_type=dynamic 时需要
    gtest-devel       # 构建 unittests 目标时需要
)

echo "==> 下载必装包 (含依赖)..."
sudo dnf install --downloadonly --resolve --destdir="$OUTDIR" "${REQUIRED[@]}"

echo "==> 下载可选包 (含依赖)..."
sudo dnf install --downloadonly --resolve --destdir="$OUTDIR" "${OPTIONAL[@]}" || \
    echo "   (可选包下载失败不影响默认构建, 忽略)"

echo "==> 完成。将 $OUTDIR 整个目录拷到目标机, 运行 install-deps.sh 安装。"
echo "    RPM 数量: $(ls "$OUTDIR"/*.rpm 2>/dev/null | wc -l)"
