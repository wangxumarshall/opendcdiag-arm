#!/bin/bash
# build-images.sh — 构建并发布"烘焙好构建依赖"的容器镜像层(方案 2 核心)。
#
# 把 container-build.sh 每次都要做的"装 ~300 个 RPM 依赖"固化为镜像层:
#   一次 build-images.sh → 之后每次源码构建(改一行 framework/)启动容器即 deps 就绪,
#   container-build.sh 跳过整段依赖安装(杠杆 1)。镜像本身不入 git,放 Registry;
#   Containerfile.template + RPM submodule pin 入仓,可复现任意历史镜像。
#
# 用法:
#   build-images.sh <series> <sp> [options]
#     series: 24.03 | 22.03 | 20.03
#     sp    : LTS | SP1 | SP2 | SP3 | SP4
#   options:
#     --push     构建后推 ghcr.io(默认只构建到 localhost tag)
#     --tar      额外导出 oci tarball 到 dist/images/(气隙用)
#     --force    忽略 manifest skip 判定,强制重建
#     --no-build 只算 manifest / 探镜像是否存在,不构建
#
#   build-images.sh --all [--push]   构建/推送全部 15 个 SP
#   build-images.sh --list           打印 image-manifest.tsv
#
# 镜像 tag 命名(与 container-build.sh 的 IMG 变量对齐,确保下游无缝):
#   本地:  localhost/openeuler-offline:24.03-LTS-SP3   ← container-build.sh 用这个
#   远端:  ghcr.io/wangxumarshall/opendcdiag-offline:24.03-LTS-SP3   ← --push 时推
#
# 幂等:Containerfile.template + RPM submodule pin + 版本宏 都不变 → 输入哈希不变 →
#   manifest 已有 + 镜像在(本地或 Registry)→ skip。改 RPM 或 Containerfile 才重建。
#
# SPDX-License-Identifier-Identifier: Apache-2.0
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"  # scripts/offline-build/images/ → 仓根
IMG_DIR="$SCRIPT_DIR"                          # Containerfile.template / manifest 在此
CONTAINERFILE="$IMG_DIR/Containerfile.template"
MANIFEST="$IMG_DIR/image-manifest.tsv"
GHCR_USER="${GHCR_USER:-wangxumarshall}"       # ghcr.io 命名空间(可用 env 覆盖)
GHCR="ghcr.io/${GHCR_USER}/opendcdiag-offline"
LOCAL_PREFIX="localhost/openeuler-offline"

# ── series/sp → 各标识 ──────────────────────────────────────────────
# SP_LABEL: 本地/远端 tag 后缀 (LTS | LTS-SP1..LTS-SP4),与 container-build.sh 一致
# SP_DIR  : 目录后缀 (LTS | LTS_SP1..),拼 OS_TAG
# QUAY_TAG: quay.io base 镜像 tag,小写 (24.03-lts-sp3 / 20.03-lts-sp4 / 22.03-lts)
# MACRO   : C 预处理宏(空=24.03;OPENEULER_22_03/OPENEULER_20_03)
parse_sp() {
    local series="$1" sp="$2"
    case "$sp" in
        LTS)   SP_LABEL="LTS";    SP_DIR="LTS";    QUAY_TAG="${series}-lts" ;;
        SP[1-4]) SP_LABEL="LTS-$sp"; SP_DIR="LTS_$sp"; QUAY_TAG="${series}-lts-$(echo "$sp" | tr '[:upper:]' '[:lower:]')" ;;
        *) echo "bad sp: $sp" >&2; exit 1 ;;
    esac
    case "$series" in
        24.03) MACRO="" ;;
        22.03) MACRO="OPENEULER_22_03" ;;
        20.03) MACRO="OPENEULER_20_03" ;;
        *) echo "bad series: $series" >&2; exit 1 ;;
    esac
    OS_TAG="openEuler-${series}${SP_DIR}"
    LOCAL_TAG="${LOCAL_PREFIX}:${series}-${SP_LABEL}"
    REMOTE_TAG="${GHCR}:${series}-${SP_LABEL}"
    RPM_DIR="$SRC_ROOT/third-party/rpms/openEuler-${series}/${OS_TAG}"
}

