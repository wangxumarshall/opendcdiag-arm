#!/bin/bash
# container-build.sh — 在 podman openEuler 容器内离线构建 OpenDCDiag-arm 并验证。
#
# 用法:
#   ./container-build.sh <series> <sp> [extra-meson-args...]
#     series: 24.03 | 22.03 | 20.03
#     sp   : LTS | SP1 | SP2 | SP3 | SP4
#
# 示例:
#   ./container-build.sh 24.03 SP3
#   ./container-build.sh 22.03 SP3
#   ./container-build.sh 20.03 SP4
#
# 设计要点(满足 CLAUDE.md 约束):
#   - 源码树只读挂载 (/src),容器内不可写 → 24.03 源码不可能被污染。
#   - 22.03/20.03 的全部适配通过 CXXFLAGS 注入:
#       -DOPENEULER_22.03 (或 OPENEULER_20.03)
#       -include /src/framework/compat/cpp23_polyfill.h   # polyfill,不改既有源码
#       (meson 的 -Dcpp_std=gnu++20 命令行覆盖,不改 meson.build)
#   - meson_version >=1.3 约束: 在容器内构建副本上 sed 临时放宽,host 源码不动。
#   - 产物拷出到 host: build-out/openEuler-XX.03LTS_SPx/ (bin + log + ldd清单)。
#
# SPDX-License-Identifier-Identifier: Apache-2.0
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

SERIES="${1:?usage: $0 <series> <sp> [meson-args]}"
SP="${2:?usage: $0 <series> <sp> [meson-args]}"
shift 2 || true
EXTRA_MESON=("$@")

# 规范化 SP 标签(镜像 tag 用 LTS-SPx,目录名用 LTS_SPx)
case "$SP" in
    LTS)   SP_LABEL="LTS";    SP_DIR="LTS" ;;
    SP[1-4]) SP_LABEL="LTS-$SP"; SP_DIR="LTS_$SP" ;;
    *) echo "bad SP: $SP" >&2; exit 1 ;;
esac

OS_TAG="openEuler-${SERIES}${SP_DIR}"
IMG="localhost/openeuler-offline:${SERIES}-${SP_LABEL}"
RPMDIR_HOST="$SRC_ROOT/third-party/rpms/openEuler-${SERIES}/${OS_TAG}"
OUTDIR_HOST="$SRC_ROOT/build-out/${OS_TAG}"

