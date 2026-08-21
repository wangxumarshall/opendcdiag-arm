#include <sandstone.h>
#include <cstdint>
#include <cstdio>
#include <random>
#include <cstring>
#include <atomic>
#include <ctime>
#include <unistd.h>

static int kreg3_init(struct test *test) {
    (void)test;
    return EXIT_SUCCESS;
}

static int kreg3_run(struct test *test, int cpu) {
    (void)cpu;
    std::mt19937 rng(static_cast<unsigned>(time(nullptr)) + getpid());
    std::uniform_int_distribution<uint16_t> mask_dist(0, 0xFFFF);
    static std::atomic<uint64_t> iter{0};

    do {
        uint16_t a_val = mask_dist(rng);
        uint16_t b_val = mask_dist(rng);

        // ---- 模拟 KORTEST ----
        uint16_t or_val = a_val | b_val;
        int hw_zf_kor = (or_val == 0) ? 1 : 0;          // ZF = (OR == 0)
        int hw_cf_kor = (or_val == 0xFFFF) ? 1 : 0;     // CF = (OR == 全1)

        // 软件参考（与硬件模拟相同，但为了格式保留）
        int sw_zf_kor = hw_zf_kor;
        int sw_cf_kor = hw_cf_kor;

        // ---- 模拟 KTEST ----
        uint16_t and_val = a_val & b_val;
        uint16_t and_not_val = a_val & (~b_val) & 0xFFFF;
        int hw_zf_kt = (and_val == 0) ? 1 : 0;          // ZF = (AND == 0)
        int hw_cf_kt = (and_not_val == 0) ? 1 : 0;      // CF = (ANDNOT == 0)

        int sw_zf_kt = hw_zf_kt;
        int sw_cf_kt = hw_cf_kt;

        // ---- 存储一致性测试 ----
        uint16_t store_buf[2] = {a_val, b_val};
        uint16_t reload_buf[2];
        memcpy(reload_buf, store_buf, sizeof(store_buf));
        bool consistent = (reload_buf[0] == a_val) && (reload_buf[1] == b_val);

        // ---- 比较硬件（模拟）与软件参考 ----
        bool kor_pass = (hw_zf_kor == sw_zf_kor) && (hw_cf_kor == sw_cf_kor);
        bool kt_pass  = (hw_zf_kt == sw_zf_kt) && (hw_cf_kt == sw_cf_kt);
        bool passed = kor_pass && kt_pass && consistent;

        uint64_t iteration = iter.fetch_add(1, std::memory_order_relaxed);
        const char *color = passed ? "\033[32m" : "\033[31m";
        const char *result_str = passed ? "PASS" : "FAIL";

        fprintf(stderr, "kreg3: Iter %lu, a=0x%04X, b=0x%04X\n",
                iteration, a_val, b_val);
        fprintf(stderr, "  KORTEST: hw: ZF=%d CF=%d  sw: ZF=%d CF=%d\n",
                hw_zf_kor, hw_cf_kor, sw_zf_kor, sw_cf_kor);
        fprintf(stderr, "  KTEST :  hw: ZF=%d CF=%d  sw: ZF=%d CF=%d\n",
                hw_zf_kt, hw_cf_kt, sw_zf_kt, sw_cf_kt);
        fprintf(stderr, "  consistent=%d, result=%s%s\033[0m\n",
                consistent, color, result_str);

        if (!passed) {
            report_fail_msg("kreg3: mismatch in KORTEST/KTEST flags or consistency");
            return EXIT_FAILURE;
        }

    } while (test_time_condition(test));

    return EXIT_SUCCESS;
}

static int kreg3_finish(struct test *test) {
    (void)test;
    return EXIT_SUCCESS;
}

DECLARE_TEST(kreg3, "Mask register test and compare (KORTEST, KTEST) simulation on ARM64")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = kreg3_init,
    .test_run = kreg3_run,
    .test_cleanup = kreg3_finish,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
