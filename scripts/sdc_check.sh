#!/bin/bash
# 脚本：sdc_check.sh
# 功能：根据 HWSentinel 条件判断单台服务器是否疑似 SDC
#       规则：SEL 有错误 → 最终输出 PASS（但仍统计重启和内核异常）
#       硬件错误仅来自 SEL，软件错误来自内核/系统日志
# 适配平台：鲲鹏920 (ARM64)

# ---------- 颜色定义 ----------
RED='\033[0;31m'
GREEN='\033[0;32m'
NC='\033[0m'

# ---------- 初始化统计变量 ----------
UNPLANNED_REBOOT_COUNT=0
PLANNED_REBOOT_COUNT=0
CORE_FAIL_CORE=""
SEL_ERROR_CLASS=""
SEL_HAS_ERROR=0
SYS_ERROR_COUNT=0
SYS_AUTH_ERR=0
SYS_SERVICE_ERR=0
SYS_SELINUX_ERR=0
SYS_FS_ERR=0
SYS_OTHER_ERR=0

# ---------- 1. 检查 SEL 硬件错误（分类，记录但不退出） ----------
echo "[*] 检查 SEL 中各类硬件错误..."
CPU_ERRORS="cpu|processor|ARM Processor error|IERR|MCE|machine check"
MEMORY_ERRORS="memory|MC error|DDR|DDRC|UCE|CE|uncorrectable|correctable|L3|L3TAG|L3DATA|HHA|SMMU"
PCIe_ERRORS="PCIe|AER|aer|PCIe aer error|AP|TL|MAC|DL|SDI"
STORAGE_ERRORS="disk|drive|hard disk|SAS|SATA|RAID|array|SMART"
NETWORK_ERRORS="network|NIC|ethernet|link down|PHY|MDIO"
ENVIRONMENT_ERRORS="temperature|temp|fan|fanspeed|voltage|power|PSU|thermal|threshold|overheat|cooling"
BMC_ERRORS="BMC|IPMI|watchdog|sel"
KUNPENG_ERRORS="MN|PLL|SLLC|AA|SIOE|POE|DISP|LPC|GIC|RDE|PA|HLLC"
OTHER_HW_ERRORS="fatal|hardware error|system error|bus error|abort"

SEL_RAW=$(sudo ipmitool sel list 2>/dev/null)
if [ -z "$SEL_RAW" ]; then
    echo "    警告：无法获取 SEL 日志，将继续检查"
    SEL_ERROR_CLASS="None"
else
    SEL_CPU_CNT=$(echo "$SEL_RAW" | grep -iEc "$CPU_ERRORS")
    SEL_MEM_CNT=$(echo "$SEL_RAW" | grep -iEc "$MEMORY_ERRORS")
    SEL_PCIE_CNT=$(echo "$SEL_RAW" | grep -iEc "$PCIe_ERRORS")
    SEL_STORAGE_CNT=$(echo "$SEL_RAW" | grep -iEc "$STORAGE_ERRORS")
    SEL_NETWORK_CNT=$(echo "$SEL_RAW" | grep -iEc "$NETWORK_ERRORS")
    SEL_ENV_CNT=$(echo "$SEL_RAW" | grep -iEc "$ENVIRONMENT_ERRORS")
    SEL_BMC_CNT=$(echo "$SEL_RAW" | grep -iEc "$BMC_ERRORS")
    SEL_KUNPENG_CNT=$(echo "$SEL_RAW" | grep -iEc "$KUNPENG_ERRORS")
    SEL_OTHER_CNT=$(echo "$SEL_RAW" | grep -iEc "$OTHER_HW_ERRORS")

    if [ $SEL_CPU_CNT -gt 0 ]; then SEL_ERROR_CLASS="${SEL_ERROR_CLASS}CPU=${SEL_CPU_CNT} "; fi
    if [ $SEL_MEM_CNT -gt 0 ]; then SEL_ERROR_CLASS="${SEL_ERROR_CLASS}Memory=${SEL_MEM_CNT} "; fi
    if [ $SEL_PCIE_CNT -gt 0 ]; then SEL_ERROR_CLASS="${SEL_ERROR_CLASS}PCIe=${SEL_PCIE_CNT} "; fi
    if [ $SEL_STORAGE_CNT -gt 0 ]; then SEL_ERROR_CLASS="${SEL_ERROR_CLASS}Storage=${SEL_STORAGE_CNT} "; fi
    if [ $SEL_NETWORK_CNT -gt 0 ]; then SEL_ERROR_CLASS="${SEL_ERROR_CLASS}Network=${SEL_NETWORK_CNT} "; fi
    if [ $SEL_ENV_CNT -gt 0 ]; then SEL_ERROR_CLASS="${SEL_ERROR_CLASS}Env=${SEL_ENV_CNT} "; fi
    if [ $SEL_BMC_CNT -gt 0 ]; then SEL_ERROR_CLASS="${SEL_ERROR_CLASS}BMC=${SEL_BMC_CNT} "; fi
    if [ $SEL_KUNPENG_CNT -gt 0 ]; then SEL_ERROR_CLASS="${SEL_ERROR_CLASS}Kunpeng=${SEL_KUNPENG_CNT} "; fi
    if [ $SEL_OTHER_CNT -gt 0 ]; then SEL_ERROR_CLASS="${SEL_ERROR_CLASS}Other=${SEL_OTHER_CNT} "; fi
    if [ -z "$SEL_ERROR_CLASS" ]; then
        SEL_ERROR_CLASS="None"
        echo "    SEL 未发现任何错误"
    else
        SEL_HAS_ERROR=1
        echo "    SEL 发现错误：$SEL_ERROR_CLASS"
    fi
