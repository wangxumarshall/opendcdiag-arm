#!/bin/bash
# download-deps.sh — 在一台有网的 openEuler 24.03 SP3 aarch64 上下载 OpenDCDiag
# 构建所需的全部 RPM（含依赖树），用于拷到无网环境离线安装。
#
# 用法:
#   ./download-deps.sh [输出目录]
#
# 默认输出目录: ./opendcdiag-rpms
# 需要 dnf-plugins-core (提供 `dnf download` 子命令). openEuler 24.03 SP3
# 最小安装默认不含它, 先在有网机上: dnf install -y dnf-plugins-core
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
# 可选包（默认构建不需要, 按需启用: 传 --optional 或取消下面注释）
OPTIONAL=(
    openssl-devel     # -Dssl_link_type=dynamic 时需要
    gtest-devel       # 构建 unittests 目标时需要
)

# 用 `dnf download` 子命令下载, 它才是正确的离线依赖收集方式:
#   --resolve  : 解析依赖关系
#   --alldeps  : 下载全部依赖(含这台机器上已满足的), 而非只下载"缺的"
#   --destdir  : 指定输出目录
#   --exclude  : 排除与构建无关的系统级包(bootloader/固件/glibc), 它们在
#               目标机上受 dnf protected_packages 保护, 离线 install 时版本
#               差异会触发"删除受保护包"冲突而拒绝安装。OpenDCDiag 构建不
#               依赖这些包, 从下载阶段就排除, 减小 RPM 集合并消除冲突面。
# 注意: `dnf install --downloadonly` 只会下载本机"尚未安装"的那部分依赖, 对
# 一台最小化目标机而言依赖是不完整的, 故不采用。`dnf download --resolve
# --alldeps` 则无论本机已装与否都把整棵依赖树拉全。
EXCLUDES=(
    --exclude=grub2\*
    --exclude=shim\*
    --exclude=mokutil
    --exclude=efivar
    --exclude=efibootmgr
    --exclude=shim-unsigned\*
    --exclude=glibc
    --exclude=glibc-common
    --exclude=glibc-devel
    --exclude=glibc-headers
)

download_pkgs() {
    local label="$1"; shift
    local pkgs=("$@")
    echo "==> 下载${label}包 (含完整依赖树, 排除 bootloader/固件/glibc)..."
    dnf download --resolve --alldeps "${EXCLUDES[@]}" --destdir="$OUTDIR" "${pkgs[@]}"
}

download_pkgs "必装" "${REQUIRED[@]}"
download_pkgs "可选" "${OPTIONAL[@]}" || \
    echo "   (可选包下载失败不影响默认构建, 忽略)"

echo "==> 完成。将 $OUTDIR 整个目录拷到目标机, 运行 install-deps.sh 安装。"
echo "    RPM 数量: $(ls "$OUTDIR"/*.rpm 2>/dev/null | wc -l)"
