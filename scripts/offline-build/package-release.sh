#!/bin/bash
# package-release.sh — 把某 SP 的 built/ 打成现场可用的 tarball(方案 2 第七个 patch)。
#
# 与 package-built-artifacts.sh 的分工:
#   package-built-artifacts.sh → 产物进 RPM submodule 的 built/(开发者侧沉淀,
#     跨 PR 可复用, git 追溯)。
#   package-release.sh → 产出现场 tarball(~20MB,只含二进制+libs+run+元数据,
#     不含 RPM 树/镜像),拷到目标机/气隙机用。轻量,多 SP 可同放一目录。
#
# 用法:
#   package-release.sh <series> <sp>
#     series: 24.03 | 22.03 | 20.03
#     sp    : LTS | SP1 | SP2 | SP3 | SP4
#   产物: dist/opendcdiag-openEuler-24.03LTS_SP3-<sha8>.tar.gz
#   含: opendcdiag libs/ run-opendcdiag.sh MANIFEST.tsv VERSION built-index.tsv run.sh
#
# SPDX-License-Identifier-Identifier: Apache-2.0
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"  # scripts/offline-build/ → 仓根

SERIES="${1:?usage: $0 <series> <sp>}"
SP="${2:?usage: $0 <series> <sp>}"
case "$SP" in
    LTS) SP_DIR="LTS" ;; SP[1-4]) SP_DIR="LTS_$SP" ;;
    *) echo "bad SP: $SP" >&2; exit 1 ;;
esac
OS_TAG="openEuler-${SERIES}${SP_DIR}"
BUILT="$SRC_ROOT/third-party/rpms/openEuler-${SERIES}/${OS_TAG}/built"

[ -x "$BUILT/opendcdiag" ] || { echo "二进制不存在: $BUILT/opendcdiag (先跑 build-all.sh)" >&2; exit 1; }

# git sha8 作 tarball 名(溯源)
GIT_SHA=$(git -C "$SRC_ROOT" rev-parse --short=8 HEAD 2>/dev/null || echo "nogit")
DIST="$SRC_ROOT/dist"
mkdir -p "$DIST"
TARBALL="$DIST/opendcdiag-${OS_TAG}-${GIT_SHA}.tar.gz"

# built-index.tsv:全 15 SP 总表(部署匹配用)。从各 SP 的 VERSION 抽取汇总。
INDEX="$SRC_ROOT/scripts/offline-build/built-index.tsv"
generate_index() {
    {
        echo "# built-index.tsv — OpenDCDiag 多版本二进制总表(部署匹配用)。"
        echo "# 由 package-release.sh 自动生成。列: sp-tag | git-sha | built-date | series | sp | cpp_std | macro | binary-sha256 | build-hash"
        echo -e "sp-tag\tgit-sha\tbuilt-date\tseries\tsp\tcpp_std\tmacro\tbinary-sha256\tbuild-hash"
        for s in 20.03 22.03 24.03; do
            for d in "$SRC_ROOT/third-party/rpms/openEuler-$s"/openEuler-${s}LTS*/; do
                v="$d/built/VERSION"
                [ -f "$v" ] || continue
                tag=$(basename "$d")
                gitsha=$(grep '^git:' "$v" | awk '{print $2}')
                bdate=$(grep '^built:' "$v" | awk '{print $2}')
                cpp=$(grep '^cpp_std:' "$v" | awk '{print $2}')
                macro=$(grep '^cpp_std:' "$v" | awk '{print $4}')
                binsha=$(grep '^binary-sha256:' "$v" | awk '{print $2}')
                bhash=$(grep '^build-hash:' "$v" | awk '{print $2}')
                printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$tag" "$gitsha" "$bdate" "$s" \
                    "$(echo "$tag" | sed 's/openEuler-'"$s"'LTS_//;s/openEuler-'"$s"'LTS/LTS/')" \
                    "$cpp" "$macro" "$binsha" "$bhash"
            done
        done
    } > "$INDEX"
}

