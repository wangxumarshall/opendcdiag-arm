#!/bin/bash
# install-deps.sh — 在无网络的 openEuler 24.03 SP3 目标机上, 离线安装由
# download-deps.sh 下载的全部 RPM。
#
# 用法 (在本脚本所在目录下, 与 RPM 同目录执行):
#   ./install-deps.sh [RPM目录]
set -euo pipefail

RPMDIR="${1:-$(pwd)}"
cd "$RPMDIR"

if ! ls *.rpm >/dev/null 2>&1; then
    echo "错误: $RPMDIR 下未找到 .rpm 文件" >&2
    exit 1
fi

echo "==> 从 $RPMDIR 离线安装 (禁用所有仓库, 自动处理依赖顺序)..."

# download-deps.sh 用 `dnf download --resolve --alldeps` 拉全整棵依赖树,
# 其中会混入与构建无关的系统级包 (bootloader/固件: grub2-*, shim, mokutil,
# efivar, efibootmgr 等)。这些包在 openEuler 上受 dnf protected_packages 保护,
# 一次性 `dnf install ./*.rpm` 时若其版本与目标机已装版本有细微差异, dnf 会
# 视"升级"为"删除受保护包"而拒绝安装 (报错: "this operation would remove
# the following protected packages: grub2-efi-aa64")。OpenDCDiag 的构建完全不
# 依赖 bootloader/固件, 故直接排除整组, 既绕过保护冲突又不丢构建所需任何东西。
EXCLUDES=(
    --exclude=grub2\*
    --exclude=shim\*
    --exclude=mokutil
    --exclude=efivar
    --exclude=efibootmgr
    --exclude=shim-unsigned\*
    # glibc 是系统核心, 受保护且任何离线 RPM 树里的版本都可能与目标机冲突;
    # 构建只依赖已装的 glibc (devel 头由系统提供), 离线树无需升级它。
    --exclude=glibc
    --exclude=glibc-common
    --exclude=glibc-devel
    --exclude=glibc-headers
)

if ! sudo dnf install --disablerepo=* -y "${EXCLUDES[@]}" ./*.rpm; then
    echo "" >&2
    echo "==> dnf 离线安装失败。常见原因与逐级兜底方案:" >&2
    echo "    1) 仍有受保护包冲突: 在上面的命令后追加更多 --exclude=<包名通配>," >&2
    echo "       或用 --allowerasing 允许替换 (注意: 会改动系统关键包, 离线环境有风险):" >&2
    echo "         sudo dnf install --disablerepo=* -y --allowerasing ./*.rpm" >&2
    echo "    2) 依赖树不完整 (下载机与目标机版本差异大): 纯 rpm 强装跳过依赖," >&2
    echo "       仅装构建工具链, 不动系统包:" >&2
    echo "         sudo rpm -Uvh --nodeps --force ./*.rpm" >&2
    echo "       (会留下依赖不一致, 但能立即装上工具链以继续 build.sh)" >&2
    exit 1
fi

echo "==> 关键工具版本自检:"
for cmd in gcc g++ meson ninja perl python3 pkg-config ar objcopy; do
    if command -v "$cmd" >/dev/null 2>&1; then
        printf "  %-10s %s\n" "$cmd" "$($cmd --version 2>/dev/null | head -1)"
    else
        printf "  %-10s 缺失!\n" "$cmd" >&2
    fi
done
echo "  isal       $(ls /usr/lib64/libisal.a 2>/dev/null || echo '未装 (可选, 但 CRC 测试需要)')"
echo "  boost hdr  $(ls /usr/include/boost/algorithm/string.hpp 2>/dev/null || echo 缺失)"
echo "  eigen5     仓库自带 third-part/eigen5/ (无需系统包)"

echo "==> 安装完成。接下来运行 build.sh 构建项目。"