fi

# ---------- 2. 检查 30 天内重启次数（区分计划内/外） ----------
echo "[*] 统计过去 30 天的重启次数..."
REBOOT_TIMES=$(last reboot -F 2>/dev/null | awk -v cutoff=$(date -d "30 days ago" +%s) '
    /wtmp begins/ {next}
    {
        month=$6; day=$7; time=$8; year=$9
        if (month && day && time && year) {
            cmd = "date -d \""month" "day" "time" "year"\" +%s 2>/dev/null"
            cmd | getline ts
            close(cmd)
            if (ts >= cutoff) print ts
        }
    }
')

if [ -z "$REBOOT_TIMES" ]; then
    PLANNED=0
    UNPLANNED=0
else
    PLANNED=0
    UNPLANNED=0
    keywords="shutting down for system reboot|systemd-reboot|reboot: Restarting system|shutdown|halt|init 6|reboot command|systemctl reboot|shutdown -r"
    for reboot_ts in $REBOOT_TIMES; do
        start_search=$(date -d "@$((reboot_ts - 300))" "+%Y-%m-%d %H:%M:%S")
        end_search=$(date -d "@$((reboot_ts + 30))" "+%Y-%m-%d %H:%M:%S")
        found=0
        if command -v journalctl &>/dev/null; then
            found_journal=$(sudo journalctl --since "$start_search" --until "$end_search" 2>/dev/null | grep -iE "$keywords")
            [ -n "$found_journal" ] && found=1
        fi
        if [ $found -eq 0 ] && [ -f /var/log/messages ]; then
            found_messages=$(sudo grep -iE "$keywords" /var/log/messages 2>/dev/null | awk -v start="$start_search" -v end="$end_search" '
                $1" "$2" "$3 >= start && $1" "$2" "$3 <= end
            ')
            [ -n "$found_messages" ] && found=1
        fi
        if [ $found -eq 0 ] && [ -f /var/log/syslog ]; then
            found_syslog=$(sudo grep -iE "$keywords" /var/log/syslog 2>/dev/null | awk -v start="$start_search" -v end="$end_search" '
                $1" "$2" "$3 >= start && $1" "$2" "$3 <= end
            ')
            [ -n "$found_syslog" ] && found=1
        fi
        if [ $found -eq 1 ]; then
            ((PLANNED++))
        else
            ((UNPLANNED++))
        fi
    done
    echo "    计划内重启: $PLANNED, 计划外重启: $UNPLANNED"
fi

reboot_fail=0
[ "$UNPLANNED" -ge 3 ] && reboot_fail=1

# ---------- 3. 检查内核异常核心集中度 ----------
echo "[*] 分析过去 30 天的内核异常..."
KERNEL_LOGS=$(journalctl -k --since "30 days ago" 2>/dev/null)
[ -z "$KERNEL_LOGS" ] && KERNEL_LOGS=$(dmesg -T 2>/dev/null)
EXCEPTIONS=$(echo "$KERNEL_LOGS" | grep -E "panic|oops|segfault|general protection|double fault|stack segment|invalid opcode|machine check" -A1)

core_fail=0
CORE_FAIL_CORE=""
if [ -n "$EXCEPTIONS" ]; then
    TMP_CORE_APP=$(mktemp)
    echo "$EXCEPTIONS" | while read -r line; do
        core=$(echo "$line" | grep -oE "CPU[ :]*[0-9]+" | grep -oE "[0-9]+" | head -1)
        app=$(echo "$line" | grep -oE "Comm: [^ ]+" | sed 's/Comm: //' | head -1)
        [ -z "$app" ] && app=$(echo "$line" | grep -oE "Task: [^ ]+" | sed 's/Task: //' | head -1)
        [ -n "$core" ] && [ -n "$app" ] && echo "$core $app"
    done > "$TMP_CORE_APP"

    TOTAL=$(wc -l < "$TMP_CORE_APP")
    if [ "$TOTAL" -gt 0 ]; then
        PHYSICAL_MAP_FILE=$(mktemp)
        for cpu_dir in /sys/devices/system/cpu/cpu[0-9]*; do
            if [ -f "$cpu_dir/topology/thread_siblings_list" ]; then
                cpu_num=$(basename "$cpu_dir" | sed 's/cpu//')
                siblings=$(cat "$cpu_dir/topology/thread_siblings_list" 2>/dev/null)
                [ -n "$siblings" ] && echo "$cpu_num $(echo "$siblings" | cut -d',' -f1)" >> "$PHYSICAL_MAP_FILE"
            fi
        done
        if [ ! -s "$PHYSICAL_MAP_FILE" ]; then
            for cpu in $(seq 0 $(($(nproc --all 2>/dev/null || echo 1) - 1))); do
                core_id=$(cat "/sys/devices/system/cpu/cpu${cpu}/topology/core_id" 2>/dev/null)
                [ -z "$core_id" ] && core_id=$cpu
                echo "$cpu $core_id" >> "$PHYSICAL_MAP_FILE"
            done
        fi

        declare -A core_apps core_counts
        while read -r logical_core app; do
            phys_core=$(grep "^$logical_core " "$PHYSICAL_MAP_FILE" | awk '{print $2}')
            [ -z "$phys_core" ] && phys_core=$logical_core
            ((core_counts[$phys_core]++))
            if [ -z "${core_apps[$phys_core]}" ]; then
                core_apps[$phys_core]="$app"
            else
                [[ "${core_apps[$phys_core]}" != *"$app"* ]] && core_apps[$phys_core]="${core_apps[$phys_core]},$app"
            fi
        done < "$TMP_CORE_APP"

        max_count=0; max_core=""
        for core in "${!core_counts[@]}"; do
            if [ "${core_counts[$core]}" -gt "$max_count" ]; then
                max_count=${core_counts[$core]}
                max_core=$core
            fi
        done

        if [ -n "$max_core" ]; then
            max_percent=$(echo "scale=2; $max_count * 100 / $TOTAL" | bc)
            app_count=$(echo "${core_apps[$max_core]}" | tr ',' '\n' | sort -u | wc -l)
            if (( $(echo "$max_percent > 60" | bc -l) )) && [ "$app_count" -ge 2 ]; then
                core_fail=1
                CORE_FAIL_CORE="$max_core"
            fi
        fi
        rm -f "$PHYSICAL_MAP_FILE"
    fi
    rm -f "$TMP_CORE_APP"
fi

# ---------- 4. 统计软件层错误（分类） ----------
echo "[*] 统计过去 30 天的软件层错误..."
SYS_LOGS=$(journalctl --since "30 days ago" --no-pager 2>/dev/null)
if [ -n "$SYS_LOGS" ]; then
    SYS_AUTH_ERR=$(echo "$SYS_LOGS" | grep -iEc "Failed password|Invalid user|Permission denied|sudo:.*command not allowed")
    SYS_SERVICE_ERR=$(echo "$SYS_LOGS" | grep -iEc "Failed to start|Dependency failed|Main process exited|Timeout")
    SYS_SELINUX_ERR=$(echo "$SYS_LOGS" | grep -iEc "SELinux: denied")
    SYS_FS_ERR=$(echo "$SYS_LOGS" | grep -iEc "No space left on device|Read-only file system|superblock corruption|I/O error|EXT4-fs error|XFS:")
    SYS_OTHER_ERR=$(echo "$SYS_LOGS" | grep -iEc "segmentation fault|aborted|core dumped|Exception|Error" | \
                    awk -v a="$SYS_AUTH_ERR" -v b="$SYS_SERVICE_ERR" -v c="$SYS_SELINUX_ERR" -v d="$SYS_FS_ERR" '{print $1 - a - b - c - d}')
    SYS_ERROR_COUNT=$((SYS_AUTH_ERR + SYS_SERVICE_ERR + SYS_SELINUX_ERR + SYS_FS_ERR + SYS_OTHER_ERR))
    echo "    软件错误总数: $SYS_ERROR_COUNT"
    echo "      - 认证/授权: $SYS_AUTH_ERR"
    echo "      - 服务启动: $SYS_SERVICE_ERR"
    echo "      - SELinux: $SYS_SELINUX_ERR"
    echo "      - 文件系统: $SYS_FS_ERR"
    echo "      - 其他: $SYS_OTHER_ERR"
else
    echo "    无法获取系统日志，软件错误统计跳过"
fi

# ---------- 5. 提取最近 20 条具体硬件错误（仅来自 SEL） ----------
echo ""
echo "[*] 最近 20 条具体硬件错误（来自 SEL）:"
if [ -n "$SEL_RAW" ]; then
    SEL_ERRORS=$(echo "$SEL_RAW" | tail -20)
    if [ -n "$SEL_ERRORS" ]; then
        echo "$SEL_ERRORS" | while IFS= read -r line; do
            echo "  $line"
        done
    else
        echo "  SEL 中未发现错误记录"
    fi
else
    echo "  无法获取 SEL 日志"
fi

# ---------- 6. 提取最近 20 条具体软件错误（来自内核日志 + 系统日志） ----------
echo ""
echo "[*] 最近 20 条具体软件错误（来自内核/系统日志）:"
SW_ERRORS=$( (journalctl -k --since "30 days ago" 2>/dev/null ; journalctl --since "30 days ago" --no-pager 2>/dev/null) | \
    grep -iE "panic|oops|segfault|general protection|double fault|stack segment|invalid opcode|machine check|Failed|Error|Exception|aborted|core dumped|segmentation fault|Permission denied|Failed to start|Dependency failed|Main process exited" | \
    sort | uniq | tail -20 )
if [ -n "$SW_ERRORS" ]; then
    echo "$SW_ERRORS" | while IFS= read -r line; do
        echo "  $line"
    done
else
    echo "  未发现软件错误日志"
fi

# ---------- 7. 最终判定 ----------
echo ""
echo "[*] 统计摘要:"
echo "    计划外重启: $UNPLANNED"
echo "    核心异常集中: ${CORE_FAIL_CORE:-无}"
echo "    SEL错误: ${SEL_ERROR_CLASS:-None}"
echo "    软件错误总数: $SYS_ERROR_COUNT"

if [ "$SEL_HAS_ERROR" -eq 1 ]; then
    echo -e "${GREEN}PASS${NC} (SEL 存在硬件错误，排除 SDC)"
else
    if [ "$reboot_fail" -eq 1 ] && [ "$core_fail" -eq 1 ]; then
        echo -e "${RED}FAIL2${NC} (重启≥3 且 核心异常>60%)"
    elif [ "$reboot_fail" -eq 1 ] || [ "$core_fail" -eq 1 ]; then
        echo -e "${RED}FAIL${NC} (重启或核心异常之一)"
    else
        echo -e "${GREEN}PASS${NC} (无显著异常)"
    fi
fi
