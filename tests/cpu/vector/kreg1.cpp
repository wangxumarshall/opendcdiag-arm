#include <sandstone.h>
#include <cstdint>
#include <cstdio>
#include <random>
#include <cstring>
#include <atomic>
#include <ctime>

#ifdef __aarch64__
#include <arm_neon.h>

static int kreg1_init(struct test *test) {
    (void)test;
    return EXIT_SUCCESS;
}

static int kreg1_run(struct test *test, int cpu) {
    (void)cpu;
    std::mt19937 rng(static_cast<unsigned>(time(nullptr)) + getpid());
    std::uniform_real_distribution<float> dist(-100.0f, 100.0f);
    static std::atomic<uint64_t> iter{0};

    do {
        // 1. 生成两个随机 128 位向量（4 个 float）
        float a_arr[4], b_arr[4];
        for (int i = 0; i < 4; ++i) {
            a_arr[i] = dist(rng);
            b_arr[i] = dist(rng);
        }
        float32x4_t vec_a = vld1q_f32(a_arr);
        float32x4_t vec_b = vld1q_f32(b_arr);

        // 2. 比较产生掩码 (NEON 比较结果为 0xFFFFFFFF 或 0)
        uint32x4_t lt = vcltq_f32(vec_a, vec_b);
        uint32x4_t gt = vcgtq_f32(vec_a, vec_b);

        // 提取低 4 位掩码 (每个 lane 取最低位)
        uint8_t mask1 = 0, mask2 = 0;
        for (int i = 0; i < 4; ++i) {
            if (vgetq_lane_u32(lt, i)) mask1 |= (1 << i);
            if (vgetq_lane_u32(gt, i)) mask2 |= (1 << i);
        }

        // 3. 掩码逻辑运算 (与 x86 完全一致)
        uint8_t and_mask  = mask1 & mask2;
        uint8_t or_mask   = mask1 | mask2;
        uint8_t xor_mask  = mask1 ^ mask2;
        uint8_t nand_mask = (~mask1) & mask2;   // 等价于 _mm512_kandn(mask1, mask2)
        uint8_t xnor_mask = ~(mask1 ^ mask2) & 0xF; // 等价于 _mm512_kxnor(mask1, mask2)

        // 4. 使用 or_mask 混合两个向量 (模拟 _mm_mask_blend_ps)
        // 构造掩码向量 (每个 lane 根据 or_mask 对应位为全 1 或全 0)
        uint32x4_t blend_mask = vdupq_n_u32(0);
        for (int i = 0; i < 4; ++i) {
            uint32_t val = (or_mask & (1 << i)) ? 0xFFFFFFFF : 0;
            blend_mask = vsetq_lane_u32(val, blend_mask, i);
        }
        // vbslq_f32: 如果掩码位为 1 选择 vec_b，否则选择 vec_a
        float32x4_t result = vbslq_f32(blend_mask, vec_b, vec_a);

        // 5. Store/load 一致性验证
        float store_buf[4];
        vst1q_f32(store_buf, result);
        float32x4_t reload = vld1q_f32(store_buf);
        uint32x4_t cmp = vceqq_f32(result, reload);
        bool consistent = (vgetq_lane_u32(cmp, 0) && vgetq_lane_u32(cmp, 1) &&
                           vgetq_lane_u32(cmp, 2) && vgetq_lane_u32(cmp, 3));

        // 6. 软件参考计算 (逐元素比较得到掩码，与 x86 相同)
        uint8_t sw_mask1 = 0, sw_mask2 = 0;
        for (int i = 0; i < 4; ++i) {
            if (a_arr[i] < b_arr[i]) sw_mask1 |= (1 << i);
            if (a_arr[i] > b_arr[i]) sw_mask2 |= (1 << i);
        }
        uint8_t sw_and  = sw_mask1 & sw_mask2;
        uint8_t sw_or   = sw_mask1 | sw_mask2;
        uint8_t sw_xor  = sw_mask1 ^ sw_mask2;
        uint8_t sw_nand = (~sw_mask1) & sw_mask2;
        uint8_t sw_xnor = ~(sw_mask1 ^ sw_mask2) & 0xF;

        // 7. 比较硬件与软件结果
        bool mask_pass = (sw_and == and_mask) && (sw_or == or_mask) &&
                         (sw_xor == xor_mask) && (sw_nand == nand_mask) &&
                         (sw_xnor == xnor_mask);
        bool passed = mask_pass && consistent;

        uint64_t iteration = iter.fetch_add(1, std::memory_order_relaxed);
        const char *color = passed ? "\033[32m" : "\033[31m";
        const char *result_str = passed ? "PASS" : "FAIL";

        // 8. 详细输出 (与 x86 版本格式保持一致)
        fprintf(stderr, "kreg1: Iter %lu, a=[%.2f,%.2f,%.2f,%.2f], b=[%.2f,%.2f,%.2f,%.2f]\n",
                iteration, a_arr[0], a_arr[1], a_arr[2], a_arr[3],
                b_arr[0], b_arr[1], b_arr[2], b_arr[3]);
        fprintf(stderr, "  sw: and=%x or=%x xor=%x nand=%x xnor=%x\n",
                sw_and, sw_or, sw_xor, sw_nand, sw_xnor);
        fprintf(stderr, "  hw: and=%x or=%x xor=%x nand=%x xnor=%x\n",
                and_mask, or_mask, xor_mask, nand_mask, xnor_mask);
        fprintf(stderr, "  consistent=%d, result=%s%s\033[0m\n",
                consistent, color, result_str);

        if (!passed) {
            report_fail_msg("kreg1: mismatch in mask logic or consistency");
            return EXIT_FAILURE;
        }

    } while (test_time_condition(test));

    return EXIT_SUCCESS;
}

static int kreg1_finish(struct test *test) {
    (void)test;
    return EXIT_SUCCESS;
}

#else // !__aarch64__

// On non-AArch64 (e.g. x86-64) there is no <arm_neon.h>; the test is kept
// listed and schedulable but reports a clean resource skip so the result is
// marked ignored rather than spuriously passing. The NEON port is the
// counterpart of the x86 AVX-512 k-register mask logic tests.
static int kreg1_init(struct test *test) {
    (void)test;
    log_skip(TestResourceIssueSkipCategory,
             "to be implemented (placeholder): ARM NEON required for "
             "mask register basic logic operations (128-bit SIMD)");
    return EXIT_SKIP;
}

static int kreg1_run(struct test *test, int cpu) {
    (void)test;
    (void)cpu;
    __builtin_unreachable();    // init reports EXIT_SKIP, run never executes
}

static int kreg1_finish(struct test *test) {
    (void)test;
    return EXIT_SUCCESS;
}

#endif // __aarch64__

DECLARE_TEST(kreg1, "Mask register basic logic operations (128-bit SIMD) on ARM NEON")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = kreg1_init,
    .test_run = kreg1_run,
    .test_cleanup = kreg1_finish,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
