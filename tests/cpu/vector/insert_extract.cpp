#include <sandstone.h>
#include <cstdio>
#include <cstdint>
#include <random>
#include <atomic>
#include <cstring>
#include <ctime>

#ifdef __aarch64__
#include <arm_neon.h>

static constexpr int LANES = 4;

struct test_data {
    std::mt19937 rng;
    float mem_buf[LANES];
};

// 初始化
static int insert_extract_init(struct test *test) {
    auto *data = new test_data;
    data->rng.seed(static_cast<unsigned>(time(nullptr)) + getpid());
    test->data = data;
    return EXIT_SUCCESS;
}

// 运行测试
static int insert_extract_run(struct test *test, int cpu) {
    (void)cpu;
    auto *data = static_cast<test_data*>(test->data);
    std::mt19937 &rng = data->rng;
    std::uniform_real_distribution<float> dist(-100.0f, 100.0f);

    static std::atomic<uint64_t> iter{0};

    do {
        // 1. 生成原始向量
        float32x4_t orig_vec = vdupq_n_f32(0.0f);
        float orig_vals[LANES];
        for (int i = 0; i < LANES; ++i) {
            orig_vals[i] = dist(rng);
            orig_vec = vsetq_lane_f32(orig_vals[i], orig_vec, i);
        }

        // 2. 随机选择 lane 和插入值
        int lane = static_cast<int>(dist(rng) * LANES);  // 0..3
        float insert_val = dist(rng);

        // 3. 执行插入和提取，使用 switch 确保 lane 为编译时常量
        float32x4_t new_vec;
        float extracted;
        switch (lane) {
            case 0:
                new_vec = vsetq_lane_f32(insert_val, orig_vec, 0);
                extracted = vgetq_lane_f32(new_vec, 0);
                break;
            case 1:
                new_vec = vsetq_lane_f32(insert_val, orig_vec, 1);
                extracted = vgetq_lane_f32(new_vec, 1);
                break;
            case 2:
                new_vec = vsetq_lane_f32(insert_val, orig_vec, 2);
                extracted = vgetq_lane_f32(new_vec, 2);
                break;
            case 3:
            default:
                new_vec = vsetq_lane_f32(insert_val, orig_vec, 3);
                extracted = vgetq_lane_f32(new_vec, 3);
                break;
        }

        // 4. 验证插入/提取一致性
        bool insert_extract_ok = (extracted == insert_val);

        // 5. Store/Load 一致性测试
        vst1q_f32(data->mem_buf, orig_vec);
        float32x4_t loaded_vec = vld1q_f32(data->mem_buf);
        uint32x4_t cmp = vceqq_f32(orig_vec, loaded_vec);
        bool store_load_ok = (vgetq_lane_u32(cmp, 0) &&
                              vgetq_lane_u32(cmp, 1) &&
                              vgetq_lane_u32(cmp, 2) &&
                              vgetq_lane_u32(cmp, 3));

        bool overall_ok = insert_extract_ok && store_load_ok;
        uint64_t iteration = iter.fetch_add(1, std::memory_order_relaxed);

        // 6. 提取加载后的元素值用于输出
        float loaded_vals[LANES];
        vst1q_f32(loaded_vals, loaded_vec);

        const char *color = overall_ok ? "\033[32m" : "\033[31m";
        const char *result_str = overall_ok ? "PASS" : "FAIL";

        fprintf(stderr,
                "Iter %lu: orig=[%.2f,%.2f,%.2f,%.2f], lane=%d, insert=%.4f, "
                "extracted=%.4f (insert_extract %s), store/load %s (loaded=[%.2f,%.2f,%.2f,%.2f]) → %s%s\033[0m\n",
                iteration,
                orig_vals[0], orig_vals[1], orig_vals[2], orig_vals[3],
                lane, insert_val, extracted,
                insert_extract_ok ? "PASS" : "FAIL",
                store_load_ok ? "PASS" : "FAIL",
                loaded_vals[0], loaded_vals[1], loaded_vals[2], loaded_vals[3],
                color, result_str);

        if (!overall_ok) {
            report_fail_msg("Vector insert/extract or store/load consistency check failed");
            return EXIT_FAILURE;
        }

    } while (test_time_condition(test));

    return EXIT_SUCCESS;
}

// 清理
static int insert_extract_finish(struct test *test) {
    auto *data = static_cast<test_data*>(test->data);
    delete data;
    test->data = nullptr;
    return EXIT_SUCCESS;
}

#else // !__aarch64__

// On non-AArch64 (e.g. x86-64) there is no <arm_neon.h>; the test is kept
// listed and schedulable but reports a clean resource skip so the result is
// marked ignored rather than spuriously passing. The NEON port is the
// counterpart of the x86 AVX-512 k-register / swizzle tests.
static int insert_extract_init(struct test *test) {
    (void)test;
    log_skip(TestResourceIssueSkipCategory,
             "to be implemented (placeholder): ARM NEON required for "
             "insert/extract + store/load consistency");
    return EXIT_SKIP;
}

static int insert_extract_run(struct test *test, int cpu) {
    (void)test;
    (void)cpu;
    __builtin_unreachable();    // init reports EXIT_SKIP, run never executes
}

static int insert_extract_finish(struct test *test) {
    (void)test;
    return EXIT_SUCCESS;
}

#endif // __aarch64__

// 注册测试（注意逗号分隔）
DECLARE_TEST(insert_extract, "NEON insert/extract + memory store/load consistency")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = insert_extract_init,
    .test_run = insert_extract_run,
    .test_cleanup = insert_extract_finish,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
