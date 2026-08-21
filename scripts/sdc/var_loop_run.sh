#!/bin/bash
# var_loop_run.sh — 核179满载真跑:窗口式循环(满载→var_loop循环→停满载→间隔)
#
# 参照 sdc_fuzz_run.sh 的满载负载生成(47核 eigen_sparse,排除179),改成:
#   起满载 → 跑50秒窗口的var_loop循环(单轮0.7-0.8s,窗口内约60-65轮)→
#   停满载 → 间隔10秒喘息 → 下一个窗口
#
# 窗口时长50秒(低于根因报告§16实证的75秒挂死阈值,留安全边际)。
# 窗口数可配置(默认3)。每轮var_loop照旧:跑完立即fsync+与golden比对。
#
# 安全约束(根因报告§16):
#   - 单窗口满载≤50s,timeout硬上限(超过55s强制停满载)
#   - 窗口间隔10s喘息
#   - trap EXIT/INT/TERM:任何退出都先停满载,不留满载残留
#   - 告警文件告警时(监控脚本那边写的)不自动停本脚本——监控在另一终端,
#     由它尝试kill本脚本PID(挂死下不保证);本脚本靠窗口timeout自保
#
# 用法: ./var_loop_run.sh [窗口数=3] [输出目录]
#
# 退出码语义(核179真跑后快速判断结果,外层唯一权威):
#   0   = 全部MATCH,无任何异常(正常)
#   2   = 检测到MISMATCH(SDC候选!)
#   3   = factorize失败(矩阵不正定,solver状态可能被污染)
#   4   = TIMEOUT(被timeout强停,挂死前兆)无MISMATCH
#   5   = MISMATCH + TIMEOUT/factorize失败 都有
#   130 = 中断(Ctrl-C)
# 两层对照:var_loop内部 0/2/3/4,外层timeout 124,外层最终 0/2/3/4/5/130
#
# SPDX-License-Identifier: Apache-2.0
set -uo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
# PORTING NOTE: scripts live at $REPO/scripts/sdc/ (one level deeper than the
# reference tree's $REPO/scripts/), so REPO needs an extra "..".
REPO="$(cd "$HERE/../.." && pwd)"
# PORTING NOTE: docs/sdc1-01-02-core179-diagnostics/ is NOT ported in this unit.
# It ships: var_data/var_cov_N512_sector42_corr43_sorted.csc (SPD precision
# matrix, produced by gen_var_cov_matrix.py), var_data/var_loop (compiled
# scripts/sdc/var_loop.cpp), and golden factors. Until that directory is
# ported, this script will fail at the existence checks below; override DIAG /
# CSC / VAR_LOOP / GOLDEN_* via env vars to point at a prepared location.
DIAG="${DIAG:-$REPO/docs/sdc1-01-02-core179-diagnostics}"
CSC="$DIAG/var_data/var_cov_N512_sector42_corr43_sorted.csc"
VAR_LOOP="$DIAG/var_data/var_loop"
GOLDEN_L="$DIAG/var_data/golden/golden_L_factor.txt"
GOLDEN_VAR="$DIAG/var_data/golden/golden_var99.txt"
LOAD_BIN="$REPO/builddir_sdc/opendcdiag"

# ===== 可配置参数(支持环境变量覆盖,便于健康核端到端演练)=====
NUM_WINDOWS="${1:-3}"                  # 窗口数(默认3,可改)
OUT_DIR="${2:-${WINDOW_OUT_DIR:-$DIAG/var_data/real_runs}}"
TARGET_CORE="${TARGET_CORE:-179}"     # 核179(健康核演练用 TARGET_CORE=176 覆盖)
WINDOW_DURATION="${WINDOW_DURATION:-50}"   # 单窗口满载秒数(<75s挂死阈值;演练可覆盖)
WINDOW_HARD_TIMEOUT="${WINDOW_HARD_TIMEOUT:-55}"  # 窗口硬上限
WINDOW_GAP="${WINDOW_GAP:-10}"        # 窗口间隔秒数(喘息)
RNG_SEED=2024                          # var_loop RNG seed(固定,确定性)
LD_SYSROOT="${HOME}/rpmroot/sysroot/usr/lib64"
LOAD_CORES_LIST="$(seq 144 178 | tr '\n' ',' | sed 's/,$//'),$(seq 180 191 | tr '\n' ',' | sed 's/,$//')"
LOAD_SEED="LCG:323306158"             # 根因报告§13.2 固定失败seed

