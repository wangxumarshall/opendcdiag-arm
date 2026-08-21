#include <sandstone.h>
#include <cstdint>
#include <cstdio>
#include <random>
#include <cstring>
#include <atomic>
#include <ctime>
#include <unistd.h>

#ifdef __aarch64__
#include <arm_neon.h>

static int kreg4_init(struct test *test) {
    (void)test;
    return EXIT_SUCCESS;
}

static int kreg4_run(struct test *test, int cpu) {
    (void)cpu;
    std::mt19937 rng(static_cast<unsigned>(time(nullptr)) + getpid());
    std::uniform_int_distribution<uint32_t> val_dist(0, 0xFFFFFFFF);
    static std::atomic<uint64_t> iter{0};

    do {
        // 1. 生成随机 16 个 32 位整数
        uint32_t a_vals[16], b_vals[16];
        for (int i = 0; i < 16; ++i) {
            a_vals[i] = val_dist(rng);
            b_vals[i] = val_dist(rng);
        }

        // 2. 使用 NEON 生成掩码（模拟 VPTESTM 和 VPTESTNM）
        //    将 16 个元素分成 4 组，每组 4 个
        uint32x4_t a0 = vld1q_u32(&a_vals[0]);
        uint32x4_t a1 = vld1q_u32(&a_vals[4]);
        uint32x4_t a2 = vld1q_u32(&a_vals[8]);
        uint32x4_t a3 = vld1q_u32(&a_vals[12]);
        uint32x4_t b0 = vld1q_u32(&b_vals[0]);
        uint32x4_t b1 = vld1q_u32(&b_vals[4]);
        uint32x4_t b2 = vld1q_u32(&b_vals[8]);
        uint32x4_t b3 = vld1q_u32(&b_vals[12]);

        // 按位与
        uint32x4_t and0 = vandq_u32(a0, b0);
        uint32x4_t and1 = vandq_u32(a1, b1);
        uint32x4_t and2 = vandq_u32(a2, b2);
        uint32x4_t and3 = vandq_u32(a3, b3);

        // 生成 VPTESTM 掩码：位 = 1 当且仅当 (and != 0)
        // 使用 vceqzq_u32 得到 (and == 0) 的掩码，然后取反
        uint32x4_t m0 = vmvnq_u32(vceqzq_u32(and0));
        uint32x4_t m1 = vmvnq_u32(vceqzq_u32(and1));
        uint32x4_t m2 = vmvnq_u32(vceqzq_u32(and2));
        uint32x4_t m3 = vmvnq_u32(vceqzq_u32(and3));

        // 提取每个 lane 的最低有效位（0 或 1）组合成 16 位掩码
        uint16_t hw_mask_m = 0;
        for (int i = 0; i < 4; ++i) {
            // 分别处理每组 4 个元素
            uint32_t v0 = vgetq_lane_u32(m0, i);
            uint32_t v1 = vgetq_lane_u32(m1, i);
            uint32_t v2 = vgetq_lane_u32(m2, i);
            uint32_t v3 = vgetq_lane_u32(m3, i);
            // 取最低位
            int bit0 = (v0 & 1) ? 1 : 0;
            int bit1 = (v1 & 1) ? 1 : 0;
            int bit2 = (v2 & 1) ? 1 : 0;
            int bit3 = (v3 & 1) ? 1 : 0;
            // 位置：组内 i 对应全局索引 i, i+4, i+8, i+12
            int idx0 = i;
            int idx1 = i + 4;
            int idx2 = i + 8;
            int idx3 = i + 12;
            if (bit0) hw_mask_m |= (1 << idx0);
            if (bit1) hw_mask_m |= (1 << idx1);
            if (bit2) hw_mask_m |= (1 << idx2);
            if (bit3) hw_mask_m |= (1 << idx3);
        }

        // VPTESTNM 掩码：位 = 1 当且仅当 (and == 0)
        // 直接使用 vceqzq_u32 的结果
        uint32x4_t nm0 = vceqzq_u32(and0);
        uint32x4_t nm1 = vceqzq_u32(and1);
        uint32x4_t nm2 = vceqzq_u32(and2);
        uint32x4_t nm3 = vceqzq_u32(and3);
        uint16_t hw_mask_nm = 0;
        for (int i = 0; i < 4; ++i) {
            uint32_t v0 = vgetq_lane_u32(nm0, i);
            uint32_t v1 = vgetq_lane_u32(nm1, i);
            uint32_t v2 = vgetq_lane_u32(nm2, i);
            uint32_t v3 = vgetq_lane_u32(nm3, i);
            int bit0 = (v0 & 1) ? 1 : 0;
            int bit1 = (v1 & 1) ? 1 : 0;
            int bit2 = (v2 & 1) ? 1 : 0;
            int bit3 = (v3 & 1) ? 1 : 0;
            int idx0 = i;
            int idx1 = i + 4;
            int idx2 = i + 8;
            int idx3 = i + 12;
            if (bit0) hw_mask_nm |= (1 << idx0);
            if (bit1) hw_mask_nm |= (1 << idx1);
            if (bit2) hw_mask_nm |= (1 << idx2);
            if (bit3) hw_mask_nm |= (1 << idx3);
        }

        // 3. 软件参考（逐元素计算）
        uint16_t sw_mask_m = 0, sw_mask_nm = 0;
        for (int i = 0; i < 16; ++i) {
            uint32_t and_val = a_vals[i] & b_vals[i];
            if (and_val != 0) {
                sw_mask_m |= (1 << i);
            } else {
                sw_mask_nm |= (1 << i);
            }
        }

        // 4. 存储一致性测试：将输入向量存储再加载比较
        uint32_t reload_a[16], reload_b[16];
        vst1q_u32(&reload_a[0], a0);
        vst1q_u32(&reload_a[4], a1);
        vst1q_u32(&reload_a[8], a2);
        vst1q_u32(&reload_a[12], a3);
        vst1q_u32(&reload_b[0], b0);
        vst1q_u32(&reload_b[4], b1);
        vst1q_u32(&reload_b[8], b2);
        vst1q_u32(&reload_b[12], b3);
        bool consistent_a = (memcmp(reload_a, a_vals, sizeof(a_vals)) == 0);
        bool consistent_b = (memcmp(reload_b, b_vals, sizeof(b_vals)) == 0);
        bool consistent = consistent_a && consistent_b;

        // 5. 比较硬件与软件
        bool mask_m_pass = (hw_mask_m == sw_mask_m);
        bool mask_nm_pass = (hw_mask_nm == sw_mask_nm);
        bool passed = mask_m_pass && mask_nm_pass && consistent;

        uint64_t iteration = iter.fetch_add(1, std::memory_order_relaxed);
        const char *color = passed ? "\033[32m" : "\033[31m";
        const char *result_str = passed ? "PASS" : "FAIL";

        // 6. 输出详细信息（仅显示前几个元素）
        fprintf(stderr, "kreg4: Iter %lu, a[0..3]=0x%08X,0x%08X,0x%08X,0x%08X\n",
                iteration, a_vals[0], a_vals[1], a_vals[2], a_vals[3]);
        fprintf(stderr, "          b[0..3]=0x%08X,0x%08X,0x%08X,0x%08X\n",
                b_vals[0], b_vals[1], b_vals[2], b_vals[3]);
        fprintf(stderr, "  sw: VPTESTM=0x%04X, VPTESTNM=0x%04X\n", sw_mask_m, sw_mask_nm);
        fprintf(stderr, "  hw: VPTESTM=0x%04X, VPTESTNM=0x%04X\n", hw_mask_m, hw_mask_nm);
        fprintf(stderr, "  consistent=%d, result=%s%s\033[0m\n",
                consistent, color, result_str);

        if (!passed) {
            report_fail_msg("kreg4: mismatch in VPTESTM/VPTESTNM masks or consistency");
            return EXIT_FAILURE;
        }

    } while (test_time_condition(test));

    return EXIT_SUCCESS;
}

static int kreg4_finish(struct test *test) {
    (void)test;
    return EXIT_SUCCESS;
}

#else // !__aarch64__

// On non-AArch64 (e.g. x86-64) there is no <arm_neon.h>; the test is kept
// listed and schedulable but reports a clean resource skip so the result is
// marked ignored rather than spuriously passing. The NEON port is the
// counterpart of the x86 AVX-512 VPTESTM/VPTESTNM mask-generation tests.
static int kreg4_init(struct test *test) {
    (void)test;
    log_skip(TestResourceIssueSkipCategory,
             "to be implemented (placeholder): ARM NEON required for "
             "vector test instructions generating masks (VPTESTM, VPTESTNM)");
    return EXIT_SKIP;
}

static int kreg4_run(struct test *test, int cpu) {
    (void)test;
    (void)cpu;
    __builtin_unreachable();    // init reports EXIT_SKIP, run never executes
}

static int kreg4_finish(struct test *test) {
    (void)test;
    return EXIT_SUCCESS;
}

#endif // __aarch64__

DECLARE_TEST(kreg4, "Vector test instructions generating masks (VPTESTM, VPTESTNM) on ARM64")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = kreg4_init,
    .test_run = kreg4_run,
    .test_cleanup = kreg4_finish,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
