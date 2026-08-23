#!/bin/bash
# verify-built-pristine.sh — 在纯净匹配容器里验证 RPM 仓 built/ 目录的二进制能独立运行。
#
# 这是"直接下载后能运行"的决定性验证: 只挂载 built/ 目录(模拟用户下载了该目录),
# 不挂载 host /usr/lib64, 不挂载 build-out/。用随包的 run-opendcdiag.sh 跑全量用例。
#
# 用法: ./verify-built-pristine.sh <series> <sp> [smoke]
#   series: 24.03 | 22.03 | 20.03
#   sp    : LTS | SP1 | SP2 | SP3 | SP4
#   smoke : 可选。传 smoke 只跑 --list-tests + zstd19(快, 验证加载);
#          否则跑全量(所有 PROD 用例, eigen 类 -n1)。
#
# stdout 末行: "RESULT: PASS|FAIL <counts>" 供 subagent 解析。
# SPDX-License-Identifier: Apache-2.0
set -uo pipefail

SERIES="${1:?usage: $0 <series> <sp> [smoke]}"
SP="${2:?usage: $0 <series> <sp> [smoke]}"
MODE="${3:-full}"

case "$SP" in
    LTS)   SP_DIR="LTS";    IMG_SP="LTS" ;;
    SP[1-4]) SP_DIR="LTS_$SP"; IMG_SP="LTS-$SP" ;;
    *) echo "bad SP: $SP" >&2; exit 1 ;;
esac

OS_TAG="openEuler-${SERIES}${SP_DIR}"
IMG="localhost/openeuler-offline:${SERIES}-${IMG_SP}"
BUILT="/pkg"   # 容器内挂载点

# host 侧 built/ 目录绝对路径
SRC_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILT_HOST="$SRC_ROOT/third-party/rpms/openEuler-${SERIES}/${OS_TAG}/built"
[ -d "$BUILT_HOST" ] || { echo "RESULT: FAIL no-built-dir($BUILT_HOST)"; exit 2; }

# 结果写到 host 的 build-out/<tag>/pristine-results/
OUT_HOST="$SRC_ROOT/build-out/${OS_TAG}/pristine-results"
mkdir -p "$OUT_HOST"

run_in_pristine() {
    # 仅挂载 built/ 目录: 模拟下载后直接运行。无 host lib64, 无 build-out。
    # cwd 设为 /out(可写挂载), 让二进制默认日志写到那里(避免只读 /pkg 报错)。
    timeout "${TIMEOUT:-540}" podman run --rm --user=0 \
        -v "$BUILT_HOST:$BUILT:ro" \
        -v "$OUT_HOST:/out" \
        -w /out \
        "$IMG" bash -c "
            mkdir -p /var/tmp /tmp 2>/dev/null
            export LD_LIBRARY_PATH=$BUILT/libs\${LD_LIBRARY_PATH:+:\$LD_LIBRARY_PATH}
            $BUILT/opendcdiag \"\$@\"
        " _ "$@" 2>&1
}

echo "=== $OS_TAG pristine (built/ only, no host libs) ===" >&2

# 1) 加载测试: --list-tests
LIST=$(run_in_pristine --list-tests 2>/dev/null | grep -v '^$' || true)
TOTAL=$(echo "$LIST" | grep -c . || echo 0)
echo "total tests: $TOTAL" >&2
if [ "${TOTAL:-0}" -lt 100 ]; then
    echo "RESULT: FAIL too-few-tests($TOTAL)" ; exit 1; fi

if [ "$MODE" = "smoke" ]; then
    # 只跑 zstd19 -n1 快速验证运行
    out=$(run_in_pristine -e zstd19 -t 1000 -n 1 2>&1 | grep -iE 'exit:' | tail -1 || echo "exit: unknown")
    echo "zstd19: $out" >&2
    echo "$out" | grep -qi 'exit: pass' && { echo "RESULT: PASS (smoke, $TOTAL tests, $out)"; exit 0; }
    echo "RESULT: FAIL (smoke, $out)"; exit 1
fi

# 2) 全量: eigen 数值类 -n1 (192核 ULP flakiness), 其余 -n8
EIGEN="eigen_svd_double eigen_sparse eigen_svd_cdouble eigen_svd_cdouble_sve"
EIGEN_FAILS=""
for t in $EIGEN; do
    echo "$LIST" | grep -qx "$t" || continue
    ey="$OUT_HOST/${t}.yaml"; rm -f "$ey"
    run_in_pristine -e "$t" -t 1000 -n 1 --ignore-timeout -o "/out/${t}.yaml" >/dev/null 2>&1 || true
    f=$(grep -c 'result: fail' "$ey" 2>/dev/null | tr -d '\n'); f=${f:-0}
    [ "${f:-0}" -gt 0 ] && EIGEN_FAILS="$EIGEN_FAILS $t"
    echo "  $t: fail=$f" >&2
done

# 其余用例
DISABLE=""
for t in $EIGEN; do echo "$LIST" | grep -qx "$t" && DISABLE="$DISABLE --disable $t"; done
FY="$OUT_HOST/fullsuite.yaml"; rm -f "$FY"
FULL=$(run_in_pristine --ignore-timeout -t 1000 -n 8 $DISABLE -o "/out/fullsuite.yaml" 2>&1 || true)
EXIT_LINE=$(echo "$FULL" | grep -iE 'exit:' | tail -1 || echo "exit: unknown")
PASSES=$(grep -c 'result: pass' "$FY" 2>/dev/null | tr -d '\n'); PASSES=${PASSES:-0}
SKIPS=$(grep -c 'result: skip' "$FY" 2>/dev/null | tr -d '\n'); SKIPS=${SKIPS:-0}
FAILS=$(grep -c 'result: fail' "$FY" 2>/dev/null | tr -d '\n'); FAILS=${FAILS:-0}
# 崩溃扫描: 只看真正的崩溃标志, 不 grep 'SDC'(出现在测试 description 里, 误报)
CRASH=$(echo "$FULL" | grep -cE 'crash-context|exited with signal|backtrace|SIGSEGV when|SIGABRT when' | tr -d '\n'); CRASH=${CRASH:-0}

echo "=== $OS_TAG 汇总 ===" >&2
echo "eigen(-n1)fails=[$EIGEN_FAILS] full(-n8): pass=$PASSES skip=$SKIPS fail=$FAILS" >&2
echo "$EXIT_LINE  crashes=$CRASH" >&2

VERDICT=PASS
{ [ "${FAILS:-0}" -gt 0 ] || [ "${CRASH:-0}" -gt 0 ] || [ -n "$EIGEN_FAILS" ]; } && VERDICT=FAIL
echo "$EXIT_LINE" | grep -qi 'exit: fail' && VERDICT=FAIL
echo "RESULT: $VERDICT (full pass=$PASSES skip=$SKIPS fail=$FAILS, eigen_fails=[$EIGEN_FAILS], crashes=$CRASH)"
[ "$VERDICT" = PASS ] && exit 0 || exit 1
