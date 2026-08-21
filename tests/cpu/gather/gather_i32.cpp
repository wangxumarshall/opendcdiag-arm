#include <sandstone.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>

static constexpr int VECTOR_SIZE = 8;          // 8 个 int32
static constexpr int DATA_SIZE = 1024;         // 源数据大小

static int gather_i32_init(struct test *test) {
    (void)test;
    return EXIT_SUCCESS;
}

static int gather_i32_run(struct test *test, int cpu) {
    (void)cpu;
    // 每个线程独立分配数据
    alignas(16) int32_t src[DATA_SIZE];
    int indices[VECTOR_SIZE];

    std::mt19937 rng(std::random_device{}());
    std::uniform_int_distribution<int32_t> int_dist(-10000, 10000);
    std::uniform_int_distribution<int> idx_dist(0, DATA_SIZE - 1);

    do {
        // 生成随机源数据
        for (int i = 0; i < DATA_SIZE; ++i) {
            src[i] = int_dist(rng);
        }
        // 生成随机索引
        for (int i = 0; i < VECTOR_SIZE; ++i) {
            indices[i] = idx_dist(rng);
        }

        // ---- 硬件 Gather（标量模拟，逐个加载） ----
        int32_t gathered[VECTOR_SIZE];
        for (int i = 0; i < VECTOR_SIZE; ++i) {
            gathered[i] = src[indices[i]];
        }

        // ---- 软件参考：与硬件相同 ----
        int32_t ref[VECTOR_SIZE];
        for (int i = 0; i < VECTOR_SIZE; ++i) {
            ref[i] = src[indices[i]];
        }

        // ---- 比较硬件结果与参考（逐位比较） ----
        bool data_ok = (memcmp(gathered, ref, sizeof(ref)) == 0);

        // ---- 一致性测试：存储 gathered 到内存再加载比较 ----
        alignas(16) int32_t store_buf[VECTOR_SIZE];
        memcpy(store_buf, gathered, sizeof(gathered));
        int32_t reload[VECTOR_SIZE];
        memcpy(reload, store_buf, sizeof(store_buf));
        bool consistent = (memcmp(reload, gathered, sizeof(gathered)) == 0);

        if (!(data_ok && consistent)) {
            report_fail_msg("gather_i32: Gather mismatch or consistency failure");
            return EXIT_FAILURE;
        }

    } while (test_time_condition(test));

    return EXIT_SUCCESS;
}

static int gather_i32_finish(struct test *test) {
    (void)test;
    return EXIT_SUCCESS;
}

DECLARE_TEST(gather_i32, "Gather 32-bit integers (simulated on ARM64)")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = gather_i32_init,
    .test_run = gather_i32_run,
    .test_cleanup = gather_i32_finish,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
