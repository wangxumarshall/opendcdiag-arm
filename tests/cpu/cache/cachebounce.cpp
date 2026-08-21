#include <sandstone.h>
#include <atomic>
#include <cstdint>
#include <new>

static constexpr size_t CACHE_LINE_SIZE = 64;

struct alignas(CACHE_LINE_SIZE) SharedData {
    std::atomic<uint64_t> counter;      // 实际累加结果
    std::atomic<uint64_t> total_adds;   // 总尝试递增次数
};

static int cachebounce_init(struct test *test) {
    auto *data = new (std::align_val_t{CACHE_LINE_SIZE}) SharedData();
    if (!data) return EXIT_FAILURE;
    data->counter = 0;
    data->total_adds = 0;
    test->data = data;
    return EXIT_SUCCESS;
}

static int cachebounce_run(struct test *test, int cpu) {
    auto *data = static_cast<SharedData*>(test->data);
    do {
        data->counter.fetch_add(1, std::memory_order_relaxed);
        data->total_adds.fetch_add(1, std::memory_order_relaxed);
    } while (test_time_condition(test));
    return EXIT_SUCCESS;
}

static int cachebounce_finish(struct test *test) {
    auto *data = static_cast<SharedData*>(test->data);
    if (!data) return EXIT_SUCCESS;

    uint64_t c = data->counter.load();
    uint64_t t = data->total_adds.load();

    if (c != t) {
        report_fail_msg("cachebounce: counter (%lu) != total_adds (%lu)", c, t);
        return EXIT_FAILURE;
    }
    // 测试通过，框架会自动记录 pass
    return EXIT_SUCCESS;
}

// 使用 group_math（框架已有）
DECLARE_TEST(cachebounce, "Cache line bouncing test for cache coherency")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = cachebounce_init,
    .test_run = cachebounce_run,
    .test_cleanup = cachebounce_finish,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
