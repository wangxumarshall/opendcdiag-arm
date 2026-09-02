#!/bin/bash
# supplement-20.03-gcc10.sh — 为 20.03 各 SP 的 RPM 子目录补充 GCC-10 工具链 + meson 0.59。
#
# 背景: openEuler 20.03 全 SP 的仓库只含 gcc-7.3.0(无 <span>/<=>/<bit>),
# 无法编译 SDCShield 的 C++20/23 源码。20.03-SP4 仓库额外提供 gcc-10.3.0 系列
# (与 22.03 同版本), 故从 20.03-SP4 仓库补 gcc-10 全家桶到每个 20.03 SP 子目录。
# meson: 20.03 自带 0.54 太旧(缺 meson.project_source_root, 0.56+ 才有),
# 从 22.03 仓库补 meson 0.59 noarch 包。
#
# 用法: ./supplement-20.03-gcc10.sh [SP1|SP2|SP3|SP4|LTS|all]
#   默认 all: 处理 20.03 LTS/SP1-SP4 全部子目录。
#
# SPDX-License-Identifier: Apache-2.0
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
BASE20="$SRC_ROOT/third-party/rpms/openEuler-20.03"

# 20.03-SP4 仓库 (gcc-10 来源)
SP4_REPO="https://repo.openeuler.org/openEuler-20.03-LTS-SP4/everything/aarch64/Packages"
# 22.03-SP3 仓库 (meson 0.59 来源)
SP3_22_REPO="https://repo.openeuler.org/openEuler-22.03-LTS-SP3/everything/aarch64/Packages"

# gcc-10 全家桶(从 20.03-SP4 取, 以 SCL "gcc-toolset-10-*" 形式打包,
# 装到 /opt/openEuler/gcc-toolset-10/root/ 下): 编译器 + 运行时 + libstdc++ 头
GCC10_PKGS=(
    gcc-toolset-10-cpp-10.3.0-4.oe2003sp4.aarch64.rpm
    gcc-toolset-10-gcc-10.3.0-4.oe2003sp4.aarch64.rpm
    "gcc-toolset-10-gcc-c++-10.3.0-4.oe2003sp4.aarch64.rpm"
    gcc-toolset-10-libgcc-10.3.0-4.oe2003sp4.aarch64.rpm
    "gcc-toolset-10-libstdc++-10.3.0-4.oe2003sp4.aarch64.rpm"
    "gcc-toolset-10-libstdc++-devel-10.3.0-4.oe2003sp4.aarch64.rpm"
    gcc-toolset-10-libgomp-10.3.0-4.oe2003sp4.aarch64.rpm
    gcc-toolset-10-libatomic-10.3.0-4.oe2003sp4.aarch64.rpm
)
# meson 0.59 (从 22.03-SP3 取)
MESON_PKG=meson-0.59.4-2.oe2203sp3.noarch.rpm

download_one() {
    local url="$1" dest_dir="$2" pkg="$3"
    local out="$dest_dir/$pkg"
    if [ -f "$out" ] && [ "$(stat -c%s "$out" 2>/dev/null || echo 0)" -gt 1000 ]; then
        echo "  ✓ $pkg (已存在)"
        return 0
    fi
    echo "  ↓ $pkg"
    if ! curl -sL "$url/$pkg" -o "$out" 2>/dev/null; then
        echo "    FAILED 下载 $pkg" >&2
        rm -f "$out"
        return 1
    fi
    # 校验是 RPM 而非 404 HTML
    if file "$out" 2>/dev/null | grep -qi "HTML"; then
        echo "    错误: $pkg 下载得到的是 HTML(404?), 跳过" >&2
        rm -f "$out"
        return 1
    fi
    return 0
}

supplement_sp() {
    local sp_dir_name="$1"   # openEuler-20.03LTS_SP3
    local dest="$BASE20/$sp_dir_name/rpms"
    [ -d "$dest" ] || { echo "目录不存在: $dest" >&2; return 1; }
    echo "=== 补充 $sp_dir_name ==="
    for pkg in "${GCC10_PKGS[@]}"; do
        download_one "$SP4_REPO" "$dest" "$pkg" || true
    done
    download_one "$SP3_22_REPO" "$dest" "$MESON_PKG" || true
    echo "  → $dest gcc-10/meson 补充完成"
}

TARGET="${1:-all}"
case "$TARGET" in
    all)
        for sp in openEuler-20.03LTS openEuler-20.03LTS_SP1 openEuler-20.03LTS_SP2 openEuler-20.03LTS_SP3 openEuler-20.03LTS_SP4; do
            supplement_sp "$sp"
        done
        ;;
    LTS|SP1|SP2|SP3|SP4)
        supplement_sp "openEuler-20.03LTS_$TARGET"
        ;;
    *)
        echo "usage: $0 [LTS|SP1|SP2|SP3|SP4|all]" >&2; exit 1
        ;;
esac

echo "=== 完成。每个 SP 目录现含 gcc-10 + meson-0.59 RPM。 ==="
