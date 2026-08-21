#include <sandstone.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

#ifdef __aarch64__
#include <arm_acle.h>   // ARM64 CRC32 内联函数
#endif

static constexpr size_t BLOCK_SIZE = 1024;

static int crc32_init(struct test *test) {
    (void)test;
    return EXIT_SUCCESS;
}

#ifdef __aarch64__
static int crc32_run(struct test *test, int cpu) {
    (void)cpu;
    std::vector<uint8_t> local_data(BLOCK_SIZE);
    std::mt19937 rng(std::random_device{}());
    std::uniform_int_distribution<uint8_t> byte_dist(0, 255);

    do {
        for (size_t i = 0; i < BLOCK_SIZE; ++i) {
            local_data[i] = byte_dist(rng);
        }

        // 第一次硬件 CRC 计算（CRC-32C）
        uint32_t crc1 = 0xFFFFFFFF;
        for (size_t i = 0; i < BLOCK_SIZE; ++i) {
            crc1 = __crc32b(crc1, local_data[i]);
        }
        crc1 = ~crc1;   // 最终异或

        // 内存屏障，防止指令重排
        __sync_synchronize();

        // 第二次硬件 CRC 计算
        uint32_t crc2 = 0xFFFFFFFF;
        for (size_t i = 0; i < BLOCK_SIZE; ++i) {
            crc2 = __crc32b(crc2, local_data[i]);
        }
        crc2 = ~crc2;

        bool data_ok = (crc1 == crc2);

        // 一致性测试：存储 CRC 值到内存再加载比较
        uint32_t store_buf = crc1;
        uint32_t reload_buf;
        memcpy(&reload_buf, &store_buf, sizeof(store_buf));
        bool consistent = (reload_buf == crc1);

        bool passed = data_ok && consistent;

        if (!passed) {
            report_fail_msg("crc32: CRC mismatch or consistency failure");
            return EXIT_FAILURE;
        }

    } while (test_time_condition(test));

    return EXIT_SUCCESS;
}
#else
static int crc32_run(struct test *test, int cpu) {
    (void)cpu;
    log_skip(TestResourceIssueSkipCategory,
             "to be implemented (placeholder): ARM __crc32b CRC instruction required");
    return EXIT_SKIP;
}
#endif

static int crc32_finish(struct test *test) {
    (void)test;
    return EXIT_SUCCESS;
}

DECLARE_TEST(crc32, "CRC32 instruction test (random data, dual-check)")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = crc32_init,
    .test_run = crc32_run,
    .test_cleanup = crc32_finish,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
