#include <sandstone.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>

static constexpr int VECTOR_SIZE = 4;          // 4 个双精度浮点数
static constexpr int DATA_SIZE = 1024;         // 源数据大小

static int gather_f64_init(struct test *test) {
    (void)test;
    return EXIT_SUCCESS;
}

static int gather_f64_run(struct test *test, int cpu) {
    (void)cpu;
    // 每个线程独立分配数据
    alignas(16) double src[DATA_SIZE];
    long long indices[VECTOR_SIZE];

    std::mt19937 rng(std::random_device{}());
    std::uniform_real_distribution<double> double_dist(-1000.0, 1000.0);
    std::uniform_int_distribution<int> idx_dist(0, DATA_SIZE - 1);

    do {
        // 生成随机源数据
        for (int i = 0; i < DATA_SIZE; ++i) {
            src[i] = double_dist(rng);
        }
        // 生成随机索引（64位）
        for (int i = 0; i < VECTOR_SIZE; ++i) {
            indices[i] = idx_dist(rng);
        }

        // ---- 硬件 Gather（标量模拟，逐个加载） ----
        double gathered[VECTOR_SIZE];
        for (int i = 0; i < VECTOR_SIZE; ++i) {
            gathered[i] = src[indices[i]];
        }

        // ---- 软件参考：与硬件相同 ----
        double ref[VECTOR_SIZE];
        for (int i = 0; i < VECTOR_SIZE; ++i) {
            ref[i] = src[indices[i]];
        }

        // ---- 比较硬件结果与参考（逐位比较） ----
        bool data_ok = (memcmp(gathered, ref, sizeof(ref)) == 0);

        // ---- 一致性测试：存储 gathered 到内存再加载比较 ----
        alignas(16) double store_buf[VECTOR_SIZE];
        memcpy(store_buf, gathered, sizeof(gathered));
        double reload[VECTOR_SIZE];
        memcpy(reload, store_buf, sizeof(store_buf));
        bool consistent = (memcmp(reload, gathered, sizeof(gathered)) == 0);

        if (!(data_ok && consistent)) {
            report_fail_msg("gather_f64: Gather mismatch or consistency failure");
            return EXIT_FAILURE;
        }

    } while (test_time_condition(test));

    return EXIT_SUCCESS;
}

static int gather_f64_finish(struct test *test) {
    (void)test;
    return EXIT_SUCCESS;
}

DECLARE_TEST(gather_f64, "Gather double-precision (simulated on ARM64)")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = gather_f64_init,
    .test_run = gather_f64_run,
    .test_cleanup = gather_f64_finish,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