# 把本脚本PID写到文件,供监控脚本(另一终端)挂死时尝试kill
echo $$ > /tmp/var_loop_run.pid

mkdir -p "$OUT_DIR"

# 防数据污染:每次真跑前自动归档旧result_log.txt(不直接删,加时间戳归档)
if [ -f "$OUT_DIR/result_log.txt" ]; then
    ARCHIVE_NAME="result_log_$(date '+%Y%m%d_%H%M%S').txt"
    mv "$OUT_DIR/result_log.txt" "$OUT_DIR/$ARCHIVE_NAME"
    echo "[归档] 旧result_log.txt已归档为 $ARCHIVE_NAME(防新旧数据混入)"
fi

# 预估(窗口数=3时)
echo "=== 核179满载真跑配置 ==="
echo "  窗口数: $NUM_WINDOWS"
echo "  单窗口满载: ${WINDOW_DURATION}s(硬上限${WINDOW_HARD_TIMEOUT}s,低于75s挂死阈值)"
echo "  窗口间隔: ${WINDOW_GAP}s"
echo "  目标核: $TARGET_CORE (PkgID 19062, socket 4)"
echo "  满载负载: 47核 eigen_sparse (核144-191排除179, seed $LOAD_SEED)"
echo "  var_loop单轮: ~0.7-0.8s, 单窗口预计${WINDOW_DURATION}轮"
echo "  输出目录: $OUT_DIR"
echo "  golden: VaR99=$(cat "$GOLDEN_VAR" 2>/dev/null)"
echo ""
echo "  预估总耗时(窗口数=$NUM_WINDOWS):"
TOTAL_LOAD=$((NUM_WINDOWS * WINDOW_DURATION))
TOTAL_GAP=$(( (NUM_WINDOWS - 1) * WINDOW_GAP))
TOTAL=$((TOTAL_LOAD + TOTAL_GAP))
echo "    总满载暴露时间: ${TOTAL_LOAD}s"
echo "    窗口间隔总计: ${TOTAL_GAP}s"
echo "    总耗时约: ${TOTAL}s ≈ $((TOTAL / 60))分$((TOTAL % 60))秒"
echo "    预计跑var_loop总轮数: ~$((NUM_WINDOWS * WINDOW_DURATION))轮(单轮0.7-0.8s)"
echo ""

# 检查满载二进制
if [ ! -x "$LOAD_BIN" ]; then
    echo "ERROR: 满载二进制 $LOAD_BIN 不存在,先 ninja -C builddir_sdc opendcdiag" >&2
    exit 2
fi

# ===== 满载负载启停(参照 sdc_fuzz_run.sh)=====
LOAD_PID=0
start_load() {
    # 满载时长 = 窗口时长+缓冲(确保覆盖整个窗口)
    local loaddur=$((WINDOW_DURATION + 10))
    echo "[window] 起满载: 47核 eigen_sparse (排除$TARGET_CORE, seed $LOAD_SEED, ${loaddur}s)..."
    LD_LIBRARY_PATH="${LD_SYSROOT}:${LD_LIBRARY_PATH:-}" \
    timeout "$((loaddur + 10))" "$LOAD_BIN" --beta -e eigen_sparse \
        --cpuset "$LOAD_CORES_LIST" -s "$LOAD_SEED" -T "${loaddur}s" \
        --output-format=tap -o /dev/null >/dev/null 2>&1 &
    LOAD_PID=$!
    sleep 4   # 给满载起压时间
}

stop_load() {
    # 先正常 kill 父进程(timeout wrapper)
    if [ "$LOAD_PID" != "0" ]; then
        kill "$LOAD_PID" 2>/dev/null || true
        wait "$LOAD_PID" 2>/dev/null || true
    fi
    # 强杀兜底:opendcdiag 是 timeout 的子进程,kill 父不保证杀子;
    # 且 opendcdiag 可能忽略 SIGTERM。用 -9 强杀 + 两次扫(第一次可能没杀干净)
    pkill -9 -f "eigen_sparse --cpuset" 2>/dev/null || true
    sleep 1
    pkill -9 -f "eigen_sparse --cpuset" 2>/dev/null || true
    # 也清 timeout wrapper 残留
    pkill -9 -f "timeout.*eigen_sparse" 2>/dev/null || true
    wait 2>/dev/null || true
}