[ -d "$RPMDIR_HOST" ] || { echo "RPM dir missing: $RPMDIR_HOST" >&2; exit 1; }
ls "$RPMDIR_HOST"/*.rpm >/dev/null 2>&1 || { echo "no rpms in $RPMDIR_HOST" >&2; exit 1; }

echo "==> 镜像: $IMG"
echo "==> RPM 目录(host): $RPMDIR_HOST"
echo "==> 产物输出(host): $OUTDIR_HOST"

# host 端准备输出目录
mkdir -p "$OUTDIR_HOST"

# 容器内入口脚本:装依赖 + 构建 + 测试,所有产物落 /out (映射到 host OUTDIR)
INNER=/tmp/inner-build.sh
cat > "$SRC_ROOT/build-out/inner-build.sh" <<'INNER_EOF'
#!/bin/bash
set -euo pipefail

# 从容器 env 读参数(OS_TAG, EXTRA_MESON 数组, OS_SERIES)
RPMDIR="/rpms"
SRC="/src"
BUILD="/build"
OUT="/out"

echo "===== [1/5] 离线安装依赖 ($RPMDIR) ====="
# 最小镜像缺 find, 先用 rpm 直装 findutils (--nodeps, 依赖已在镜像)。
FINDUTILS_RPM=$(ls "$RPMDIR"/findutils-*.rpm 2>/dev/null | head -1 || true)
if [ -n "$FINDUTILS_RPM" ] && ! command -v find >/dev/null 2>&1; then
    echo "  预装 findutils (镜像缺 find)..."
    rpm -Uvh --nodeps --force "$FINDUTILS_RPM" >/dev/null 2>&1 || true
fi
command -v find >/dev/null 2>&1 || { echo "ERROR: find 仍不可用" >&2; exit 1; }

# 创建构建工作目录 (容器内可写)
mkdir -p "$BUILD" "$OUT"

# 容器是最小化 KIWI 镜像, 系统包(util-linux/libuuid 等)版本与 RPM 树来源子版本号
# 略有差异(如 -31 vs -39), dnf 的精确版本依赖会触发 "protected dnf" 死结。
# 容器是可丢弃环境, 用 rpm --nodeps --force 直接强装整棵依赖树(等价于
# install-deps.sh 的兜底方案 4), 依赖物理上都在 RPM 树里, 强装后能正常工作。
EXCLUDE_RE='grub2|shim|mokutil|efivar|dracut|kpartx|fuse|glibc$|glibc-common|glibc-headers|glibc-static|systemd$|systemd-libs|systemd-udev|setup$|filesystem$|basesystem|shadow|pam|crypto-policies|openEuler-release|openEuler-gpg|openEuler-repos'
KEEP=$(find "$RPMDIR" -maxdepth 1 -name '*.rpm' | grep -ivE "$EXCLUDE_RE")
echo "  强装 $(echo "$KEEP" | wc -l) 个 RPM (--nodeps --force)..."
rpm -Uvh --nodeps --force $KEEP >/tmp/rpm-install.log 2>&1 || true
echo "  安装完成; gcc 版本: $(gcc --version 2>/dev/null | head -1 || echo MISSING)"

echo "===== [2/5] 工具链自检 ====="
for cmd in gcc g++ meson ninja perl python3 pkg-config ar; do
    printf "  %-9s %s\n" "$cmd" "$($cmd --version 2>/dev/null | head -1 || echo MISSING)"
done

echo "===== [3/5] meson setup ====="
# 把只读 /src 拷一份到可写 /build/src,以便对 meson.build 做临时 sed(放宽 meson_version)
# — host 源码不被改动(只读挂载)。
cp -a "$SRC" "$BUILD/src"
SRCW="$BUILD/src"

# 放宽 meson_version >=1.3 → >=0.59 (22.03/20.03 的 meson 是 0.59/0.54)
# 用 perl 精确替换,避免误伤。
if ! perl -0pi -e "s/meson_version\s*:\s*'>=1\.3'/meson_version : '>=0.59'/" "$SRCW/meson.build"; then
    echo "warn: perl sed meson_version failed" >&2
fi
grep -n "meson_version" "$SRCW/meson.build" | head -1

# CXXFLAGS 注入 polyfill 头 + 版本宏 (只对 22.03/20.03;24.03 不注入)
POLYFILL_HDR="$SRCW/framework/compat/cpp23_polyfill.h"
CXXFLAGS_EXTRA=""
if [ -n "${OPENEULER_MACRO:-}" ] && [ -f "$POLYFILL_HDR" ]; then
    CXXFLAGS_EXTRA="-D${OPENEULER_MACRO} -include $POLYFILL_HDR"
    echo "  注入: $CXXFLAGS_EXTRA"
fi

export PKG_CONFIG_PATH="$SRCW/third-party/eigen5"
# CXXFLAGS 让 meson 合并 -D/-include 到 cpp_args
export CXXFLAGS="${CXXFLAGS_EXTRA} ${CXXFLAGS:-}"
export CPPSTD="${CPPSTD:-gnu++23}"

rm -rf "$BUILD/builddir"
meson setup "$BUILD/builddir" "$SRCW" --buildtype=release -Dcpp_std="$CPPSTD" ${EXTRA_MESON_ARGS:-}

echo "===== [4/5] ninja ====="
ninja -C "$BUILD/builddir"

BIN="$BUILD/builddir/opendcdiag"
echo "===== [5/5] 功能验证 ====="
ls -la "$BIN"
N=$("$BIN" --list-tests 2>/dev/null | wc -l)
echo "list-tests: $N"
[ "$N" -gt 100 ] || { echo "ERROR: too few tests ($N)" >&2; exit 1; }

echo "--- zstd19 -n 1 ---"
"$BIN" -e zstd19 -t 2000 -n 1 2>&1 | grep -iE "exit|result" | head -3 || true

# 拷产物到 /out
cp "$BIN" "$OUT/opendcdiag"
ldd "$BIN" 2>/dev/null | awk '{print $1}' | sort -u > "$OUT/ldd-libs.txt" || true
echo "===== 容器内构建完成 ====="
INNER_EOF
chmod +x "$SRC_ROOT/build-out/inner-build.sh"

# 决定注入宏与 cpp_std
# 注: 宏名用 OPENEULER_22_03 / OPENEULER_20_03 (点号→下划线),因 C 预处理器宏标识符
# 不允许含点号 ('.')。语义与目标意图一致,仅隔离 22.03/20.03 适配,不触碰 24.03。
case "$SERIES" in
    24.03) OEU_MACRO="";        CPPSTD="gnu++23" ;;
    22.03) OEU_MACRO="OPENEULER_22_03"; CPPSTD="gnu++20" ;;
    20.03) OEU_MACRO="OPENEULER_20_03"; CPPSTD="gnu++20" ;;
    *) echo "bad series"; exit 1 ;;
esac

# extra meson args (数组转空格串)
EXTRA_MESON_STR="${EXTRA_MESON[*]:-}"

echo "==> 启动 podman 容器构建..."
# ACL(arithmetic_arm 测试)在 meson.build 里硬编码了 host 路径:
#   - 头: /home/sdc/root/arm64-sdc-fuzzing/third_party/arm-opt-install/include
#   - 库: /usr/lib64/libarm_compute.so
#   - clang-rt builtins: /usr/lib/clang/17/lib/aarch64-openEuler-linux-gnu/libclang_rt.builtins.a
# 容器内把这些 host 路径只读 bind 挂到同名位置,使未修改的 meson.build 能找到。
# 仅当 host 上存在时挂载(24.03 容器镜像 == host 同 SP, 这些路径有效)。
ACL_HDR="/home/sdc/root/arm64-sdc-fuzzing/third_party/arm-opt-install/include"
ACL_LIB="/usr/lib64"
CLANG_RT="/usr/lib/clang/17"
MOUNTS=(-v "$SRC_ROOT:/src:ro" -v "$RPMDIR_HOST:/rpms:ro" -v "$OUTDIR_HOST:/out" -v "$SRC_ROOT/build-out/inner-build.sh:$INNER:ro")
[ -d "$ACL_HDR" ] && MOUNTS+=(-v "$ACL_HDR:$ACL_HDR:ro")
[ -d "$ACL_LIB" ] && MOUNTS+=(-v "$ACL_LIB/libarm_compute.so:$ACL_LIB/libarm_compute.so:ro" -v "$ACL_LIB/libarm_compute_graph.so:$ACL_LIB/libarm_compute_graph.so:ro")
[ -d "$CLANG_RT" ] && MOUNTS+=(-v "$CLANG_RT:$CLANG_RT:ro")

timeout 600 podman run --rm --user=0 \
    "${MOUNTS[@]}" \
    -e OPENEULER_MACRO="$OEU_MACRO" \
    -e CPPSTD="$CPPSTD" \
    -e EXTRA_MESON_ARGS="$EXTRA_MESON_STR" \
    "$IMG" \
    bash "$INNER"

echo "==> 产物在: $OUTDIR_HOST"
ls -la "$OUTDIR_HOST"
