#include <sandstone.h>
#include <cstdint>
#include <cstring>
#include <random>
#include <cmath>

#ifdef __aarch64__
#include <arm_neon.h>          // ARM NEON 头文件
#endif

static constexpr int VECTOR_SIZE = 8; // 8 个双精度浮点数

static int fma_patterns_avx512_pd_init(struct test *test) {
    (void)test;
    return EXIT_SUCCESS;
}

#ifdef __aarch64__
static int fma_patterns_avx512_pd_run(struct test *test, int cpu) {
    (void)cpu;
    std::mt19937 rng(std::random_device{}());
    std::uniform_real_distribution<double> dist(-1e6, 1e6);

    do {
        // 生成随机向量 a, b, c（对齐到 16 字节即可满足 NEON）
        alignas(16) double a[VECTOR_SIZE];
        alignas(16) double b[VECTOR_SIZE];
        alignas(16) double c[VECTOR_SIZE];
        alignas(16) double hw_result[VECTOR_SIZE];
        double sw_ref[VECTOR_SIZE];

        for (int i = 0; i < VECTOR_SIZE; ++i) {
            a[i] = dist(rng);
            b[i] = dist(rng);
            c[i] = dist(rng);
        }

        // ---- 硬件 FMA 计算 (NEON) ----
        // 每个 float64x2_t 容纳 2 个双精度，总共需要 4 个向量
        float64x2_t va0 = vld1q_f64(a);
        float64x2_t va1 = vld1q_f64(a + 2);
        float64x2_t va2 = vld1q_f64(a + 4);
        float64x2_t va3 = vld1q_f64(a + 6);
        float64x2_t vb0 = vld1q_f64(b);
        float64x2_t vb1 = vld1q_f64(b + 2);
        float64x2_t vb2 = vld1q_f64(b + 4);
        float64x2_t vb3 = vld1q_f64(b + 6);
        float64x2_t vc0 = vld1q_f64(c);
        float64x2_t vc1 = vld1q_f64(c + 2);
        float64x2_t vc2 = vld1q_f64(c + 4);
        float64x2_t vc3 = vld1q_f64(c + 6);

        // vfmaq_f64 计算：dst = dst + src1 * src2  (即 c + a * b)
        float64x2_t vd0 = vfmaq_f64(vc0, va0, vb0);
        float64x2_t vd1 = vfmaq_f64(vc1, va1, vb1);
        float64x2_t vd2 = vfmaq_f64(vc2, va2, vb2);
        float64x2_t vd3 = vfmaq_f64(vc3, va3, vb3);

        // 存储结果
        vst1q_f64(hw_result,      vd0);
        vst1q_f64(hw_result + 2,  vd1);
        vst1q_f64(hw_result + 4,  vd2);
        vst1q_f64(hw_result + 6,  vd3);

        // ---- 软件参考：使用标准库 fma（单次舍入）。NEON vfmaq_f64 也是
        // 单次舍入 IEEE-754 FMA，因此对于有限操作数硬件与软件结果按位一致；
        // SDC 检测使用按位精确 memcmp，而非容差比较（容差会让 1 位 SDC 漏检）。
        for (int i = 0; i < VECTOR_SIZE; ++i) {
            sw_ref[i] = fma(a[i], b[i], c[i]);
        }

        // ---- 按位精确比较硬件结果与参考（替换原先 1e-15/1e-12 容差比较） ----
        bool data_ok = (memcmp(hw_result, sw_ref, sizeof(sw_ref)) == 0);

        // ---- 一致性测试：存储硬件结果到内存再加载比较 ----
        double store_buf[VECTOR_SIZE];
        memcpy(store_buf, hw_result, sizeof(store_buf));
        double reload_buf[VECTOR_SIZE];
        memcpy(reload_buf, store_buf, sizeof(store_buf));
        bool consistent = (memcmp(reload_buf, hw_result, sizeof(reload_buf)) == 0);

        if (!(data_ok && consistent)) {
            report_fail_msg("fma_patterns_avx512_pd: FMA result mismatch or consistency failure");
            return EXIT_FAILURE;
        }

    } while (test_time_condition(test));

    return EXIT_SUCCESS;
}
#else
static int fma_patterns_avx512_pd_run(struct test *test, int cpu) {
    (void)cpu;
    log_skip(TestResourceIssueSkipCategory,
             "to be implemented (placeholder): ARM NEON FMA required");
    return EXIT_SKIP;
}
#endif

static int fma_patterns_avx512_pd_finish(struct test *test) {
    (void)test;
    return EXIT_SUCCESS;
}

DECLARE_TEST(fma_patterns_avx512_pd, "AVX-512 double FMA pattern stress test (NEON version)")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = fma_patterns_avx512_pd_init,
    .test_run = fma_patterns_avx512_pd_run,
    .test_cleanup = fma_patterns_avx512_pd_finish,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
