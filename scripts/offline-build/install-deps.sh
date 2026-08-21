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
sudo dnf install --disablerepo=* -y ./*.rpm

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
