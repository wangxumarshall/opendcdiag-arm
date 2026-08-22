#!/bin/bash
# install-deps.sh — 在无网络的 openEuler 24.03 SPx 目标机上, 离线安装由
# download-deps.sh 下载的全部 RPM。
#
# 用法:
#   ./install-deps.sh [RPM目录]
#
# 默认从 download-deps.sh 的输出目录 ./opendcdiag-rpms 安装; 也可显式指定
# RPM 所在目录作为第一个参数。
#
# **版本管控**: 读取 RPM 目录里的 .os-version 标记 (download-deps.sh 写入),
# 与目标机 openEuler 版本严格比对; 不一致则拒绝安装并指引到同版本机重下。
# **依赖排序**: 用 rpm -qp --provides/--requires 提取每个候选 RPM 的依赖,
# 建依赖图, tsort 拓扑排序后, 按序分批传给 dnf 安装 (避免乱序导致的
# "依赖未就绪" 问题; 实际 dnf 也会内部排序, 本脚本额外显式排序以确定性)。
set -euo pipefail

# 加载共享的版本检测逻辑
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=_common.sh
source "$SCRIPT_DIR/_common.sh"

require_openeuler
require_min_os

# 默认指向 download-deps.sh 的输出目录
RPMDIR="${1:-$(pwd)/opendcdiag-rpms}"

if [ ! -d "$RPMDIR" ]; then
    echo "错误: RPM 目录不存在: $RPMDIR" >&2
    echo "       用法: $0 [RPM目录]  (默认 ./opendcdiag-rpms)" >&2
    echo "       若尚未下载, 先在与本机同版本的 openEuler 有网机上运行 download-deps.sh。" >&2
    exit 1
fi

# 转成绝对路径: 后面会 cd 进去, 若保留相对路径则 cd 后所有基于 $RPMDIR 的
# 查找 (如 .os-version、find $RPMDIR) 都会因 CWD 变更而失效。
RPMDIR="$(cd "$RPMDIR" && pwd)"

cd "$RPMDIR"

# ---- 版本管控: 严格比对下载标记与目标机版本 ----
OS_TAG="$RPMDIR/.os-version"
if [ -f "$OS_TAG" ]; then
    DOWNLOAD_OS=$(cat "$OS_TAG" | tr -d '[:space:]')
    TARGET_OS=$(detect_os_version_full)
    if [ "$DOWNLOAD_OS" != "$TARGET_OS" ]; then
        echo "错误: 版本不匹配 — 下载机 '$DOWNLOAD_OS' vs 目标机 '$TARGET_OS'" >&2
        echo "       SP3 下载的 RPM 装到 SP4 (或反之) 会触发降级冲突 + glibc-devel" >&2
        echo "       精确版本依赖死结, 无法干净安装。" >&2
        echo "       正解: 在与目标机同版本 ($TARGET_OS) 的 openEuler 有网机上" >&2
        echo "       重跑 download-deps.sh 重新下载 RPM 树, 再拷过来安装。" >&2
        exit 1
    fi
    echo "==> 版本核对通过: $TARGET_OS (下载机与目标机一致)"
else
    echo "警告: $RPMDIR/.os-version 标记缺失 (RPM 树可能是旧版脚本下载的)。" >&2
    echo "       跳过版本核对。若遇降级冲突, 请用新版 download-deps.sh 重下。" >&2
fi

