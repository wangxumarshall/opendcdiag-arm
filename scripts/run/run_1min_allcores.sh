#!/bin/bash
# 批量运行 15 个测试用例，每个 1 分钟
# 所有日志写入 fail_1min/，终端只显示简要进度

mkdir -p fail_1min

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
    echo -n "运行 $test (1分钟)... "
    # 使用 -q 静默，-o 写日志，并将所有标准输出/错误丢弃（终端无输出）
    ./builddir/sdcshield --enable="$test" -t 1m -T 2m --strict-runtime -q -o "fail_1min/${test}_1min.log" > /dev/null 2>&1
    if [ $? -eq 0 ]; then
        echo "✅ PASS"
    else
        echo "❌ FAIL"
    fi
done

echo "所有测试完成。日志保存在 fail_1min/ 目录。"
