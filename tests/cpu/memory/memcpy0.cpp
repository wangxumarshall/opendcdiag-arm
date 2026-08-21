#include <sandstone.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <atomic>
#include <random>

// 测试块大小：选择典型值如 256 字节，也可调整
static constexpr size_t BLOCK_SIZE = 256;

static int memcpy0_init(struct test *test) {
    (void)test;
    return EXIT_SUCCESS;
}

static int memcpy0_run(struct test *test, int cpu) {
    (void)cpu;

    // 分配源和目标缓冲区（对齐可选，但 memcpy 不要求对齐）
    uint8_t *src = static_cast<uint8_t*>(malloc(BLOCK_SIZE));
    uint8_t *dst = static_cast<uint8_t*>(malloc(BLOCK_SIZE));
    if (!src || !dst) {
        free(src);
        free(dst);
        return EXIT_FAILURE;
    }

    // 填充源数据为全 0（memcpy0 的典型模式）
    memset(src, 0x00, BLOCK_SIZE);

    // 清除目标缓冲区（避免未初始化数据干扰）
    memset(dst, 0xFF, BLOCK_SIZE);

    static std::atomic<uint64_t> iter{0};

    do {
        // 执行内存复制
        memcpy(dst, src, BLOCK_SIZE);

        // 验证数据是否完全一致
        bool data_ok = (memcmp(dst, src, BLOCK_SIZE) == 0);

        // 一致性测试：存储 dst 到新缓冲区，再重新加载比较
        uint8_t *store_buf = static_cast<uint8_t*>(malloc(BLOCK_SIZE));
        if (store_buf) {
            memcpy(store_buf, dst, BLOCK_SIZE);          // 存储
            bool consistent = (memcmp(store_buf, dst, BLOCK_SIZE) == 0);
            free(store_buf);

            // 只有 data_ok 和 consistent 都通过才算通过
            bool passed = data_ok && consistent;

            uint64_t iteration = iter.fetch_add(1, std::memory_order_relaxed);
            const char *color = passed ? "\033[32m" : "\033[31m";
            const char *result_str = passed ? "PASS" : "FAIL";

            // 输出源数据摘要（前 8 个字节）
            fprintf(stderr, "memcpy0: Iter %lu, src[0..7]=%02X %02X %02X %02X %02X %02X %02X %02X\n",
                    iteration,
                    src[0], src[1], src[2], src[3],
                    src[4], src[5], src[6], src[7]);
            fprintf(stderr, "  data_ok=%d, consistent=%d, result=%s%s\033[0m\n",
                    data_ok, consistent, color, result_str);
            fflush(stderr);

            if (!passed) {
                report_fail_msg("memcpy0: data mismatch or consistency failure");
                free(src);
                free(dst);
                return EXIT_FAILURE;
            }
        } else {
            // 分配失败则直接检查 data_ok
            if (!data_ok) {
                report_fail_msg("memcpy0: data mismatch");
                free(src);
                free(dst);
                return EXIT_FAILURE;
            }
        }

    } while (test_time_condition(test));

    free(src);
    free(dst);
    return EXIT_SUCCESS;
}

static int memcpy0_finish(struct test *test) {
    (void)test;
    return EXIT_SUCCESS;
}

DECLARE_TEST(memcpy0, "Basic memory copy (all-zero data)")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = memcpy0_init,
    .test_run = memcpy0_run,
    .test_cleanup = memcpy0_finish,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
