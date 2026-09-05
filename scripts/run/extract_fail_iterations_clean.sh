#!/bin/bash

mkdir -p fail_only_clean

# 测试列表（请确保与您的列表完全一致）
tests=(
    memcpy_shuffle_v2
    memcpy_shuffle
    isal_crc_iscsi
    isal_crc_ieee
    isal_crc64_jones_norm
    isal_crc64_iso_refl
    isal_crc64_iso_norm
    isal_crc64_ecma182_refl
    isal_crc64_ecma182_norm
    isal_crc32_gzip
    isal_crc_t10dif
    isal_crc64_jones_refl
    crc32_fixed_shuffled
    crc32_fixed
    crc32
)

for test in "${tests[@]}"; do
    echo "处理测试: $test"
    # 提取该测试的完整块（从 - test: 到下一个 - test: 或文件末尾）
    awk -v t="$test" '
        /^- test: / {
            if (flag) exit
            if ($0 ~ "^- test: " t) flag=1
            else flag=0
        }
        flag { print }
    ' ./logs/run_1.log | \
    # 在块内提取包含 data[0..7] 和 data_ok=0/consistent=0 的行及其前一行
    grep -B 1 -E "(data_ok=0|consistent=0)" | \
    # 过滤掉可能残留的 "^- test:" 或 "stderr messages:" 行
    grep -v "^- test:" | grep -v "stderr messages:" \
    > "fail_only_clean/${test}_fail_iterations.log"
done

echo "提取完成！文件存放在 fail_only_clean/ 目录下。"
