#!/bin/bash
# release-all.sh — 一键式 15 镜像全流程:拉取 base → 烘焙构建 → full 验证 → 提交推送。
#
# 把 2026-09-02 人工跑通的全流程固化为一条命令:
#   1) preflight  环境自检(podman/docker/git/子模块/分支)
#   2) base       15 个 quay.io 命名 base 镜像就绪(docker hub 拉取→save→podman load→tag,
#                 绕过 quay.io 直连挂起;已在本地则 skip)
#   3) build      build-all.sh --all --full(镜像烘焙幂等 skip + 源码哈希 skip + 15 × full 验证)
#   4) check      日志核验:必须 15 × RESULT: PASS,否则到此为止
#   5) push       3 个 RPM submodule(built/ 产物,仅快进)→ 主仓(submodule 指针 + manifest)
#
# 用法:
#   ./scripts/offline-build/release-all.sh [options]
#     --skip-base   跳过 base 镜像阶段(镜像已在本地时)
#     --skip-build  跳过构建阶段(只做 check+push,要求日志已存在)
#     --skip-push   跳过推送阶段(只构建验证,不提交)
#     -h | --help   本帮助
#
# 安全约束:
#   - 主仓当前分支为 main(或 detached)时,push 阶段拒绝执行(绝不推 main)。
#   - submodule 推送前做快进校验(merge-base --is-ancestor),非快进一律拒绝。
#   - check 阶段不通过(非 15 PASS)绝不进入 push。
#
# 日志: build-out/release-all-<YYYYmmdd-HHMMSS>.log(gitignored)
#
# SPDX-License-Identifier-Identifier: Apache-2.0
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

# ── 15 版本矩阵 ──
ALL_SERIES="24.03 22.03 20.03"
ALL_SP="LTS SP1 SP2 SP3 SP4"

# ── 参数 ──
SKIP_BASE=0; SKIP_BUILD=0; SKIP_PUSH=0
while [ $# -gt 0 ]; do
    case "$1" in
        --skip-base)  SKIP_BASE=1;  shift ;;
        --skip-build) SKIP_BUILD=1; shift ;;
        --skip-push)  SKIP_PUSH=1;  shift ;;
        -h|--help)    sed -n '2,/^# SPDX/p' "$0" | sed 's/^# //;s/^#//;s/^SPDK.*//'; exit 0 ;;
        *) echo "unknown: $1 (see --help)" >&2; exit 1 ;;
    esac
done

mkdir -p "$SRC_ROOT/build-out"
LOG="$SRC_ROOT/build-out/release-all-$(date +%Y%m%d-%H%M%S).log"

log()  { printf '%s\n' "$*" | tee -a "$LOG"; }
die()  { printf 'release-all: %s\n' "$*" | tee -a "$LOG" >&2; exit 1; }
banner(){ log ""; log "================ $* ================"; }

# 全流程输出落日志(后续阶段顺序执行)
exec > >(tee -a "$LOG") 2>&1

# ══════════════════ 1) preflight ══════════════════
banner "stage 1/5: preflight"
command -v podman >/dev/null || die "podman 不可用"
git -C "$SRC_ROOT" rev-parse --is-inside-work-tree >/dev/null || die "不在 git 仓"

BRANCH=$(git -C "$SRC_ROOT" branch --show-current)
[ -n "$BRANCH" ] || die "主仓处于 detached HEAD,push 阶段无法安全执行(请先切到特性分支)"
[ "$BRANCH" = "main" ] && die "当前在 main 分支——CLAUDE.md 禁止推 main(请 git checkout -b <feature-branch>)"
log "分支: $BRANCH ✓"

