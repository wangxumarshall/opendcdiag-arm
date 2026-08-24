#!/bin/bash
# build-all.sh — 多 openEuler 版本矩阵构建编排器(方案 2 第三个 patch)。
#
# 把"改一行源码 → 全 15 SP 验证"压成一条命令。本 patch 是串行 + smoke/full
# 两档骨架;哈希 skip(杠杆2)、--since(杠杆4)、-P 并行(杠杆3)由后续 patch 叠加。
#
# 用法:
#   build-all.sh [series] [sp] [options]
#     series: 24.03 | 22.03 | 20.03 | all  (默认 all)
#     sp    : LTS | SP1..SP4 | all          (默认 all)
#   options:
#     --smoke   快档:verify-built-pristine.sh smoke (默认)
#     --full    全量:verify-built-pristine.sh full (发布闸门)
#     --no-verify  只构建 + 打包,不验证
#     --jobs N  并行度(本 patch 串行,占位;后续 patch 启用)
#
# 流程(每个目标 SP):
#   1) build-images.sh <s> <sp>     镜像就绪(幂等 skip)
#   2) container-build.sh <s> <sp>  源码构建(杠杆1:镜像已烘焙 deps)
#   3) package-built-artifacts.sh   打进 built/
#   4) verify-built-pristine.sh smoke|full  闸门
#
# SPDX-License-Identifier-Identifier: Apache-2.0
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

# ── SP 矩阵 ──
ALL_SERIES="24.03 22.03 20.03"
ALL_SP="LTS SP1 SP2 SP3 SP4"

# ── 参数 ──
SERIES_ARG="all"; SP_ARG="all"; MODE="smoke"; JOBS=1; NOVERIFY=0
while [ $# -gt 0 ]; do
    case "$1" in
        --smoke)    MODE="smoke"; shift ;;
        --full)     MODE="full";  shift ;;
        --no-verify) NOVERIFY=1;  shift ;;
        --jobs)     JOBS="$2"; shift 2 ;;
        --help|-h) sed -n '2,/^$/p' "$0" | sed 's/^# //;s/^#//'; exit 0 ;;
        -*) echo "unknown: $1" >&2; exit 1 ;;
        *)  if [ "$SERIES_ARG" = "all" ] && [[ "$1" != all ]]; then SERIES_ARG="$1"
            elif [ "$SP_ARG" = "all" ] && [[ "$1" != all ]]; then SP_ARG="$1"
            else shift; fi; shift ;;
    esac
done

# 展开目标列表
if [ "$SERIES_ARG" = "all" ]; then SERIES_LIST="$ALL_SERIES"; else SERIES_LIST="$SERIES_ARG"; fi
if [ "$SP_ARG" = "all" ]; then SP_LIST="$ALL_SP"; else SP_LIST="$SP_ARG"; fi

echo "############ build-all: series=[$SERIES_LIST] sp=[$SP_LIST] mode=$MODE ############"

# ── 单 SP 流程 ──────────────────────────────────────────────────────
build_one() {
    local series="$1" sp="$2"
    local tag="openEuler-${series}$(case "$sp" in LTS) echo LTS;; SP[1-4]) echo LTS_$sp;; esac)"
    echo ""
    echo "==================== $tag ===================="
    # 1) 镜像就绪(幂等:已烘焙则 skip)
    if ! "$SCRIPT_DIR/images/build-images.sh" "$series" "$sp" 2>&1; then
        echo "RESULT: FAIL $tag (image build)" >&2; return 1
    fi
    # 2) 源码构建(杠杆1:镜像已烘焙 deps)
    if ! "$SCRIPT_DIR/container-build.sh" "$series" "$sp" 2>&1; then
        echo "RESULT: FAIL $tag (source build)" >&2; return 1
    fi
    # 3) 打包
    if ! "$SCRIPT_DIR/package-built-artifacts.sh" "$series" "$sp" 2>&1; then
        echo "RESULT: FAIL $tag (package)" >&2; return 1
    fi
    # 4) 闸门
    if [ "$NOVERIFY" != 1 ]; then
        if ! "$SCRIPT_DIR/verify-built-pristine.sh" "$series" "$sp" "$MODE" 2>&1; then
            echo "RESULT: FAIL $tag (verify $MODE)" >&2; return 1
        fi
    fi
    echo "RESULT: PASS $tag"
}

# ── 执行矩阵 ────────────────────────────────────────────────────────
PASS_N=0; FAIL_N=0; FAIL_TAGS=""
for series in $SERIES_LIST; do
    for sp in $SP_LIST; do
        if build_one "$series" "$sp"; then
            PASS_N=$((PASS_N+1))
        else
            FAIL_N=$((FAIL_N+1)); FAIL_TAGS="$FAIL_TAGS ${series}-${sp}"
        fi
    done
done

echo ""
echo "############ 汇总 ############"
echo "PASS: $PASS_N   FAIL: $FAIL_N"
[ -n "$FAIL_TAGS" ] && echo "失败:$FAIL_TAGS"
[ "$FAIL_N" = 0 ]