# 任何退出都先停满载(防残留导致持续满载)
# trap 函数:INT/TERM 时清满载后强制退出(EXIT trap只清不退,因EXIT是正常退出路径)
cleanup_on_signal() {
    stop_load
    exit 130   # 128+2(SIGINT),中断退出码
}
trap stop_load EXIT
trap cleanup_on_signal INT TERM

# ===== 窗口循环 =====
start_epoch=$(date +%s)
total_rounds_done=0
total_mismatches=0
total_timeouts=0
total_fails=0
window_summary=""   # 每窗口的轮数/不匹配数汇总

for w in $(seq 1 "$NUM_WINDOWS"); do
    echo ""
    echo "===== 窗口 $w/$NUM_WINDOWS 开始: $(date '+%H:%M:%S') ====="

    # 起满载
    start_load

    # 窗口内跑var_loop循环:用窗口硬timeout包裹整段
    win_start=$(date +%s)
    win_rounds=0
    win_mismatch=0
    win_timeout=0
    win_fail=0
    round_in_window=0

    # v2:var_loop同进程循环factorize,一次调用跑完n_rounds轮。
    # 每个窗口起一次var_loop进程,n_rounds=窗口内预计轮数(按单轮0.65s算,50s约76轮,取75留余量)
    # 窗口到点用timeout包裹整个var_loop进程强停(若var_loop跑完75轮就正常退出)
    N_ROUNDS_PER_WINDOW="${N_ROUNDS_PER_WINDOW:-75}"
    rid_start=$(( (w - 1) * 1000 + 1 ))   # 轮次编号:窗口w从(w-1)*1000+1开始

    echo "[window] 起var_loop(v2同进程循环factorize, n_rounds=$N_ROUNDS_PER_WINDOW, rid_start=$rid_start)..."
    timeout "$WINDOW_HARD_TIMEOUT" taskset -c "$TARGET_CORE" "$VAR_LOOP" \
        "$CSC" "$N_ROUNDS_PER_WINDOW" "$OUT_DIR" "$GOLDEN_L" "$GOLDEN_VAR" "$RNG_SEED" "$rid_start" \
        > "/tmp/var_loop_w${w}.out" 2>&1
    rc=$?

    # var_loop退出码:0=全MATCH, 2=有MISMATCH, 3=factorize失败, 4=analyzePattern失败
    # 外层timeout:124=被强停
    # 外层最终退出码统一:0=正常, 2=MISMATCH, 3=factorize失败, 4=TIMEOUT, 5=两者都有
    if [ "$rc" -eq 124 ]; then
        win_timeout=$((win_timeout + 1))
        echo "  [窗口 $w] var_loop被timeout强停(${WINDOW_HARD_TIMEOUT}s) — 可能触发或挂死前兆"
    elif [ "$rc" -eq 0 ]; then
        echo "  [窗口 $w] var_loop正常完成,$N_ROUNDS_PER_WINDOW轮全MATCH"
    elif [ "$rc" -eq 2 ]; then
        echo "  [窗口 $w] ★ var_loop检出MISMATCH! 见 $OUT_DIR/run_*.txt 和 result_log.txt"
    elif [ "$rc" -eq 3 ]; then
        win_fail=$((win_fail + 1))
        echo "  [窗口 $w] var_loop报factorize失败(不正定,solver状态可能被污染)"
    elif [ "$rc" -eq 4 ]; then
        win_fail=$((win_fail + 1))
        echo "  [窗口 $w] var_loop报analyzePattern失败(不正定)"
    else
        echo "  [窗口 $w] var_loop异常退出 rc=$rc"
    fi

    # 统计这个窗口实际跑了多少轮(从result_log.txt数,按rid_start范围)
    # rid_start是数字(如1),result_log里rid格式是"round 0001:",awk match捕获后转数字比较
    win_rounds=$(awk -v s="$rid_start" -v n="$N_ROUNDS_PER_WINDOW" \
        'BEGIN{e=s+n} {if(match($0,/round ([0-9]+):/,a)){r=a[1]+0; if(r>=s && r<e)c++}} END{print c+0}' \
        "$OUT_DIR/result_log.txt" 2>/dev/null)
    win_mismatch=$(awk -v s="$rid_start" -v n="$N_ROUNDS_PER_WINDOW" \
        'BEGIN{e=s+n} {if(match($0,/round ([0-9]+):/,a)){r=a[1]+0; if(r>=s && r<e && /MISMATCH/)c++}} END{print c+0}' \
        "$OUT_DIR/result_log.txt" 2>/dev/null)
    # win_timeout只从rc判断(rc==124=被timeout强停);win_fail从rc==3/4判断
    # win_timeout 和 win_fail 已在上面 rc 判断时设置

    # 停满载(窗口到点)
    stop_load

    total_rounds_done=$((total_rounds_done + win_rounds))
    total_mismatches=$((total_mismatches + win_mismatch))
    total_timeouts=$((total_timeouts + win_timeout))
    total_fails=$((total_fails + win_fail))
    window_summary+="窗口$w: 跑$win_rounds轮, MISMATCH=$win_mismatch, TIMEOUT=$win_timeout, FAIL=$win_fail\n"
    echo "===== 窗口 $w 结束: 跑$win_rounds轮, MISMATCH=$win_mismatch, TIMEOUT=$win_timeout, FAIL=$win_fail ====="

    # 窗口间隔(最后一个窗口不等)
    if [ "$w" -lt "$NUM_WINDOWS" ]; then
        echo "[gap] 窗口间隔 ${WINDOW_GAP}s 喘息..."
        sleep "$WINDOW_GAP"
    fi