if ! ls ./*.rpm >/dev/null 2>&1; then
    echo "错误: $RPMDIR 下未找到 .rpm 文件" >&2
    exit 1
fi

echo "==> 从 $RPMDIR 离线安装 (禁用所有仓库, 拓扑排序后分批安装)..."

# ---- A 类: 静态排除受保护/无关系统包 ----
# download-deps.sh 用 `dnf download --resolve --alldeps` 拉全整棵依赖树,
# 其中混入 bootloader/固件/系统核心等受 dnf protected_packages 保护的包
# (grub2-*, shim, mokutil, efivar, efibootmgr, dracut, os-prober, kpartx,
# device-mapper, fuse, glibc 本身, systemd, pam, setup, filesystem, ...)
# 它们版本若有差异 dnf 会视升级为删除受保护包而拒绝。OpenDCDiag 不依赖它们。
# glibc-devel / glibc-headers 不排除 (gcc 需要); 版本匹配时它们与目标机 glibc 配对。
EXCLUDE_RE='^(grub2|grubby|shim|mokutil|efivar|efibootmgr|os-prober|dracut|'
EXCLUDE_RE+='kpartx|device-mapper|fuse|fuse-common|fuse-help|'
EXCLUDE_RE+='glibc$|glibc-common$|glibc-headers$|glibc-static$|'
EXCLUDE_RE+='systemd$|systemd-libs$|systemd-udev$|'
EXCLUDE_RE+='setup$|filesystem$|basesystem$|shadow$|pam$|'
EXCLUDE_RE+='crypto-policies$|openEuler-release$|openEuler-gpg-keys$|openEuler-repos$)'

# ---- B 类: 跳过目标机已装同版本包 (减少安装量, 避免无谓重装) ----
# 版本严格匹配前提下, 已装即同版本, 跳过无损。
skip_if_installed() {
    local rpmfile="$1"
    local name
    name=$(rpm -qp --qf '%{NAME}' --noscript --nodigest --nosignature "$rpmfile" 2>/dev/null)
    [ -z "$name" ] && return 1
    if rpm -q "$name" >/dev/null 2>&1; then
        return 0   # 已装, 跳过
    fi
    return 1
}

# 收集候选: 排除 A 类 + 跳过 B 类已装
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
    echo "所有包已安装或被排除, 无需安装。" >&2
    exit 0
fi

TOTAL=$(ls -1 ./*.rpm 2>/dev/null | wc -l)
EXCLUDED_A=$(find . -maxdepth 1 -name '*.rpm' -printf '%f\n' | grep -icE "$EXCLUDE_RE")
echo "    RPM 总数: $TOTAL"
echo "    排除A(受保护/无关系统包): $EXCLUDED_A"
echo "    跳过B(目标机已装): $SKIP_INSTALLED"
echo "    待拓扑排序并安装: ${#KEEP[@]} 个"

# ---- 拓扑排序: 用 rpm provides/requires 建依赖图, tsort 排序 ----
# 思路: 对每个候选 RPM, 提取它提供的符号 (provides: 包名/soname) 和需要的
# 符号 (requires)。若 RPM B 的某 require 符号由 RPM A 提供, 则 A 须先于 B
# 安装 → 建边 A->B。tsort 产出安装顺序。
# requires 里过滤掉: rpmlib(...)、文件路径(/bin/sh 等)、已满足于目标机的符号。
#   对已满足符号 (目标机已装包提供) 不建边, 否则会把已装系统包当依赖卡住。

# 1) 收集所有候选 RPM 的 provides: symbol -> rpmfile
declare -A PROVIDE_MAP
# 2) 收集目标机已装包提供的符号 (避免把系统已满足的依赖当卡点)
SYS_PROVIDES=$(rpm -qa --provides 2>/dev/null | awk '{print $1}' | sort -u)

# 建 PROVIDE_MAP: symbol -> 第一个提供它的 RPM 文件路径
while IFS= read -r line; do
    # line 格式: sym<TAB>rpmfile
    sym=${line%%$'\t'*}
    rfile=${line#*$'\t'}
    [ -z "$sym" ] && continue
    [ -n "${PROVIDE_MAP[$sym]+x}" ] && continue
    PROVIDE_MAP[$sym]=$rfile
done < <(
    for f in "${KEEP[@]}"; do
        rpm -qp --provides --noscript --nodigest --nosignature "$f" 2>/dev/null \
            | awk '{print $1}' | sort -u | while read -r sym; do
            [ -z "$sym" ] && continue
            printf '%s\t%s\n' "$sym" "$f"
        done
    done
)

# 建依赖边: 对每个 RPM 的每个 require 符号, 若某候选 RPM 提供它且不是自身,
# 且该符号未被目标机已装包提供 (否则是系统已满足, 不该卡), 则建边 provider->requirer
EDGES_FILE=$(mktemp)
trap 'rm -f "$EDGES_FILE"' EXIT
for f in "${KEEP[@]}"; do
    name=$(rpm -qp --qf '%{NAME}' --noscript --nodigest --nosignature "$f" 2>/dev/null)
    [ -z "$name" ] && continue
    rpm -qp --requires --noscript --nodigest --nosignature "$f" 2>/dev/null \
        | awk '{print $1}' | sort -u | while read -r req; do
        # 过滤: rpmlib(...)、文件路径依赖 (以 / 开头)、空
        [ -z "$req" ] && continue
        [[ "$req" == rpmlib* ]] && continue
        [[ "$req" == /* ]] && continue
        # 跳过目标机已装包提供的符号 (系统已满足, 不建边)
        if echo "$SYS_PROVIDES" | grep -qxF "$req"; then
            continue
        fi
        # 查候选 RPM 中谁提供该符号
        provider="${PROVIDE_MAP[$req]:-}"
        [ -z "$provider" ] && continue
        [ "$provider" = "$f" ] && continue   # 自身提供, 不建边
        # 边: provider 先于 f 安装
        printf '%s %s\n' "$provider" "$f"
    done
done > "$EDGES_FILE"

# tsort 排序 (忽略环, tsort 会警告但仍输出)
SORTED_FILE=$(mktemp)
trap 'rm -f "$EDGES_FILE" "$SORTED_FILE"' EXIT
if ! tsort "$EDGES_FILE" > "$SORTED_FILE" 2>/dev/null; then
    # tsort 报环警告但通常仍输出部分顺序; 失败则回退到原 KEEP 顺序
    echo "警告: 依赖图含环, tsort 退化为原顺序 (dnf 仍会内部排序)" >&2
    printf '%s\n' "${KEEP[@]}" > "$SORTED_FILE"
fi

# 按拓扑顺序构造安装列表 (tsort 输出中有的, 加上未入图的孤立 RPM)
mapfile -t SORTED < "$SORTED_FILE"
# 加上不在 tsort 输出里的 KEEP 成员 (孤立节点, 无依赖关系)
declare -A IN_SORTED
for s in "${SORTED[@]}"; do IN_SORTED[$s]=1; done
INSTALL_LIST=("${SORTED[@]}")
for f in "${KEEP[@]}"; do
    [ -z "${IN_SORTED[$f]:-}" ] && INSTALL_LIST+=("$f")
done

echo "    拓扑排序完成, 安装顺序示例(前10):"
for f in $(printf '%s\n' "${INSTALL_LIST[@]:0:10}"); do
    echo "      $(rpm -qp --qf '%{NAME}' --noscript --nodigest --nosignature "$f" 2>/dev/null)"
done

# ---- 安装: 按拓扑顺序, 整批传给 dnf (dnf 在已排序基础上解析, 且能聚合冲突) ----
if ! sudo dnf install --disablerepo=* -y "${INSTALL_LIST[@]}"; then
    echo "" >&2
    echo "==> dnf 离线安装失败。逐级兜底方案:" >&2
    echo "    1) 若报版本降级冲突 (cannot install both X sp3 and sp4):" >&2
    echo "       说明 .os-version 标记可能缺失或被绕过。确认下载机与本机同 SP 版本。" >&2
    echo "    2) 若报精确版本依赖缺失 (nothing provides glibc = X.sp3):" >&2
    echo "       glibc-devel 等精确版本依赖无法跨 SP 满足, 必须用同版本 RPM 树。" >&2
    echo "    3) 允许替换受保护包(改动系统关键包, 离线环境有风险):" >&2
    echo "         sudo dnf install --disablerepo=* -y --allowerasing \"\${INSTALL_LIST[@]}\"" >&2
    echo "    4) 按拓扑顺序逐个 rpm 强装跳过依赖 (最后兜底):" >&2
    echo "         for f in \"\${INSTALL_LIST[@]}\"; do sudo rpm -Uvh --nodeps --force \"\$f\"; done" >&2
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
echo "  eigen5     仓库自带 third-party/eigen5/ (无需系统包)"

echo "==> 安装完成。接下来运行 build.sh 构建项目。"
