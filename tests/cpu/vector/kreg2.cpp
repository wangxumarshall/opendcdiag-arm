#include <sandstone.h>
#include <cstdint>
#include <cstdio>
#include <random>
#include <cstring>
#include <atomic>
#include <ctime>

// 模拟掩码左移：直接使用整数移位（在 ARM64 上编译为普通移位指令）
static inline uint16_t kshiftl(uint16_t src, int amount) {
    return static_cast<uint16_t>(src << amount);
}

// 模拟掩码右移
static inline uint16_t kshiftr(uint16_t src, int amount) {
    return static_cast<uint16_t>(src >> amount);
}

static int kreg2_init(struct test *test) {
    (void)test;
    return EXIT_SUCCESS;
}

static int kreg2_run(struct test *test, int cpu) {
    (void)cpu;
    std::mt19937 rng(static_cast<unsigned>(time(nullptr)) + getpid());
    std::uniform_int_distribution<uint16_t> mask_dist(0, 0xFFFF);
    std::uniform_int_distribution<int> shift_dist(0, 15);
    static std::atomic<uint64_t> iter{0};

    do {
        uint16_t src_mask_val = mask_dist(rng);
        int shift_amount = shift_dist(rng);

        // 硬件执行移位（在 ARM64 上，这里就是普通整数移位指令）
        uint16_t left_shifted  = kshiftl(src_mask_val, shift_amount);
        uint16_t right_shifted = kshiftr(src_mask_val, shift_amount);

        // 软件参考（同样的计算，用于对比）
        uint16_t sw_left  = static_cast<uint16_t>(src_mask_val << shift_amount);
        uint16_t sw_right = static_cast<uint16_t>(src_mask_val >> shift_amount);

        // 存储一致性测试：将结果写入内存并重新加载
        uint16_t store_buf[2] = {left_shifted, right_shifted};
        uint16_t reload_buf[2];
        memcpy(reload_buf, store_buf, sizeof(store_buf));
        bool consistent = (reload_buf[0] == left_shifted) && (reload_buf[1] == right_shifted);

        // 比较硬件（实际计算结果）与软件参考
        bool left_pass  = (sw_left == left_shifted);
        bool right_pass = (sw_right == right_shifted);
        bool passed = left_pass && right_pass && consistent;

        uint64_t iteration = iter.fetch_add(1, std::memory_order_relaxed);
        const char *color = passed ? "\033[32m" : "\033[31m";
        const char *result_str = passed ? "PASS" : "FAIL";

        fprintf(stderr, "kreg2: Iter %lu, src=0x%04X, shift=%d\n",
                iteration, src_mask_val, shift_amount);
        fprintf(stderr, "  sw: left=0x%04X, right=0x%04X\n", sw_left, sw_right);
        fprintf(stderr, "  hw: left=0x%04X, right=0x%04X\n", left_shifted, right_shifted);
        fprintf(stderr, "  consistent=%d, result=%s%s\033[0m\n",
                consistent, color, result_str);

        if (!passed) {
            report_fail_msg("kreg2: mismatch in shift or consistency");
            return EXIT_FAILURE;
        }

    } while (test_time_condition(test));

    return EXIT_SUCCESS;
}

static int kreg2_finish(struct test *test) {
    (void)test;
    return EXIT_SUCCESS;
}

DECLARE_TEST(kreg2, "Mask register shift operations (KSHIFTL, KSHIFTR) simulation on ARM64")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = kreg2_init,
    .test_run = kreg2_run,
    .test_cleanup = kreg2_finish,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
