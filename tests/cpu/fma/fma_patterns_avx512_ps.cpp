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

static constexpr int VECTOR_SIZE = 16; // 16 个单精度浮点数

static int fma_patterns_avx512_ps_init(struct test *test) {
    (void)test;
    return EXIT_SUCCESS;
}

#ifdef __aarch64__
static int fma_patterns_avx512_ps_run(struct test *test, int cpu) {
    (void)cpu;
    std::mt19937 rng(std::random_device{}());
    std::uniform_real_distribution<float> dist(-1e6f, 1e6f);
    static std::atomic<uint64_t> iter{0};

    do {
        // 生成随机向量 a, b, c（对齐到 16 字节即可满足 NEON）
        alignas(16) float a[VECTOR_SIZE];
        alignas(16) float b[VECTOR_SIZE];
        alignas(16) float c[VECTOR_SIZE];
        alignas(16) float hw_result[VECTOR_SIZE];
        float sw_ref[VECTOR_SIZE];

        for (int i = 0; i < VECTOR_SIZE; ++i) {
            a[i] = dist(rng);
            b[i] = dist(rng);
            c[i] = dist(rng);
        }

        // ---- 硬件 FMA 计算 (NEON) ----
        // 使用 4 个 float32x4_t 向量容纳 16 个元素
        float32x4_t va0 = vld1q_f32(a);
        float32x4_t va1 = vld1q_f32(a + 4);
        float32x4_t va2 = vld1q_f32(a + 8);
        float32x4_t va3 = vld1q_f32(a + 12);
        float32x4_t vb0 = vld1q_f32(b);
        float32x4_t vb1 = vld1q_f32(b + 4);
        float32x4_t vb2 = vld1q_f32(b + 8);
        float32x4_t vb3 = vld1q_f32(b + 12);
        float32x4_t vc0 = vld1q_f32(c);
        float32x4_t vc1 = vld1q_f32(c + 4);
        float32x4_t vc2 = vld1q_f32(c + 8);
        float32x4_t vc3 = vld1q_f32(c + 12);

        // vfmaq_f32 计算：dst = dst + src1 * src2  (即 c + a * b)
        float32x4_t vd0 = vfmaq_f32(vc0, va0, vb0);
        float32x4_t vd1 = vfmaq_f32(vc1, va1, vb1);
        float32x4_t vd2 = vfmaq_f32(vc2, va2, vb2);
        float32x4_t vd3 = vfmaq_f32(vc3, va3, vb3);

        // 存储结果
        vst1q_f32(hw_result,      vd0);
        vst1q_f32(hw_result + 4,  vd1);
        vst1q_f32(hw_result + 8,  vd2);
        vst1q_f32(hw_result + 12, vd3);

        // ---- 软件参考：使用标准库 fmaf（正确舍入） ----
        for (int i = 0; i < VECTOR_SIZE; ++i) {
            sw_ref[i] = fmaf(a[i], b[i], c[i]);
        }

        // ---- 比较硬件结果与参考（允许 1e-6 相对误差或 1e-5 绝对误差） ----
        bool data_ok = true;
        for (int i = 0; i < VECTOR_SIZE; ++i) {
            float diff = fabsf(hw_result[i] - sw_ref[i]);
            float tol = 1e-6f * fmaxf(fabsf(hw_result[i]), fabsf(sw_ref[i]));
            if (diff > tol && diff > 1e-5f) {
                data_ok = false;
                break;
            }
        }

        // ---- 一致性测试：存储硬件结果到内存再加载比较 ----
        float store_buf[VECTOR_SIZE];
        memcpy(store_buf, hw_result, sizeof(store_buf));
        float reload_buf[VECTOR_SIZE];
        memcpy(reload_buf, store_buf, sizeof(store_buf));
        bool consistent = (memcmp(reload_buf, hw_result, sizeof(reload_buf)) == 0);

        bool passed = data_ok && consistent;

        uint64_t iteration = iter.fetch_add(1, std::memory_order_relaxed);
        const char *color = passed ? "\033[32m" : "\033[31m";
        const char *result_str = passed ? "PASS" : "FAIL";

        // ---- 输出日志（与 x86 版本完全一致） ----
        fprintf(stderr, "fma_patterns_avx512_ps: Iter %lu, a[0..3]=%.6e %.6e %.6e %.6e\n",
                iteration, a[0], a[1], a[2], a[3]);
        fprintf(stderr, "                    b[0..3]=%.6e %.6e %.6e %.6e\n",
                b[0], b[1], b[2], b[3]);
        fprintf(stderr, "                    c[0..3]=%.6e %.6e %.6e %.6e\n",
                c[0], c[1], c[2], c[3]);
        fprintf(stderr, "  hw_result[0..3]=%.6e %.6e %.6e %.6e\n",
                hw_result[0], hw_result[1], hw_result[2], hw_result[3]);
        fprintf(stderr, "  sw_ref[0..3]=%.6e %.6e %.6e %.6e\n",
                sw_ref[0], sw_ref[1], sw_ref[2], sw_ref[3]);
        fprintf(stderr, "  data_ok=%d, consistent=%d, result=%s%s\033[0m\n",
                data_ok, consistent, color, result_str);
        fflush(stderr);

        if (!passed) {
            report_fail_msg("fma_patterns_avx512_ps: FMA result mismatch or consistency failure");
            return EXIT_FAILURE;
        }

    } while (test_time_condition(test));

    return EXIT_SUCCESS;
}
#else
static int fma_patterns_avx512_ps_run(struct test *test, int cpu) {
    (void)cpu;
    log_skip(TestResourceIssueSkipCategory,
             "to be implemented (placeholder): ARM NEON FMA required");
    return EXIT_SKIP;
}
#endif

static int fma_patterns_avx512_ps_finish(struct test *test) {
    (void)test;
    return EXIT_SUCCESS;
}

DECLARE_TEST(fma_patterns_avx512_ps, "AVX-512 single FMA pattern stress test (NEON version)")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = fma_patterns_avx512_ps_init,
    .test_run = fma_patterns_avx512_ps_run,
    .test_cleanup = fma_patterns_avx512_ps_finish,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
