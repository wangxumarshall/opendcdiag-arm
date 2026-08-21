#include <sandstone.h>
#include <cstdint>
#include <cstdio>
#include <random>
#include <cstring>
#include <atomic>

#ifdef __aarch64__
#include <arm_neon.h>          // ARM NEON 头文件

static constexpr size_t NUM_ELEMENTS = 8;

static void fill_random_float(float *arr, size_t n, std::mt19937 &rng) {
    std::uniform_real_distribution<float> dist(-100.0f, 100.0f);
    for (size_t i = 0; i < n; ++i) {
        arr[i] = dist(rng);
    }
}

// 标量参考实现
static void swizzle_reference(const float *src, float *dst, const int *perm) {
    for (int i = 0; i < 8; ++i) {
        dst[i] = src[perm[i]];
    }
}

static int swizzle_init(struct test *test) {
    (void)test;
    return EXIT_SUCCESS;
}

static int swizzle_run(struct test *test, int cpu) {
    (void)cpu;

    std::mt19937 rng(std::random_device{}());
    // 置换模式：交换每对相邻元素 (0↔1, 2↔3, 4↔5, 6↔7)
    //int perm[8] = {0, 2, 1, 3, 4, 6, 5, 7};
    int perm[8] = {1, 0, 3, 2, 5, 4, 7, 6};

    float src[8];
    float dst_ref[8];
    float dst_hw[8];

    static std::atomic<uint64_t> iter{0};  // 全局迭代计数（多线程共享）

    do {
        fill_random_float(src, 8, rng);
        swizzle_reference(src, dst_ref, perm);

        // === ARM NEON 实现：vrev64q_f32 交换每对相邻单精度 ===
        float32x4_t v0 = vld1q_f32(src);       // 加载前4个
        float32x4_t v1 = vld1q_f32(src + 4);   // 加载后4个
        v0 = vrev64q_f32(v0);                  // 交换 0↔1, 2↔3
        v1 = vrev64q_f32(v1);                  // 交换 4↔5, 6↔7
        vst1q_f32(dst_hw, v0);
        vst1q_f32(dst_hw + 4, v1);

        bool passed = (memcmp(dst_hw, dst_ref, sizeof(dst_hw)) == 0);
        uint64_t iteration = iter.fetch_add(1, std::memory_order_relaxed);

        const char *color = passed ? "\033[32m" : "\033[31m";
        const char *result_str = passed ? "PASS" : "FAIL";
        fprintf(stderr, "swizzle: Iteration %lu, input[0..3] = %.2f, %.2f, %.2f, %.2f, result = %s%s\033[0m\n",
                iteration, src[0], src[1], src[2], src[3], color, result_str);

        if (!passed) {
            report_fail_msg("swizzle: data mismatch");
            return EXIT_FAILURE;
        }

    } while (test_time_condition(test));

    return EXIT_SUCCESS;
}

static int swizzle_finish(struct test *test) {
    (void)test;
    return EXIT_SUCCESS;
}

#else // !__aarch64__

// On non-AArch64 (e.g. x86-64) there is no <arm_neon.h>; the test is kept
// listed and schedulable but reports a clean resource skip so the result is
// marked ignored rather than spuriously passing.
static int swizzle_init(struct test *test) {
    (void)test;
    log_skip(TestResourceIssueSkipCategory,
             "to be implemented (placeholder): ARM NEON required for "
             "vector swizzle (permute) operations");
    return EXIT_SKIP;
}

static int swizzle_run(struct test *test, int cpu) {
    (void)test;
    (void)cpu;
    __builtin_unreachable();    // init reports EXIT_SKIP, run never executes
}

static int swizzle_finish(struct test *test) {
    (void)test;
    return EXIT_SUCCESS;
}

#endif // __aarch64__

DECLARE_TEST(swizzle, "Test vector swizzle (permute) operations on ARM NEON")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = swizzle_init,
    .test_run = swizzle_run,
    .test_cleanup = swizzle_finish,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
