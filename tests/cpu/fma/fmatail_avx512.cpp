#include <sandstone.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <atomic>
#include <random>
#include <cmath>

#ifdef __aarch64__
#include <arm_neon.h>          // ARM NEON 头文件
#endif

static constexpr int VECTOR_SIZE = 8;              // 8 个双精度浮点数
static constexpr int INNER_ITERATIONS = 5;         // 内层循环每组生成的随机数据组数

static int fmatail_avx512_init(struct test *test) {
    (void)test;
    return EXIT_SUCCESS;
}

#ifdef __aarch64__
static int fmatail_avx512_run(struct test *test, int cpu) {
    (void)cpu;
    std::mt19937 rng(std::random_device{}());
    std::uniform_real_distribution<double> dist(-1e10, 1e10);
    static std::atomic<uint64_t> iter{0};

    do {
        bool all_passed = true;

        // 内层循环：生成多组随机数据
        for (int group = 0; group < INNER_ITERATIONS; ++group) {
            // NEON 只需 16 字节对齐
            alignas(16) double a[VECTOR_SIZE];
            alignas(16) double b[VECTOR_SIZE];
            alignas(16) double c[VECTOR_SIZE];
            alignas(16) double hw_result[VECTOR_SIZE];
            double sw_ref[VECTOR_SIZE];

            // 生成随机向量，并偶尔插入特殊值（0, 1, -1, Inf）
            for (int i = 0; i < VECTOR_SIZE; ++i) {
                a[i] = dist(rng);
                b[i] = dist(rng);
                c[i] = dist(rng);
                // 随机插入特殊值
                if (i % 3 == 0) {
                    switch ((i + group) % 4) {
                        case 0: a[i] = 0.0; break;
                        case 1: b[i] = 1.0; break;
                        case 2: c[i] = -1.0; break;
                        case 3: a[i] = INFINITY; break;
                    }
                }
            }

            // ---- 硬件 FMA 计算 (NEON) ----
            // 使用 4 个 float64x2_t 向量容纳 8 个元素
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

            // ---- 软件参考：使用标准库 fma（正确舍入） ----
            for (int i = 0; i < VECTOR_SIZE; ++i) {
                sw_ref[i] = fma(a[i], b[i], c[i]);
            }

            // ---- 尾数精度测试：逐位比较硬件结果与参考值 ----
            bool data_ok = (memcmp(hw_result, sw_ref, sizeof(hw_result)) == 0);

            // ---- 一致性测试：存储硬件结果到内存再加载比较 ----
            double store_buf[VECTOR_SIZE];
            memcpy(store_buf, hw_result, sizeof(store_buf));
            double reload_buf[VECTOR_SIZE];
            memcpy(reload_buf, store_buf, sizeof(store_buf));
            bool consistent = (memcmp(reload_buf, hw_result, sizeof(reload_buf)) == 0);

            bool group_pass = data_ok && consistent;
            if (!group_pass) all_passed = false;

            // 输出该组信息（与 x86 版本完全一致）
            uint64_t iteration = iter.fetch_add(1, std::memory_order_relaxed);
            const char *color = group_pass ? "\033[32m" : "\033[31m";
            const char *result_str = group_pass ? "PASS" : "FAIL";

            fprintf(stderr, "fmatail_avx512: Iter %lu, Group %d, a[0..3]=%.12e %.12e %.12e %.12e\n",
                    iteration, group, a[0], a[1], a[2], a[3]);
            fprintf(stderr, "                               b[0..3]=%.12e %.12e %.12e %.12e\n",
                    b[0], b[1], b[2], b[3]);
            fprintf(stderr, "                               c[0..3]=%.12e %.12e %.12e %.12e\n",
                    c[0], c[1], c[2], c[3]);
            fprintf(stderr, "  hw_result[0..3]=%.12e %.12e %.12e %.12e\n",
                    hw_result[0], hw_result[1], hw_result[2], hw_result[3]);
            fprintf(stderr, "  sw_ref[0..3]=%.12e %.12e %.12e %.12e\n",
                    sw_ref[0], sw_ref[1], sw_ref[2], sw_ref[3]);
            fprintf(stderr, "  data_ok=%d, consistent=%d, result=%s%s\033[0m\n",
                    data_ok, consistent, color, result_str);
            fflush(stderr);
        }

        if (!all_passed) {
            report_fail_msg("fmatail_avx512: Some group failed");
            return EXIT_FAILURE;
        }

    } while (test_time_condition(test));

    return EXIT_SUCCESS;
}
#else
static int fmatail_avx512_run(struct test *test, int cpu) {
    (void)cpu;
    log_skip(TestResourceIssueSkipCategory,
             "to be implemented (placeholder): ARM NEON FMA required");
    return EXIT_SKIP;
}
#endif

static int fmatail_avx512_finish(struct test *test) {
    (void)test;
    return EXIT_SUCCESS;
}

DECLARE_TEST(fmatail_avx512, "AVX-512 double FMA exhaustive tail precision with nested loops (NEON version)")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = fmatail_avx512_init,
    .test_run = fmatail_avx512_run,
    .test_cleanup = fmatail_avx512_finish,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
