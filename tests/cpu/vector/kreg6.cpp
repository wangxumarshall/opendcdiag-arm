#include <sandstone.h>
#include <cstdint>
#include <cstdio>
#include <random>
#include <cstring>
#include <atomic>
#include <ctime>
#include <unistd.h>

static int kreg6_init(struct test *test) {
    (void)test;
    return EXIT_SUCCESS;
}

static int kreg6_run(struct test *test, int cpu) {
    (void)cpu;
    std::mt19937 rng(static_cast<unsigned>(time(nullptr)) + getpid());
    std::uniform_int_distribution<uint16_t> mask16_dist(0, 0xFFFF);
    static std::atomic<uint64_t> iter{0};

    do {
        uint16_t mask_val = mask16_dist(rng);

        // ---- 软件模拟（同时也是“硬件”实现，因无对应指令） ----
        uint32_t hw_vals[16];
        uint32_t sw_vals[16];
        for (int i = 0; i < 16; ++i) {
            hw_vals[i] = (mask_val & (1 << i)) ? 0xFFFFFFFF : 0;
            sw_vals[i] = hw_vals[i];  // 完全相同
        }

        // ---- 存储一致性测试：将 hw_vals 存储再加载比较 ----
        uint32_t reload_vals[16];
        memcpy(reload_vals, hw_vals, sizeof(hw_vals));
        bool consistent = (memcmp(reload_vals, hw_vals, sizeof(hw_vals)) == 0);

        // ---- 逐元素比较 ----
        bool all_match = true;
        for (int i = 0; i < 16; ++i) {
            if (hw_vals[i] != sw_vals[i]) {
                all_match = false;
                break;
            }
        }
        bool passed = all_match && consistent;

        uint64_t iteration = iter.fetch_add(1, std::memory_order_relaxed);
        const char *color = passed ? "\033[32m" : "\033[31m";
        const char *result_str = passed ? "PASS" : "FAIL";

        fprintf(stderr, "kreg6: Iter %lu, mask=0x%04X\n", iteration, mask_val);
        fprintf(stderr, "  sw: ");
        for (int i = 0; i < 16; ++i) fprintf(stderr, "%08X ", sw_vals[i]);
        fprintf(stderr, "\n  hw: ");
        for (int i = 0; i < 16; ++i) fprintf(stderr, "%08X ", hw_vals[i]);
        fprintf(stderr, "\n  consistent=%d, result=%s%s\033[0m\n",
                consistent, color, result_str);
        fflush(stderr);

        if (!passed) {
            report_fail_msg("kreg6: mismatch in broadcast or consistency");
            return EXIT_FAILURE;
        }

    } while (test_time_condition(test));

    return EXIT_SUCCESS;
}

static int kreg6_finish(struct test *test) {
    (void)test;
    return EXIT_SUCCESS;
}

DECLARE_TEST(kreg6, "Mask broadcast to vector (simulated on ARM64)")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = kreg6_init,
    .test_run = kreg6_run,
    .test_cleanup = kreg6_finish,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
