#!/bin/bash
# install-deps.sh — 在无网络的 openEuler 24.03 SP3 目标机上, 离线安装由
# download-deps.sh 下载的全部 RPM。
#
# 用法 (在本脚本所在目录下, 与 RPM 同目录执行):
#   ./install-deps.sh [RPM目录]
set -euo pipefail

RPMDIR="${1:-$(pwd)}"
cd "$RPMDIR"

if ! ls ./*.rpm >/dev/null 2>&1; then
    echo "错误: $RPMDIR 下未找到 .rpm 文件" >&2
    exit 1
fi

echo "==> 从 $RPMDIR 离线安装 (禁用所有仓库, 自动处理依赖顺序)..."

# download-deps.sh 用 `dnf download --resolve --alldeps` 拉全整棵依赖树,
# 其中会混入与构建无关、且在 openEuler 上受 dnf protected_packages 保护的
# 系统级包 (bootloader/固件: grub2-*, shim, mokutil, efivar, efibootmgr;
# 核心: glibc, systemd, pam, setup, filesystem, basesystem, shadow, dbus,
# openEuler-release 等)。一次性 `dnf install ./*.rpm` 时, 这些包的版本与
# 目标机已装版本若有细微差异, dnf 会视"升级"为"删除受保护包"而拒绝安装:
#   Problem: this operation would remove the following protected packages:
#   grub2-efi-aa64
# OpenDCDiag 的构建完全不依赖这些包, 故直接不传给 dnf。
#
# 注意: 不能用 `dnf install --exclude=grub2* ./*.rpm` —— dnf 的 --exclude 对
# 命令行显式指定的本地 .rpm 文件参数无效, 它会把匹配的 .rpm 从候选移除,
# 却仍为这些"参数"去仓库找匹配, 在 --disablerepo=* 下报
# "No match for argument: grub2-...rpm"。正确做法是在 shell 层面就过滤掉
# 这些文件, 只把构建必需的 .rpm 传给 dnf。
#
# 排除模式按文件名前缀匹配 (download 的 RPM 文件名 = 包名-版本-发行号.架构.rpm)。
# 只排除会触发 protected 冲突或与构建无关的 bootloader/固件 + 系统核心包。
# 收窄原则: 凡是 dnf protected.d/ 里列名的包, 以及引导加载/initramfs 链上的
# 包, 一律排除; 其余(哪怕构建用不到)留着, 让 dnf 自己处理依赖更稳妥。
EXCLUDE_RE='^(grub2|grubby|shim|mokutil|efivar|efibootmgr|os-prober|dracut|'
EXCLUDE_RE+='kpartx|device-mapper|fuse|'
EXCLUDE_RE+='glibc|glibc-common|glibc-devel|glibc-headers|'
EXCLUDE_RE+='systemd|systemd-libs|systemd-udev|'
EXCLUDE_RE+='setup|filesystem|basesystem|shadow|pam|'
EXCLUDE_RE+='crypto-policies|openEuler-release|openEuler-gpg-keys|openEuler-repos)'

# 构建待安装列表: 所有 .rpm, 去掉文件名匹配排除模式的。
# 用 %f (纯文件名) 做 grep 匹配, 因为 EXCLUDE_RE 按文件名前缀锚定 (^grub2 等);
# 若用 %p (./glibc-...) 则 ^ 锚定在 "./" 上会失配。保留的文件名再拼回完整路径。
mapfile -t KEEP_NAMES < <(find "$RPMDIR" -maxdepth 1 -name '*.rpm' -printf '%f\n' \
                          | grep -ivE "$EXCLUDE_RE" | sort)
KEEP=()
for n in "${KEEP_NAMES[@]}"; do
    KEEP+=("$RPMDIR/$n")
done

if [ ${#KEEP[@]} -eq 0 ]; then
    echo "错误: 过滤后无 RPM 可装 (检查 $RPMDIR 内容与排除模式)" >&2
    exit 1
fi

echo "    RPM 总数: $(ls -1 ./*.rpm 2>/dev/null | wc -l)"
echo "    排除(受保护/无关系统包): $(find . -maxdepth 1 -name '*.rpm' -printf '%f\n' | grep -icE "$EXCLUDE_RE")"
echo "    待装: ${#KEEP[@]} 个"

if ! sudo dnf install --disablerepo=* -y "${KEEP[@]}"; then
    echo "" >&2
    echo "==> dnf 离线安装失败。常见原因与逐级兜底方案:" >&2
    echo "    1) 仍有受保护包冲突(本脚本未覆盖到): 查 dnf 报错里的包名," >&2
    echo "       把它加进脚本的 EXCLUDE_RE, 或临时手动排除后重跑:" >&2
    echo "         sudo dnf install --disablerepo=* -y \\" >&2
    echo "           \$(find . -maxdepth 1 -name '*.rpm' | grep -ivE '<包名>') \\" >&2
    echo "    2) 允许替换受保护包(注意: 会改动系统关键包, 离线环境有风险):" >&2
    echo "         sudo dnf install --disablerepo=* -y --allowerasing \"\${KEEP[@]}\"" >&2
    echo "    3) 依赖树不完整(下载机与目标机版本差异大): 纯 rpm 强装跳过依赖," >&2
    echo "       仅装构建工具链, 不动系统包:" >&2
    echo "         sudo rpm -Uvh --nodeps --force \"\${KEEP[@]}\"" >&2
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