# ── 输入哈希:Containerfile 内容 + RPM submodule pin + 宏 + base tag ──
# submodule pin 用 git ls-tree(即使未 checkout 也能拿到指针 sha)。pin 移动 = RPM 树变。
input_hash() {
    local series="$1" sp="$2"
    local pin
    pin=$(git -C "$SRC_ROOT" ls-tree HEAD "third-party/rpms/openEuler-${series}" 2>/dev/null | awk '{print $3}')
    local cf_sha
    cf_sha=$(sha256sum "$CONTAINERFILE" | awk '{print $1}')
    # 拼: containerfile + submodule-pin + macro + quay-base-tag
    printf '%s|%s|%s|%s\n' "$cf_sha" "${pin:-none}" "${MACRO:-none}" "$QUAY_TAG" | sha256sum | awk '{print $1}'
}

# ── manifest 读写 ────────────────────────────────────────────────────
manifest_ensure() {
    [ -f "$MANIFEST" ] || cat > "$MANIFEST" <<'EOF'
# image-manifest.tsv — 容器镜像构建追溯表。列说明:
# sp-tag | input-hash | containerfile-sha | rpm-pin | macro | quay-base | image-digest | local? | remote? | built-date | size-MB
# 由 build-images.sh 自动维护。input-hash 变化触发重建。
sp-tag	input-hash	containerfile-sha	rpm-pin	macro	quay-base	image-digest	local	remote	built-date	size-MB
EOF
}

# 查 manifest 有无该 sp-tag 的 input-hash 记录。有则打印行,无则空。
# 用共享锁(flock -s)串行化读,避免并发 worker 同时读旧 manifest 都判"无"→都重建。
manifest_find() {
    local tag="$1" hash="$2"
    (
        flock -s 200
        manifest_ensure
        grep -P "^${tag}\t${hash}\b" "$MANIFEST" 2>/dev/null | head -1 || true
    ) 200>"${MANIFEST}.lock"
}

# 追加/更新一行。用 flock 串行化(build-all.sh 并发 worker 同时改 manifest 会丢更新)。
manifest_set() {
    local tag="$1" hash="$2" cfsha="$3" pin="$4" macro="$5" quay="$6" digest="$7" local_ok="$8" remote_ok="$9" date_="${10}" size="${11}"
    (
        flock -x 200
        manifest_ensure
        # 删旧(同 sp-tag 任意 hash),再追加新行
        grep -vP "^${tag}\t" "$MANIFEST" > "$MANIFEST.tmp" || true
        printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
            "$tag" "$hash" "$cfsha" "$pin" "$macro" "$quay" "$digest" "$local_ok" "$remote_ok" "$date_" "$size" >> "$MANIFEST.tmp"
        mv "$MANIFEST.tmp" "$MANIFEST"
    ) 200>"${MANIFEST}.lock"
}

# ── 镜像体积估算(本地镜像,podman image inspect) ──
image_size_mb() {
    local tag="$1"
    podman image inspect "$tag" --format '{{.Size}}' 2>/dev/null | awk '{printf "%.0f", $1/1048576}' || echo "?"
}

# ── 镜像 digest(本地:repoDigests;无则 image ID) ──
image_digest() {
    local tag="$1"
    # repoDigests 优先(与 Registry 一致),无则用 image ID 短形
    podman image inspect "$tag" --format '{{range .RepoDigests}}{{.}}{{end}}' 2>/dev/null \
        | sed 's#.*@##' | head -c 71 || true
    [ -z "$(podman image inspect "$tag" --format '{{range .RepoDigests}}{{.}}{{end}}' 2>/dev/null)" ] && \
        podman image inspect "$tag" --format '{{.Id}}' 2>/dev/null | sed 's#.*:##' | head -c 12 || true
}

