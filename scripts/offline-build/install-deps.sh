#!/bin/bash
# install-deps.sh — 在无网络的 openEuler 24.03 SP3 目标机上, 离线安装由
# download-deps.sh 下载的全部 RPM。
#
# 用法:
#   ./install-deps.sh [RPM目录]
#
# 默认从 download-deps.sh 的输出目录 ./opendcdiag-rpms 安装; 也可显式指定
# RPM 所在目录作为第一个参数。
set -euo pipefail

# 默认指向 download-deps.sh 的输出目录 (它在有网机上默认输出到
# $(pwd)/opendcdiag-rpms)。用户可传入第一个参数覆盖。
RPMDIR="${1:-$(pwd)/opendcdiag-rpms}"

if [ ! -d "$RPMDIR" ]; then
    echo "错误: RPM 目录不存在: $RPMDIR" >&2
    echo "       用法: $0 [RPM目录]  (默认 ./opendcdiag-rpms)" >&2
    echo "       若尚未下载, 先在有网机上运行 download-deps.sh。" >&2
    exit 1
fi

cd "$RPMDIR"

if ! ls ./*.rpm >/dev/null 2>&1; then
    echo "错误: $RPMDIR 下未找到 .rpm 文件" >&2
    echo "       先在有网机上运行 download-deps.sh 下载依赖。" >&2
    exit 1
fi

echo "==> 从 $RPMDIR 离线安装 (禁用所有仓库, 自动处理依赖顺序)..."

# download-deps.sh 用 `dnf download --resolve --alldeps` 拉全整棵依赖树,
# 其中会混入两类不该原样喂给 dnf 的包:
#
# A. 受保护/无关系统包 (bootloader/固件/系统核心): grub2-*, shim, mokutil,
#    efivar, efibootmgr, dracut, os-prober, kpartx, device-mapper, fuse,
#    glibc, systemd, pam, setup, filesystem, basesystem, shadow, crypto-
#    policies, openEuler-release 等。它们在 openEuler 上受 dnf
#    protected_packages 保护, 一次性 `dnf install ./*.rpm` 时版本若有差异,
#    dnf 会视"升级"为"删除受保护包"而拒绝:
#      Problem: this operation would remove the following protected packages:
#      grub2-efi-aa64
#    OpenDCDiag 的构建完全不依赖这些包, 直接排除。
#
# B. 与目标机已装版本冲突的"降级"包: 当下载机的 openEuler 版本(如 SP3)与
#    目标机(如 SP4)不一致时, 下载树里的 audit-libs / openssl-libs / rpm /
#    cyrus-sasl-lib / openssl-pkcs11 等 RPM 会比目标机已装的旧, dnf 拒绝降级
#    且目标机的 bind-libs/rng-tools/python3-rpm 等依赖 SP4 版本 → 冲突。
#    对这类包, 保留目标机已装的更新版本, 不传下载的旧 RPM 给 dnf。
#    (正解是下载机与目标机同版本; 此过滤是版本错配时的兜底。)
#
# 注意: 不能用 `dnf install --exclude=X ./*.rpm` —— dnf 的 --exclude 对命令行
# 显式指定的本地 .rpm 文件参数无效, 它会把匹配的 .rpm 从候选移除后仍为这些
# "参数"去仓库找匹配, 在 --disablerepo=* 下报 "No match for argument"。
# 正确做法是在 shell 层面过滤: 先按前缀排除 A 类, 再对每个剩余 RPM 查目标机
# 已装版本, 若已装且版本 >= 下载 RPM 的版本则跳过(避免降级, 见 B 类)。

# A 类: 静态排除模式(按文件名前缀)。不含 glibc-devel/glibc-headers —— 它们
# 是 gcc 的依赖, 目标机最小安装可能没装, 需要装; 若目标机已装则由下面的
# "已装则跳过"逻辑自动处理。
EXCLUDE_RE='^(grub2|grubby|shim|mokutil|efivar|efibootmgr|os-prober|dracut|'
EXCLUDE_RE+='kpartx|device-mapper|fuse|fuse-common|fuse-help|'
EXCLUDE_RE+='glibc$|glibc-common$|glibc-headers$|glibc-static$|'
EXCLUDE_RE+='systemd$|systemd-libs$|systemd-udev$|'
EXCLUDE_RE+='setup$|filesystem$|basesystem$|shadow$|pam$|'
EXCLUDE_RE+='crypto-policies$|openEuler-release$|openEuler-gpg-keys$|openEuler-repos$)'

# B 类: 对每个候选 RPM, 若目标机已装同名包且已装版本不旧于下载 RPM 版本,
# 则跳过(保留目标机版本, 避免降级冲突)。版本比较用 rpmdev-vercmp 或回退到
# 字符串比较; 为零依赖, 这里用 `rpmdev-vercmp` 不存在时退化为 `vercmp` 不可
# 用则直接保留(让 dnf 报错)。更稳妥: 直接查 `rpm -q 包名`, 已装即跳过该 RPM
# (因为目标机已有该包, 离线场景下没必要重装, 重装只会带来版本冲突)。
skip_if_installed() {
    # 入参: RPM 文件路径。输出包名(nevra name)。返回 0=跳过(已装), 1=保留。
    local rpmfile="$1"
    local name
    name=$(rpm -qp --qf '%{NAME}' --noscript --nodigest --nosignature "$rpmfile" 2>/dev/null)
    [ -z "$name" ] && return 1   # 查不到包名, 保守保留
    # 目标机是否已装该包? (rpm -q 对未装返回非 0)
    if rpm -q "$name" >/dev/null 2>&1; then
        return 0   # 已装, 跳过该 RPM (保留目标机版本, 不降级)
    fi
    return 1
}

# 构建待安装列表:
# 1) 按前缀排除 A 类受保护/无关系统包 (用 %f 纯文件名匹配, 因正则 ^锚定文件名)
# 2) 对剩余每个 RPM, 若目标机已装同名包则跳过 (B 类降级冲突兜底)
KEEP=()
SKIP_INSTALLED=0
mapfile -t CANDIDATES < <(find "$RPMDIR" -maxdepth 1 -name '*.rpm' -printf '%f\n' \
                          | grep -ivE "$EXCLUDE_RE" | sort)
for n in "${CANDIDATES[@]}"; do
    if skip_if_installed "$RPMDIR/$n"; then
        SKIP_INSTALLED=$((SKIP_INSTALLED + 1))
    else
        KEEP+=("$RPMDIR/$n")
    fi
done

if [ ${#KEEP[@]} -eq 0 ]; then
    echo "错误: 过滤后无 RPM 可装 (检查 $RPMDIR 内容与排除模式)" >&2
    exit 1
fi

TOTAL=$(ls -1 ./*.rpm 2>/dev/null | wc -l)
EXCLUDED_A=$(find . -maxdepth 1 -name '*.rpm' -printf '%f\n' | grep -icE "$EXCLUDE_RE")
echo "    RPM 总数: $TOTAL"
echo "    排除A(受保护/无关系统包): $EXCLUDED_A"
echo "    跳过B(目标机已装, 避免降级冲突): $SKIP_INSTALLED"
echo "    待装: ${#KEEP[@]} 个"

if ! sudo dnf install --disablerepo=* -y "${KEEP[@]}"; then
    echo "" >&2
    echo "==> dnf 离线安装失败。常见原因与逐级兜底方案:" >&2
    echo "    1) **下载机与目标机 openEuler 版本不一致** (如 SP3 下载、SP4 目标):" >&2
    echo "       这是降级冲突的根因。正解是在与目标机**同版本**的机器上重跑" >&2
    echo "       download-deps.sh 重新下载 RPM 树。版本错配时, 本脚本已自动跳过" >&2
    echo "       目标机已装的包, 但 glibc-devel 等精确版本依赖仍可能无解:" >&2
    echo "         SP3 glibc-devel 要求 glibc = 2.38-84.sp3 (精确), 无法装到 SP4" >&2
    echo "       此时需在 SP4 目标机上补装对应 glibc-devel(若有本地 SP4 源)," >&2
    echo "       或用方案 3 的 rpm --nodeps 强装工具链跳过该约束。" >&2
    echo "    2) 仍有受保护包冲突(本脚本未覆盖到): 查 dnf 报错里的包名," >&2
    echo "       把它的前缀加进脚本的 EXCLUDE_RE 后重跑。" >&2
    echo "    3) 允许替换受保护包(注意: 会改动系统关键包, 离线环境有风险):" >&2
    echo "         sudo dnf install --disablerepo=* -y --allowerasing \"\${KEEP[@]}\"" >&2
    echo "    4) 依赖树不完整(版本差异大): 纯 rpm 强装跳过依赖, 仅装构建工具链:" >&2
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