done

end_epoch=$(date +%s)

# ===== 汇总 =====
echo ""
echo "========================================"
echo "=== 核179满载真跑汇总 ==="
echo "========================================"
echo "  总耗时: $((end_epoch - start_epoch))s"
echo "  总满载暴露时间: ${TOTAL_LOAD}s (${NUM_WINDOWS}窗口×${WINDOW_DURATION}s)"
echo "  总完成轮数: $total_rounds_done"
echo "  MISMATCH(SDC检出): $total_mismatches"
echo "  TIMEOUT(挂死前兆): $total_timeouts"
echo ""
echo "  各窗口明细:"
while IFS= read -r line; do
    echo "    $line"
done <<< "$(printf "$window_summary")"

# MISMATCH详情(若有)
if [ "$total_mismatches" -gt 0 ]; then
    echo ""
    echo "=== MISMATCH轮次详情(SDC检出!)==="
    grep MISMATCH "$OUT_DIR/result_log.txt"
fi

# TIMEOUT详情(若有)
if [ "$total_timeouts" -gt 0 ]; then
    echo ""
    echo "=== TIMEOUT轮次(可能挂死前兆)==="
    echo "  见 /tmp/var_loop_r*.out 里rc=124的轮次"
fi

echo ""
echo "⚠️  请手动检查监控告警文件: cat /tmp/var_loop_alert.txt"
echo "    (由另一终端的监控脚本维护,本脚本不读不碰)"
echo ""
echo "=== 完成: $(date '+%Y-%m-%d %H:%M:%S') ==="

# ===== 显式退出码(外层唯一权威,语义清晰,不被EXIT trap覆盖)=====
# 0   = 全部MATCH,无任何异常(正常)
# 2   = 检测到MISMATCH(SDC候选!)  ← 跳过1避免和trap污染的1混淆
# 3   = 有factorize失败(矩阵不正定,solver状态可能被污染)
# 4   = 有TIMEOUT(被timeout强停,挂死前兆)无MISMATCH
# 5   = MISMATCH + TIMEOUT/factorize失败 都有
# 130 = 中断(Ctrl-C/SIGINT)
# 其他非0 = 脚本本身执行异常(二进制缺失/参数错等)
#
# 两层退出码对照:
#   var_loop内部: 0=全MATCH, 2=有MISMATCH, 3=factorize失败, 4=analyzePattern失败
#   外层timeout:  124=被强停
#   外层最终(本脚本): 0/2/3/4/5/130(上面6种,唯一权威)
#   核179真跑后看这一个数字即可判断结果,不用翻日志。
if [ "$total_mismatches" -gt 0 ] && { [ "$total_timeouts" -gt 0 ] || [ "$total_fails" -gt 0 ]; }; then
    exit 5   # MISMATCH + TIMEOUT/fail 都有
elif [ "$total_mismatches" -gt 0 ]; then
    exit 2   # 只有MISMATCH
elif [ "$total_fails" -gt 0 ]; then
    exit 3   # factorize失败
elif [ "$total_timeouts" -gt 0 ]; then
    exit 4   # TIMEOUT
else
    exit 0
fi
