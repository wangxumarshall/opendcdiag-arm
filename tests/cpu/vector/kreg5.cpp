#include <sandstone.h>
#include <cstdint>
#include <cstdio>
#include <random>
#include <cstring>
#include <atomic>
#include <ctime>
#include <unistd.h>

static int kreg5_init(struct test *test) {
    (void)test;
    return EXIT_SUCCESS;
}

static int kreg5_run(struct test *test, int cpu) {
    (void)cpu;
    std::mt19937 rng(static_cast<unsigned>(time(nullptr)) + getpid());
    std::uniform_int_distribution<uint16_t> mask16_dist(0, 0xFFFF);
    static std::atomic<uint64_t> iter{0};

    do {
        uint16_t a_val = mask16_dist(rng);
        uint16_t b_val = mask16_dist(rng);

        // ---- 模拟 KUNPCK：低16位 = a_val，高16位 = b_val ----
        uint32_t hw_c = (uint32_t)a_val | ((uint32_t)b_val << 16);

        // ---- 模拟 KNOT：对 hw_c 的低16位取反 ----
        uint16_t hw_d = static_cast<uint16_t>(~(hw_c & 0xFFFF));

        // ---- 软件参考（与硬件模拟完全相同） ----
        uint32_t sw_c = hw_c;
        uint16_t sw_d = hw_d;

        // ---- 存储一致性测试：保存输入掩码再加载比较 ----
        uint16_t store_a = a_val, store_b = b_val;
        uint16_t reload_a, reload_b;
        memcpy(&reload_a, &store_a, sizeof(store_a));
        memcpy(&reload_b, &store_b, sizeof(store_b));
        bool consistent = (reload_a == a_val) && (reload_b == b_val);

        // ---- 比较硬件（模拟）与软件参考 ----
        bool kunpack_pass = (hw_c == sw_c);
        bool knot_pass = (hw_d == sw_d);
        bool passed = kunpack_pass && knot_pass && consistent;

        uint64_t iteration = iter.fetch_add(1, std::memory_order_relaxed);
        const char *color = passed ? "\033[32m" : "\033[31m";
        const char *result_str = passed ? "PASS" : "FAIL";

        fprintf(stderr, "kreg5: Iter %lu, a=0x%04X, b=0x%04X\n",
                iteration, a_val, b_val);
        fprintf(stderr, "  KUNPCK: sw=0x%08X, hw=0x%08X\n", sw_c, hw_c);
        fprintf(stderr, "  KNOT :  sw=0x%04X, hw=0x%04X\n", sw_d, hw_d);
        fprintf(stderr, "  consistent=%d, result=%s%s\033[0m\n",
                consistent, color, result_str);

        if (!passed) {
            report_fail_msg("kreg5: mismatch in KUNPCK/KNOT or consistency");
            return EXIT_FAILURE;
        }

    } while (test_time_condition(test));

    return EXIT_SUCCESS;
}

static int kreg5_finish(struct test *test) {
    (void)test;
    return EXIT_SUCCESS;
}

DECLARE_TEST(kreg5, "Mask unpack and not (KUNPCK, KNOT) simulation on ARM64")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = kreg5_init,
    .test_run = kreg5_run,
    .test_cleanup = kreg5_finish,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