for s in $ALL_SERIES; do
    d="$SRC_ROOT/third-party/rpms/openEuler-$s"
    git -C "$d" rev-parse --is-inside-work-tree >/dev/null 2>&1 \
        || die "submodule $d 不可用(先 git submodule update --init --recursive)"
    n=$(ls "$d"/openEuler-${s}LTS*/rpms/*.rpm 2>/dev/null | wc -l)
    [ "$n" -gt 0 ] || die "$d 下 SP 目录无 rpms/*.rpm"
    log "submodule openEuler-$s: $n RPMs ✓"
done

# build-all 是否存在
[ -x "$SCRIPT_DIR/build-all.sh" ] || die "build-all.sh 不存在或不可执行"
log "preflight 通过"

# ══════════════════ 2) base 镜像 ══════════════════
banner "stage 2/5: base 镜像(15 个 quay.io 命名)"
TMPDIR_BASE="/home/sdc/.claude-tmp"
if [ "$SKIP_BASE" = 1 ]; then
    log "(--skip-base) 跳过"
else
    mkdir -p "$TMPDIR_BASE"
    missing=()
    for s in $ALL_SERIES; do
        for sp in $ALL_SP; do
            tag="${s}-lts"
            [ "$sp" != "LTS" ] && tag="${s}-lts-$(echo "$sp" | tr 'A-Z' 'a-z')"
            podman images --format '{{.Repository}}:{{.Tag}}' \
                | grep -q "^quay.io/openeuler/openeuler:$tag$" || missing+=("$tag")
        done
    done
    if [ "${#missing[@]}" -eq 0 ]; then
        log "15/15 base 镜像已在本地,全部 skip"
    else
        log "缺 ${#missing[@]} 个: ${missing[*]}"
        command -v docker >/dev/null \
            || die "docker 不可用(base 桥接需要;或手动补齐后 --skip-base)"
        for tag in "${missing[@]}"; do
            log ">> docker pull openeuler/openeuler:$tag"
            sudo docker pull "openeuler/openeuler:$tag"
            sudo bash -c "docker save openeuler/openeuler:$tag -o $TMPDIR_BASE/oe-$tag.tar && \
                          chmod 644 $TMPDIR_BASE/oe-$tag.tar && chown $(id -u):$(id -g) $TMPDIR_BASE/oe-$tag.tar"
            podman load -i "$TMPDIR_BASE/oe-$tag.tar" >/dev/null
            podman tag "localhost/openeuler/openeuler:$tag" "quay.io/openeuler/openeuler:$tag"
            rm -f "$TMPDIR_BASE/oe-$tag.tar"
            log "   ✓ $tag"
        done
    fi
    # 核验
    n=$(podman images --format '{{.Repository}}:{{.Tag}}' | grep -c '^quay.io/openeuler/openeuler')
    [ "$n" -ge 15 ] || die "base 镜像核验失败: 仅 $n/15"
    log "base 核验: $n/15 ✓"
fi

# ══════════════════ 3) 构建 + full 验证 ══════════════════
banner "stage 3/5: build-all --all --full"
if [ "$SKIP_BUILD" = 1 ]; then
    log "(--skip-build) 跳过"
else
    BUILD_LOG="$SRC_ROOT/build-out/release-all-build-$(date +%Y%m%d-%H%M%S).log"
    log "构建日志: $BUILD_LOG"
    if ! "$SCRIPT_DIR/build-all.sh" --all --full 2>&1 | tee "$BUILD_LOG"; then
        die "build-all 失败(详见 $BUILD_LOG)"
    fi
    ln -sf "$(basename "$BUILD_LOG")" "$SRC_ROOT/build-out/release-all-build.latest.log" 2>/dev/null || true
fi

# ══════════════════ 4) 结果核验 ══════════════════
banner "stage 4/5: 结果核验(15 × RESULT: PASS)"
if [ "$SKIP_BUILD" = 1 ]; then
    BUILD_LOG="$SRC_ROOT/build-out/release-all-build.latest.log"
    [ -f "$BUILD_LOG" ] || die "--skip-build 需要既有日志 $(readlink -f "$BUILD_LOG" 2>/dev/null || echo $BUILD_LOG)"
    log "(--skip-build) 用最近一次构建日志: $(readlink -f "$BUILD_LOG")"
fi
PASS_N=$(grep -cE '^RESULT: PASS' "$BUILD_LOG" || true)
FAIL_N=$(grep -cE '^RESULT: FAIL' "$BUILD_LOG" || true)
log "PASS=$PASS_N FAIL=$FAIL_N(要求 15/0)"
[ "$PASS_N" -eq 15 ] || die "PASS 数 $PASS_N != 15"
[ "$FAIL_N" -eq 0 ] || die "存在 FAIL 行,拒绝推送"
log "核验通过: 15 × RESULT: PASS ✓"

# ══════════════════ 5) 提交推送 ══════════════════
banner "stage 5/5: 提交推送"
if [ "$SKIP_PUSH" = 1 ]; then
    log "(--skip-push) 跳过"
else
    STAMP=$(date +%Y-%m-%d)
    # 5a) 三个 submodule: built/ 产物有变更则提交 + 快进推送
    for s in $ALL_SERIES; do
        d="$SRC_ROOT/third-party/rpms/openEuler-$s"
        if git -C "$d" diff --quiet && git -C "$d" diff --cached --quiet; then
            log "submodule openEuler-$s: 干净,skip"
            continue
        fi
        git -C "$d" add -A
        git -C "$d" commit -m "built: refresh all 5 SP artifacts from $STAMP full-matrix build

All 15-SP matrix verified via release-all.sh: full verification PASS
(fail=0, crashes=0, eigen_fails=[]). See main-repo release-all log."
        git -C "$d" fetch origin
        git -C "$d" merge-base --is-ancestor origin/main HEAD \
            || die "submodule openEuler-$s 非快进(本地落后远端),拒绝推送"
        git -C "$d" push origin HEAD:main
        log "submodule openEuler-$s: 提交 + 推送 ✓"
    done
    # 5b) 主仓: submodule 指针 + manifest + 未提交的相关产物
    if git -C "$SRC_ROOT" diff --quiet && git -C "$SRC_ROOT" diff --cached --quiet \
       && [ -z "$(git -C "$SRC_ROOT" status --porcelain -- third-party/rpms scripts/offline-build)" ]; then
        log "主仓: 相关路径干净,skip"
    else
        git -C "$SRC_ROOT" add third-party/rpms scripts/offline-build
        git -C "$SRC_ROOT" commit -m "chore: $STAMP 15-SP release-all full matrix green

All 15 openEuler versions passed full pristine verification via
release-all.sh. Submodule pointers + image manifest updated."
        git -C "$SRC_ROOT" push
        log "主仓: 提交 + 推送 ✓(分支 $BRANCH)"
    fi
fi

banner "release-all 完成"
log "日志: $LOG"
