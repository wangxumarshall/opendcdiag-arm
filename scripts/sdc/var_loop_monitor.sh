#!/bin/bash
# var_loop_monitor.sh — 并行监控:双路径冗余抓挂死前兆(零fork关键路径版)
#
# 价值:告警文件是"崩溃前最后发生了什么"的关键证据。匹配即写+sync。
#
# ─── 零fork关键路径设计(2026-08-14 v2)───────────────────────────────
# v1的path_dmesg逐行处理连续fork三个进程(grep提时间戳/awk浮点比较/
# grep匹配关键字)。这与"少依赖用户态进程"的设计初衷矛盾:RCU stall下
# fork新进程需内核调度正常,若系统连启动awk/grep都做不到,dmesg路径同样
# 会卡住。v2改成全bash内置:
#   - 时间戳提取:[[ =~ ]] + BASH_REMATCH(bash内建,不fork)
#   - 时间戳比较:字符串切片${var%%.*}/${var#*.} + (( 10# ))算术比较(内建)
#     (10#前缀避免前导0被当八进制,如008123会报"value too great for base")
#   - 关键字匹配:[[ =~ $KEYWORDS ]](已验证与grep -E等价,见脚本末验证)
# 只在"匹配到告警需写文件+sync"时才fork(date/sync),这非高频路径,风险可接受。
#
# ─── 双路径冗余(2026-08-14 v1设计,保留)─────────────────────────────
# 路径1 [dmesg]:直接读内核缓冲区,不经用户态中转。解决历史刷屏:启动时记录
#   最后时间戳作起点,follow只处理>起点的行(dmesg --follow从buffer头刷历史)。
# 路径2 [journalctl]:journald中转,平时不误报,作参考路。
# 两路独立后台跑,互不阻塞。告警标注来源,只一路抓到时可知哪路关键时刻失效。
#
# ─── 关键字(精确,根因报告§16实证 + 标准崩溃格式;不含宽泛SDEI/watchdog)──
# 已用bash =~ 验证:5条真实告警全匹配,4条正常开机/运行行全不误报(见脚本末)。
#
# 局限(诚实):RCU stall+IPI失效下SIGTERM大概率发不到;留证据是核心。
#
# 用法: sudo bash scripts/sdc/var_loop_monitor.sh [alert_file] [loop_pid_file]
# SPDX-License-Identifier: Apache-2.0
set -uo pipefail

ALERT_FILE="${1:-/tmp/var_loop_alert.txt}"
LOOP_PID_FILE="${2:-/tmp/var_loop_run.pid}"

# 精确关键字(bash =~ 已验证等价于 grep -E;5真告警匹配,4正常行不误报)
KEYWORDS='rcu_sched detected stalls|rcu: INFO:.*detected stall|Sending NMI from CPU|Sending IPI failed|rcu_sched kthread starved|rcu_sched.*timer wakeup didn|Call Trace:|Kernel panic|kernel:.*hung task|soft lockup.*CPU|hard LOCKUP'

# 写告警(标注来源)+ sync —— 唯一fork点(date/sync),非每行高频路径
write_alert() {
    local src="$1" line="$2"
    local ts=$(date '+%Y-%m-%d %H:%M:%S')
    {
        echo "!!! [$ts] [$src] 告警:匹配到挂死前兆关键字 !!!"
        echo "    原始日志: $line"
        echo "    ---"
    } >> "$ALERT_FILE"
    sync
    echo "[monitor $ts] [$src] ★ 告警写入: $line" >&2
}