# ── 单 SP 构建 ───────────────────────────────────────────────────────
build_one() {
    local series="$1" sp="$2"
    parse_sp "$series" "$sp"

    echo "========== ${series}-${SP_LABEL} =========="
    echo "  quay base : quay.io/openeuler/openeuler:${QUAY_TAG}"
    echo "  local tag : $LOCAL_TAG"
    [ "$PUSH" = 1 ] && echo "  remote tag: $REMOTE_TAG"
    echo "  rpm dir   : $RPM_DIR  ($(ls "$RPM_DIR"/*.rpm 2>/dev/null | wc -l) RPM)"

    if [ ! -d "$RPM_DIR" ]; then
        echo "  跳过:RPM 目录不存在(子模块未 checkout?) — $RPM_DIR" >&2
        # 仍写入 manifest 标记 remote/local 都缺?不,直接跳。
        return 0
    fi
    ls "$RPM_DIR"/*.rpm >/dev/null 2>&1 || { echo "  跳过:目录无 RPM" >&2; return 0; }

    local hash cfsha pin
    hash=$(input_hash "$series" "$sp")
    cfsha=$(sha256sum "$CONTAINERFILE" | awk '{print $1}')
    pin=$(git -C "$SRC_ROOT" ls-tree HEAD "third-party/rpms/openEuler-${series}" 2>/dev/null | awk '{print $3}')
    pin="${pin:-none}"

    # skip 判定
    if [ "$FORCE" != 1 ] && [ "$NOBUILD" != 1 ]; then
        local existing
        existing=$(manifest_find "${series}-${SP_LABEL}" "$hash")
        if [ -n "$existing" ]; then
            local_ok=$(echo "$existing" | awk -F'\t' '{print $8}')
            # 镜像真在本地?(manifest 说有但可能被 podman prune 了)
            if [ "$local_ok" = "yes" ] && podman image exists "$LOCAL_TAG" 2>/dev/null; then
                echo "  skip:input-hash 未变 + 本地镜像存在($(image_size_mb "$LOCAL_TAG")MB)"
                # 即便 skip,--tar 仍导出(用户要的是 tar 包,不是重建)
                if [ "$TAR" = 1 ]; then
                    mkdir -p "$SRC_ROOT/dist/images"
                    local tarpath="$SRC_ROOT/dist/images/opendcdiag-offline-${series}-${SP_LABEL}.oci.tar"
                    [ -f "$tarpath" ] || { echo "  导出 tar(skip): $tarpath"; podman save -o "$tarpath" "$LOCAL_TAG" 2>/dev/null || echo "  ⚠ save 失败" >&2; }
                fi
                return 0
            fi
            echo "  manifest 有记录但本地镜像缺失 → 重建"
        fi
    fi

    if [ "$NOBUILD" = 1 ]; then
        echo "  --no-build:仅探测,不构建"
        return 0
    fi

    # 拉 base(quay SP tag)
    echo "  拉 base: quay.io/openeuler/openeuler:${QUAY_TAG}"
    podman pull "quay.io/openeuler/openeuler:${QUAY_TAG}" >/dev/null

    # podman build
    echo "  构建:podman build ..."
    podman build \
        -f "$CONTAINERFILE" \
        --ignorefile "$IMG_DIR/.containerignore" \
        --build-arg SP_TAG="$QUAY_TAG" \
        --build-arg SERIES="$series" \
        --build-arg SP="$SP_DIR" \
        --build-arg OPENEULER_MACRO="$MACRO" \
        -t "$LOCAL_TAG" \
        "$RPM_DIR" >&2 || { echo "  构建失败" >&2; return 1; }

    # 自检解析:镜像内 gcc/find 可用?
    echo "  自检:容器内 gcc/find ..."
    podman run --rm "$LOCAL_TAG" bash -c 'command -v gcc >/dev/null && gcc --version | head -1 || echo "gcc MISSING"' >&2 || true
    podman run --rm "$LOCAL_TAG" bash -c 'command -v find >/dev/null && echo "find OK" || echo "find MISSING"' >&2 || true

    local local_ok="yes" remote_ok="no" digest="" size="" date_=""
    size=$(image_size_mb "$LOCAL_TAG")
    # date 用构建产物的时间锚点(不用 Date.now,用镜像 Created)
    date_=$(podman image inspect "$LOCAL_TAG" --format '{{.Created}}' 2>/dev/null | sed 's/T/ /;s#\..*##' || echo "unknown")

    # push
    if [ "$PUSH" = 1 ]; then
        echo "  推: $REMOTE_TAG"
        # 给镜像打远端 tag 并 push(需先 podman login ghcr.io,见下方提示)
        podman tag "$LOCAL_TAG" "$REMOTE_TAG" 2>/dev/null
        # push 失败时打印真实 stderr(不吞),便于诊断(未登录/auth 错/token 权限)
        if push_out=$(podman push "$REMOTE_TAG" 2>&1); then
            remote_ok="yes"
            # push 后 repoDigests 才有远端 digest
            digest=$(podman image inspect "$REMOTE_TAG" --format '{{range .RepoDigests}}{{.}}{{end}}' 2>/dev/null | sed 's#.*@##' | head -c 71 || echo "")
        else
            echo "  ⚠ push 失败:" >&2
            echo "$push_out" | grep -iE "error|denied|unauthorized|login|not found" | head -3 >&2
            echo "  (未登录? 跑: echo \$GHCR_TOKEN | podman login ghcr.io -u ${GHCR_USER} --password-stdin)" >&2
        fi
    fi
    [ -z "$digest" ] && digest=$(image_digest "$LOCAL_TAG")

    # tar
    if [ "$TAR" = 1 ]; then
        mkdir -p "$SRC_ROOT/dist/images"
        local tarpath="$SRC_ROOT/dist/images/opendcdiag-offline-${series}-${SP_LABEL}.oci.tar"
        echo "  导出 tar: $tarpath"
        podman save -o "$tarpath" "$LOCAL_TAG" 2>/dev/null || echo "  ⚠ save 失败" >&2
    fi

    # 写 manifest
    manifest_set "${series}-${SP_LABEL}" "$hash" "$cfsha" "$pin" "${MACRO:-none}" "$QUAY_TAG" "$digest" "$local_ok" "$remote_ok" "$date_" "$size"
    echo "  ✓ manifest 更新 (local=$local_ok remote=$remote_ok size=${size}MB)"
}

# ── 参数解析 ─────────────────────────────────────────────────────────
PUSH=0 TAR=0 FORCE=0 NOBUILD=0 ALL=0 LIST=0
SERIES_ARG="" SP_ARG=""

while [ $# -gt 0 ]; do
    case "$1" in
        --push)    PUSH=1; shift ;;
        --tar)     TAR=1; shift ;;
        --force)   FORCE=1; shift ;;
        --no-build) NOBUILD=1; shift ;;
        --all)     ALL=1; shift ;;
        --list)    LIST=1; shift ;;
        --help|-h) sed -n '2,/^$/p' "$0" | sed 's/^# //;s/^#//'; exit 0 ;;
        -*) echo "unknown: $1" >&2; exit 1 ;;
        *)  if [ -z "$SERIES_ARG" ]; then SERIES_ARG="$1"; else SP_ARG="$1"; fi; shift ;;
    esac
done

if [ "$LIST" = 1 ]; then
    manifest_ensure
    column -t -s $'\t' "$MANIFEST" 2>/dev/null || cat "$MANIFEST"
    exit 0
fi

# ── 全量矩阵 ──
ALL_SP="LTS SP1 SP2 SP3 SP4"
ALL_SERIES="20.03 22.03 24.03"

if [ "$ALL" = 1 ]; then
    for s in $ALL_SERIES; do
        for sp in $ALL_SP; do
            build_one "$s" "$sp" || true
        done
    done
    echo "========== 全部完成 =========="
    column -t -s $'\t' "$MANIFEST" 2>/dev/null || cat "$MANIFEST"
    exit 0
fi

# ── 单 SP ──
[ -n "$SERIES_ARG" ] || { echo "usage: $0 <series> <sp> [--push|--tar|--force|--no-build] | --all | --list" >&2; exit 1; }
if [ -z "$SP_ARG" ]; then
    # 只给 series → 构建该系列全部 SP
    for sp in $ALL_SP; do build_one "$SERIES_ARG" "$sp" || true; done
else
    build_one "$SERIES_ARG" "$SP_ARG" || exit 1
fi
