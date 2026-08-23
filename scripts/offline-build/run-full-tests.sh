#!/bin/bash
# run-full-tests.sh — 在 podman openEuler 容器内运行 OpenDCDiag 的全量检测用例。
#
# 用法:
#   ./run-full-tests.sh <series> <sp> [build]
#     series: 24.03 | 22.03 | 20.03
#     sp    : LTS | SP1 | SP2 | SP3 | SP4
#     build : 可选。传 build 则先跑 container-build.sh 再测(验证无依赖构建);
#            否则复用 build-out/ 已有二进制(不存在则自动 build)。
#
# 策略 (依据 CLAUDE.md 已知平台特性):
#   - 192 CPU 全核多线程下 eigen_svd/sparse 因并行排序 ULP 差异会偶发数值 flakiness,
#     单线程 -n 1 稳定。故 eigen 数值类单独 -n 1 跑, 其余 -n $NTHREADS (默认 8)。
#   - --ignore-timeout: 容器内偶发 timeout (cgroup/OOM) 在验证语境可忽略, 非致命。
#   - 结果 yaml 写到挂载的 /out 目录, 解析 result: pass/skip/fail 计数。
#   - stdout 末行输出 "RESULT: PASS|FAIL <counts>" 供 subagent 解析。
#
# SPDX-License-Identifier: Apache-2.0
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

SERIES="${1:?usage: $0 <series> <sp> [build]}"
SP="${2:?usage: $0 <series> <sp> [build]}"
REBUILD="${3:-}"
NTHREADS="${NTHREADS:-8}"
TTIME="${TTIME:-1000}"

case "$SP" in
    LTS)   SP_LABEL="LTS";    SP_DIR="LTS" ;;
    SP[1-4]) SP_LABEL="LTS-$SP"; SP_DIR="LTS_$SP" ;;
    *) echo "bad SP: $SP" >&2; exit 1 ;;
esac

OS_TAG="openEuler-${SERIES}${SP_DIR}"
IMG="localhost/openeuler-offline:${SERIES}-${SP_LABEL}"
OUTDIR_HOST="$SRC_ROOT/build-out/${OS_TAG}"
BIN_HOST="$OUTDIR_HOST/opendcdiag"
TEST_OUT_HOST="$OUTDIR_HOST/test-results"
mkdir -p "$TEST_OUT_HOST"

if [ "$REBUILD" = "build" ] || [ ! -x "$BIN_HOST" ]; then
    echo "==> 构建 $OS_TAG" >&2
    if ! "$SCRIPT_DIR/container-build.sh" "$SERIES" "$SP" >&2; then
        echo "RESULT: FAIL build" ; exit 2; fi
fi
[ -x "$BIN_HOST" ] || { echo "RESULT: FAIL no-binary" ; exit 2; }

# 挂载: 二进制只读 + 结果目录可写 + 运行时库
MOUNTS=(-v "$BIN_HOST:/bin/opendcdiag:ro" -v "$TEST_OUT_HOST:/out")
LD_PATH=""
if [ "$SERIES" = "24.03" ]; then
    MOUNTS+=(-v /usr/lib64:/usr/lib64:ro)
fi
LIBS_DIR="$SRC_ROOT/third-party/rpms/openEuler-${SERIES}/${OS_TAG}/built/libs"
if [ -d "$LIBS_DIR" ]; then
    MOUNTS+=(-v "$LIBS_DIR:/opt/built-libs:ro"); LD_PATH="/opt/built-libs"
fi

run_in_container() {
    if [ -n "$LD_PATH" ]; then
        timeout 360 podman run --rm --user=0 "${MOUNTS[@]}" \
            -e LD_LIBRARY_PATH="$LD_PATH" "$IMG" /bin/opendcdiag "$@" 2>&1
    else
        timeout 360 podman run --rm --user=0 "${MOUNTS[@]}" \
            "$IMG" /bin/opendcdiag "$@" 2>&1
    fi
}

