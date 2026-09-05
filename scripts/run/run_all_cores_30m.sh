#!/bin/bash
# ============================================================
# 在 0-31 号核心上并行执行 OpenDCDiag 全部测试用例
# 总时间限制：30 分钟（--strict-runtime 会强制停止）
# 每个核心的日志保存在 ./core_all_tests_30m/core_<核心编号>.log
# ============================================================

# 1. 配置参数
OPENDCDIAG="./builddir/sdcshield"          # 可执行文件路径（请根据实际位置调整）
LOG_DIR="./core_all_tests_30m"              # 日志根目录
TOTAL_TIME="30m"                            # 总运行时间
CORES=(0 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20 21 22 23 24 25 26 27 28 29 30 31)

# 2. 创建日志目录（如果不存在）
mkdir -p "$LOG_DIR"

echo "========== 开始并行测试 =========="
echo "日志目录: $LOG_DIR"
echo "参与核心: ${CORES[@]}"
echo "总时间限制: $TOTAL_TIME"
echo "启动时间: $(date)"
echo ""

# 3. 启动每个核心的测试（后台并行）
for core in "${CORES[@]}"; do
    LOG_FILE="$LOG_DIR/core_${core}.log"
    echo "启动核心 $core，日志文件: $LOG_FILE"
    # 将标准输出和标准错误都重定向到日志文件，终端不会显示任何内容
    $OPENDCDIAG -T $TOTAL_TIME --strict-runtime --cpuset=$core -o "$LOG_FILE" > "$LOG_FILE" 2>&1 &
done

# 4. 等待所有后台进程完成
wait

echo ""
echo "========== 所有核心测试完成 =========="
echo "结束时间: $(date)"

# 5. 可选：快速汇总每个核心的最终测试结果
echo ""
echo "========== 各核心测试结果汇总 =========="
for core in "${CORES[@]}"; do
    LOG_FILE="$LOG_DIR/core_${core}.log"
    if [ -f "$LOG_FILE" ]; then
        # 提取最后一个 result: 行
        LAST_RESULT=$(grep "result:" "$LOG_FILE" | tail -1)
        if [ -n "$LAST_RESULT" ]; then
            echo "核心 $core: $LAST_RESULT"
        else
            echo "核心 $core: 未找到 result 行（可能测试未完成）"
        fi
    else
        echo "核心 $core: 日志文件不存在"
    fi
done

echo ""
echo "详细日志请查看: $LOG_DIR/core_*.log"