# 现场入口 run.sh(随 tarball 携带):检测 OS → 精确匹配 built-index → exec
generate_run_sh() {
    cat <<'RUN_EOF'
#!/bin/bash
# run.sh — 目标机入口:检测 openEuler 版本 → 精确匹配 tarball 内二进制 → exec。
# A 路线:无匹配 → 硬停指路重构建,绝不静默用相邻 SP 顶替(placeholder-honesty)。
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"

detect_os() {
    . /etc/os-release 2>/dev/null || { echo "error: no /etc/os-release" >&2; exit 2; }
    [ "$ID" = "openEuler" ] || { echo "error: 仅支持 openEuler (ID=$ID)" >&2; exit 2; }
    # VERSION_ID="24.03" + VERSION="24.03 (LTS-SP3)" → openEuler-24.03LTS_SP3
    local sp=""
    sp=$(echo "$VERSION" | sed -nE 's/.*LTS[-_]?(SP[0-9]+).*/\1/p' | tr '[:upper:]' '[:lower:]')
    [ -z "$sp" ] && sp="LTS" || sp=$(echo "$sp" | tr '[:lower:]' '[:upper:]')
    case "$sp" in
        LTS) echo "openEuler-${VERSION_ID}LTS" ;;
        SP[1-4]) echo "openEuler-${VERSION_ID}LTS_$sp" ;;
    esac
}

OS_TAG=$(detect_os)
INDEX="$HERE/built-index.tsv"
echo "检测到: $OS_TAG"

# 精确匹配
match=$(grep -m1 -P "^${OS_TAG}\t" "$INDEX" 2>/dev/null || true)
[ -n "$match" ] || {
    echo "错误: 本机 $OS_TAG 无匹配二进制。"
    echo "可用版本(见 built-index.tsv):"
    grep -vE '^#|sp-tag' "$INDEX" | awk -F'\t' '{print "  "$1"  (gcc "$3", built "$4")}' 2>/dev/null || true
    echo "请在同版本机上运行 scripts/offline-build/build-all.sh $OS_TAG --full 重建,再 package-release.sh 打包拷来。"
    exit 2
}

echo "匹配: $(echo "$match" | awk -F'\t' '{print "gcc=","binary="substr($8,1,12)"...","build-hash="substr($9,1,12)"..."}')"

# 校验 binary-sha256(防下载损坏)
expected=$(echo "$match" | awk -F'\t' '{print $8}')
if [ -x "$HERE/opendcdiag" ]; then
    actual=$(sha256sum "$HERE/opendcdiag" | awk '{print $1}')
    [ "$actual" = "$expected" ] || { echo "校验失败: binary-sha256 不匹配(expected=$expected actual=$actual)" >&2; exit 1; }
    echo "校验: binary-sha256 ✓"
fi

exec "$HERE/run-opendcdiag.sh" "$@"
RUN_EOF
}

# 暂存目录(打 tar)
STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT
cp "$BUILT/opendcdiag" "$STAGE/"
cp -r "$BUILT/libs" "$STAGE/" 2>/dev/null || mkdir -p "$STAGE/libs"
cp "$BUILT/run-opendcdiag.sh" "$STAGE/"
cp "$BUILT/MANIFEST.tsv" "$STAGE/" 2>/dev/null || true
cp "$BUILT/VERSION" "$STAGE/" 2>/dev/null || true
generate_index; cp "$INDEX" "$STAGE/built-index.tsv"
generate_run_sh > "$STAGE/run.sh"; chmod +x "$STAGE/run.sh"

tar -czf "$TARBALL" -C "$STAGE" .
echo "==> 产出现场 tarball: $TARBALL ($(du -h "$TARBALL" | awk '{print $1}'))"
echo "    含: opendcdiag libs/ run-opendcdiag.sh run.sh MANIFEST.tsv VERSION built-index.tsv"
tar -tzf "$TARBALL" | head