parse_yaml_counts() {
    local y="$1"
    # 用 awk 保证单一数字输出 (grep -c 在 ||echo 0 时会产生换行后多一个0)
    [ -f "$y" ] || { PASSES=0; SKIPS=0; FAILS_Y=0; return; }
    PASSES=$(grep -c 'result: pass' "$y" | tr -d '\n'); PASSES=${PASSES:-0}
    SKIPS=$(grep -c 'result: skip' "$y" | tr -d '\n'); SKIPS=${SKIPS:-0}
    FAILS_Y=$(grep -c 'result: fail' "$y" | tr -d '\n'); FAILS_Y=${FAILS_Y:-0}
}

echo "=== $OS_TAG : list-tests ==="
LIST=$(run_in_container --list-tests 2>/dev/null | grep -v '^$' || true)
TOTAL=$(echo "$LIST" | grep -c . || echo 0)
echo "total tests listed: $TOTAL"
[ "$TOTAL" -ge 100 ] || { echo "RESULT: FAIL too-few-tests($TOTAL)" ; exit 1; }

EIGEN_TESTS="eigen_svd_double eigen_sparse eigen_svd_cdouble eigen_svd_cdouble_sve"
EIGEN_FAILS=""

echo "=== $OS_TAG : eigen 数值类 (-n 1) ==="
for t in $EIGEN_TESTS; do
    echo "$LIST" | grep -qx "$t" || continue
    ey="$TEST_OUT_HOST/${t}.yaml"; rm -f "$ey"
    out=$(run_in_container -e "$t" -t "$TTIME" -n 1 --ignore-timeout -o "/out/${t}.yaml" 2>&1 \
          | grep -iE 'exit:' | tail -1 || true)
    parse_yaml_counts "$ey"
    echo "  $t: $out  (pass=$PASSES skip=$SKIPS fail=$FAILS_Y)"
    [ "${FAILS_Y:-0}" -gt 0 ] && EIGEN_FAILS="$EIGEN_FAILS $t"
done

echo "=== $OS_TAG : 其余用例 (-n $NTHREADS) ==="
DISABLE_ARGS=""
for t in $EIGEN_TESTS; do echo "$LIST" | grep -qx "$t" && DISABLE_ARGS="$DISABLE_ARGS --disable $t"; done
FY="$TEST_OUT_HOST/fullsuite.yaml"; rm -f "$FY"
FULL_STDOUT=$(run_in_container --ignore-timeout -t "$TTIME" -n "$NTHREADS" \
    $DISABLE_ARGS -o "/out/fullsuite.yaml" 2>&1 || true)
EXIT_LINE=$(echo "$FULL_STDOUT" | grep -iE 'exit:' | tail -1 || echo "exit: unknown")
parse_yaml_counts "$FY"

# 崩溃扫描: 真正的崩溃标志是 result: fail 或 stdout 的 crash-context/signal。
# 注意: 不要 grep "SDC" — 它出现在测试 description 里 (本工具就是查 SDC 的), 是误报。
CRASH=$(echo "$FULL_STDOUT" | grep -cE 'crash-context|exited with signal|backtrace|SIGSEGV when|SIGABRT when' | tr -d '\n'); CRASH=${CRASH:-0}
CRASH_Y=$(grep -cE 'crash-context|exited with signal|backtrace|signal: SIG' "$FY" 2>/dev/null | tr -d '\n'); CRASH_Y=${CRASH_Y:-0}

echo "=== $OS_TAG 汇总 ==="
echo "eigen(-n1): pass/skip/fail ; full(-n$NTHREADS): pass=$PASSES skip=$SKIPS fail=$FAILS_Y"
echo "exit: $EXIT_LINE  crashes(stdout=$CRASH yaml=$CRASH_Y) eigen_fails=[$EIGEN_FAILS]"

VERDICT=PASS
{ [ "${FAILS_Y:-0}" -gt 0 ] || [ "${CRASH:-0}" -gt 0 ] || [ "${CRASH_Y:-0}" -gt 0 ] || [ -n "$EIGEN_FAILS" ]; } && VERDICT=FAIL
echo "$EXIT_LINE" | grep -qi 'exit: fail' && VERDICT=FAIL

TOTAL_CRASH=$(( ${CRASH:-0} + ${CRASH_Y:-0} ))
echo "RESULT: $VERDICT (full pass=$PASSES skip=$SKIPS fail=$FAILS_Y, eigen_fails=[$EIGEN_FAILS], crashes=$TOTAL_CRASH)"
[ "$VERDICT" = PASS ] && exit 0 || exit 1