# 尝试终止主循环(挂死下可能发不到,尽力而为)
try_kill_loop() {
    if [ -f "$LOOP_PID_FILE" ]; then
        local pid=$(cat "$LOOP_PID_FILE" 2>/dev/null)
        if [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null; then
            echo "[monitor] 尝试kill主循环PID=$pid(挂死下可能失败)" >> "$ALERT_FILE"
            sync
            kill -TERM "$pid" 2>/dev/null || true
            sleep 1
            kill -KILL "$pid" 2>/dev/null || true
        fi
    fi
}

# ─── 诊断块(启动时,打印身份+两路数据源可用性)─────────────────────
{
    echo "========================================================"
    echo "[诊断 $(date '+%H:%M:%S')] 脚本启动身份检查"
    echo "  id=$(id 2>&1)"
    echo "  whoami=$(whoami 2>&1)"
    echo "  EUID=$EUID  UID=$UID"
    echo "  --- dmesg当前总行数(路径1起点)---"
    sudo dmesg 2>/dev/null | wc -l
    echo "  --- dmesg当前最后时间戳(路径1起点)---"
    sudo dmesg 2>/dev/null | grep -oE '^\[ *[0-9]+\.[0-9]+\]' | tail -1
    echo "  --- journalctl -k 测试(路径2,前2行)---"
    journalctl -k --no-pager 2>&1 | tail -2
    echo "  --- journalctl退出码: $? ---"
    echo "  --- dmesg_restrict / selinux ---"
    cat /proc/sys/kernel/dmesg_restrict 2>&1
    getenforce 2>&1 || echo "(无SELinux)"
    echo "========================================================"
} >> "$ALERT_FILE"
sync
echo "[monitor $(date '+%H:%M:%S')] 双路径监控启动(零fork关键路径版): [dmesg]+[journalctl],告警文件: $ALERT_FILE" | tee -a "$ALERT_FILE"

# ══════════════════════════════════════════════════════════════════════
# 路径1:dmesg —— 直接读内核缓冲区,零fork逐行处理
# 时间戳提取用 [[ =~ ]]+BASH_REMATCH;比较用字符串切片+(( 10# ));匹配用 [[ =~ ]]
# ══════════════════════════════════════════════════════════════════════
path_dmesg() {
    # 取当前最后时间戳作起点(形如 81742.676938)—— 这一步fork,但只在启动时一次
    local start_line=$(sudo dmesg 2>/dev/null | grep -oE '^\[ *[0-9]+\.[0-9]+\]' | tail -1)
    if [ -z "$start_line" ]; then
        echo "[monitor $(date '+%H:%M:%S')] [dmesg] 路径1:取不到起点时间戳,该路停" >> "$ALERT_FILE"
        sync
        return
    fi
    # 解析起点时间戳的整数/小数部分(bash内置切片)
    local start_raw="${start_line//[\[\] ]/}"   # 去[]和空格
    local start_int="${start_raw%%.*}"
    local start_frac="${start_raw#*.}"
    echo "[monitor $(date '+%H:%M:%S')] [dmesg] 路径1启动,起点=$start_raw (int=$start_int frac=$start_frac)" >> "$ALERT_FILE"
    sync

    local prev_passed=0
    # follow持续读;逐行处理全用bash内置,不fork
    # 注:sudo dmesg --follow 本身是一个进程(数据源),不在逐行处理里fork
    while IFS= read -r line; do
        local do_match=0
        # 提取本行时间戳:bash =~ + BASH_REMATCH(内建,不fork)
        if [[ "$line" =~ ^\[\ *([0-9]+)\.([0-9]+)\] ]]; then
            local cur_int="${BASH_REMATCH[1]}"
            local cur_frac="${BASH_REMATCH[2]}"
            # 比较:bash算术(( )),10#避免前导0八进制坑
            # 先比整数部分,整数相等再比小数(小数定长6位,字符串补齐后可直接整数比,但10#更稳)
            if (( 10#$cur_int > 10#$start_int )); then
                do_match=1; prev_passed=1
            elif (( 10#$cur_int == 10#$start_int )) && (( 10#$cur_frac > 10#$start_frac )); then
                do_match=1; prev_passed=1
            else
                prev_passed=0
            fi
        else
            # 无时间戳续行(如Call Trace后续行):前一行通过则跟随
            if [ "$prev_passed" = "1" ]; then do_match=1; fi
        fi
        # 关键字匹配:bash =~ (内建,不fork)
        if [ "$do_match" = "1" ] && [[ "$line" =~ $KEYWORDS ]]; then
            write_alert "dmesg" "$line"   # 唯一fork点(date/sync)
            try_kill_loop
        fi
    done < <(sudo dmesg --follow 2>/dev/null)
}

# ══════════════════════════════════════════════════════════════════════
# 路径2:journalctl -k --follow —— journald中转,参考路(也改零fork)
# ══════════════════════════════════════════════════════════════════════
path_journalctl() {
    echo "[monitor $(date '+%H:%M:%S')] [journalctl] 路径2启动" >> "$ALERT_FILE"
    sync
    # journalctl --follow 是单进程流式输出;逐行用bash =~匹配(不fork grep)
    while IFS= read -r line; do
        if [[ "$line" =~ $KEYWORDS ]]; then
            write_alert "journalctl" "$line"
            try_kill_loop
        fi
    done < <(journalctl -k --follow 2>/dev/null)
}

# 两路独立后台跑,互不阻塞
path_dmesg &
path_journalctl &

wait

# ─── 验证记录(功能等价性 + 八进制坑)─────────────────────────────────
# 1. KEYWORDS bash =~ 等价性:5条真实告警(rcu_sched detected stalls /
#    Sending NMI / Sending IPI failed / rcu_sched kthread starved / Call Trace)
#    全匹配=1;4条正常开机行(ACPI:SDEI / SDEI NMI watchdog registered /
#    sdei: SDEIv1.0 / capability: warning)全不匹配=0。已验证。
# 2. 八进制坑:时间戳小数 008123 不加10#会报 "value too great for base",
#    10#$frac 强制十进制解决。已验证。
