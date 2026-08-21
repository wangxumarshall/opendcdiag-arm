#include <sandstone.h>
#include <cstdint>
#include <cstring>
#include <random>

static constexpr int VECTOR_SIZE = 16;          // 16 个单精度浮点数
static constexpr int DATA_SIZE = 1024;          // 源/目标数据大小

static int gatherscatter_f32_init(struct test *test) {
    (void)test;
    return EXIT_SUCCESS;
}

static int gatherscatter_f32_run(struct test *test, int cpu) {
    (void)cpu;
    // 每个线程独立分配数据（栈上，16字节对齐即可）
    alignas(16) float src[DATA_SIZE];
    alignas(16) float dst[DATA_SIZE];
    int indices[VECTOR_SIZE];

    std::mt19937 rng(std::random_device{}());
    std::uniform_real_distribution<float> float_dist(-1000.0f, 1000.0f);
    std::uniform_int_distribution<int> idx_dist(0, DATA_SIZE - 1);

    do {
        // 生成随机源数据
        for (int i = 0; i < DATA_SIZE; ++i) {
            src[i] = float_dist(rng);
        }
        // 清空目标数组（用于分散写回）
        memset(dst, 0, sizeof(dst));
        // 生成随机索引（16个）
        for (int i = 0; i < VECTOR_SIZE; ++i) {
            indices[i] = idx_dist(rng);
        }

        // ---- 硬件执行（标量模拟 gather + scatter） ----
        float gathered[VECTOR_SIZE];
        for (int i = 0; i < VECTOR_SIZE; ++i) {
            gathered[i] = src[indices[i]];
        }
        // 乘以2并分散写入 dst
        for (int i = 0; i < VECTOR_SIZE; ++i) {
            int idx = indices[i];
            dst[idx] = gathered[i] * 2.0f;
        }

        // ---- 软件参考 ----
        float ref[DATA_SIZE];
        memset(ref, 0, sizeof(ref));
        for (int i = 0; i < VECTOR_SIZE; ++i) {
            int idx = indices[i];
            ref[idx] = src[idx] * 2.0f;
        }

        // ---- 验证：比较目标数组与参考 ----
        bool data_ok = (memcmp(dst, ref, sizeof(dst)) == 0);

        // ---- 一致性测试：将 dst 中前 16 个元素加载两次，比较是否一致 ----
        alignas(16) float load_buf1[VECTOR_SIZE];
        alignas(16) float load_buf2[VECTOR_SIZE];
        memcpy(load_buf1, dst, sizeof(load_buf1));
        memcpy(load_buf2, dst, sizeof(load_buf2));
        bool consistent = (memcmp(load_buf1, load_buf2, sizeof(load_buf1)) == 0);

        if (!(data_ok && consistent)) {
            report_fail_msg("gatherscatter_f32: Gather-scatter mismatch or consistency failure");
            return EXIT_FAILURE;
        }

    } while (test_time_condition(test));

    return EXIT_SUCCESS;
}

static int gatherscatter_f32_finish(struct test *test) {
    (void)test;
    return EXIT_SUCCESS;
}

DECLARE_TEST(gatherscatter_f32, "Gather-scatter single-precision (simulated on ARM64)")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = gatherscatter_f32_init,
    .test_run = gatherscatter_f32_run,
    .test_cleanup = gatherscatter_f32_finish,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
