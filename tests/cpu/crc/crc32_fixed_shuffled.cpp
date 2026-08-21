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

static int crc32_fixed_shuffled_init(struct test *test) {
    (void)test;
    return EXIT_SUCCESS;
}

#ifdef __aarch64__
static int crc32_fixed_shuffled_run(struct test *test, int cpu) {
    (void)cpu;
    std::vector<uint8_t> local_data(BLOCK_SIZE);
    std::mt19937 rng(std::random_device{}());
    std::uniform_int_distribution<uint8_t> byte_dist(0, 255);
    std::uniform_int_distribution<int> step_dist(0, 2);  // 0:1字节, 1:2字节, 2:4字节

    do {
        for (size_t i = 0; i < BLOCK_SIZE; ++i) {
            local_data[i] = byte_dist(rng);
        }

        // 生成随机步长序列（保证两次计算使用相同序列）
        std::vector<int> steps;
        size_t pos = 0;
        while (pos < BLOCK_SIZE) {
            int step = step_dist(rng);
            if (step == 0) step = 1;
            else if (step == 1) step = 2;
            else step = 4;
            if (pos + step > BLOCK_SIZE) step = BLOCK_SIZE - pos;
            steps.push_back(step);
            pos += step;
        }

        // 第一次硬件 CRC 计算
        uint32_t crc1 = 0xFFFFFFFF;
        pos = 0;
        for (int step : steps) {
            if (step == 1) {
                crc1 = __crc32b(crc1, local_data[pos]);
            } else if (step == 2) {
                uint16_t val;
                memcpy(&val, &local_data[pos], 2);
                crc1 = __crc32h(crc1, val);
            } else { // step == 4
                uint32_t val;
                memcpy(&val, &local_data[pos], 4);
                crc1 = __crc32w(crc1, val);
            }
            pos += step;
        }
        crc1 = ~crc1;

        // 内存屏障
        __sync_synchronize();

        // 第二次硬件 CRC 计算（重用相同步长序列）
        uint32_t crc2 = 0xFFFFFFFF;
        pos = 0;
        for (int step : steps) {
            if (step == 1) {
                crc2 = __crc32b(crc2, local_data[pos]);
            } else if (step == 2) {
                uint16_t val;
                memcpy(&val, &local_data[pos], 2);
                crc2 = __crc32h(crc2, val);
            } else { // step == 4
                uint32_t val;
                memcpy(&val, &local_data[pos], 4);
                crc2 = __crc32w(crc2, val);
            }
            pos += step;
        }
        crc2 = ~crc2;

        bool data_ok = (crc1 == crc2);

        uint32_t store_buf = crc1;
        uint32_t reload_buf;
        memcpy(&reload_buf, &store_buf, sizeof(store_buf));
        bool consistent = (reload_buf == crc1);

        bool passed = data_ok && consistent;

        if (!passed) {
            report_fail_msg("crc32_fixed_shuffled: CRC mismatch or consistency failure");
            return EXIT_FAILURE;
        }

    } while (test_time_condition(test));

    return EXIT_SUCCESS;
}
#else
static int crc32_fixed_shuffled_run(struct test *test, int cpu) {
    (void)cpu;
    log_skip(TestResourceIssueSkipCategory,
             "to be implemented (placeholder): ARM __crc32b/h/w CRC instruction required");
    return EXIT_SKIP;
}
#endif

static int crc32_fixed_shuffled_finish(struct test *test) {
    (void)test;
    return EXIT_SUCCESS;
}

DECLARE_TEST(crc32_fixed_shuffled, "CRC32 instruction with shuffled access and mixed width (dual-check)")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = crc32_fixed_shuffled_init,
    .test_run = crc32_fixed_shuffled_run,
    .test_cleanup = crc32_fixed_shuffled_finish,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
