#include <sandstone.h>
#include <cstdint>
#include <cstring>
#include <random>

static constexpr int VECTOR_SIZE = 16;          // 16 个 int32
static constexpr int DATA_SIZE = 1024;          // 源/目标数据大小

static int gatherscatter_i32_init(struct test *test) {
    (void)test;
    return EXIT_SUCCESS;
}

static int gatherscatter_i32_run(struct test *test, int cpu) {
    (void)cpu;
    // 每个线程独立分配数据（栈上，16字节对齐即可）
    alignas(16) int32_t src[DATA_SIZE];
    alignas(16) int32_t dst[DATA_SIZE];
    int32_t indices[VECTOR_SIZE];

    std::mt19937 rng(std::random_device{}());
    std::uniform_int_distribution<int32_t> int_dist(-10000, 10000);
    std::uniform_int_distribution<int> idx_dist(0, DATA_SIZE - 1);

    do {
        // 生成随机源数据
        for (int i = 0; i < DATA_SIZE; ++i) {
            src[i] = int_dist(rng);
        }
        // 清空目标数组
        memset(dst, 0, sizeof(dst));
        // 生成随机索引（16个）
        for (int i = 0; i < VECTOR_SIZE; ++i) {
            indices[i] = idx_dist(rng);
        }

        // ---- 硬件执行（标量模拟 gather + scatter） ----
        int32_t gathered[VECTOR_SIZE];
        for (int i = 0; i < VECTOR_SIZE; ++i) {
            gathered[i] = src[indices[i]];
        }
        // 乘以2并分散写入 dst
        for (int i = 0; i < VECTOR_SIZE; ++i) {
            int idx = indices[i];
            dst[idx] = gathered[i] * 2;
        }

        // ---- 软件参考 ----
        int32_t ref[DATA_SIZE];
        memset(ref, 0, sizeof(ref));
        for (int i = 0; i < VECTOR_SIZE; ++i) {
            int idx = indices[i];
            ref[idx] = src[idx] * 2;
        }

        // ---- 验证：比较目标数组与参考 ----
        bool data_ok = (memcmp(dst, ref, sizeof(dst)) == 0);

        // ---- 一致性测试：将 dst 中前 16 个元素加载两次，比较是否一致 ----
        alignas(16) int32_t load_buf1[VECTOR_SIZE];
        alignas(16) int32_t load_buf2[VECTOR_SIZE];
        memcpy(load_buf1, dst, sizeof(load_buf1));
        memcpy(load_buf2, dst, sizeof(load_buf2));
        bool consistent = (memcmp(load_buf1, load_buf2, sizeof(load_buf1)) == 0);

        if (!(data_ok && consistent)) {
            report_fail_msg("gatherscatter_i32: Gather-scatter mismatch or consistency failure");
            return EXIT_FAILURE;
        }

    } while (test_time_condition(test));

    return EXIT_SUCCESS;
}

static int gatherscatter_i32_finish(struct test *test) {
    (void)test;
    return EXIT_SUCCESS;
}

DECLARE_TEST(gatherscatter_i32, "Gather-scatter 32-bit integers (simulated on ARM64)")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = gatherscatter_i32_init,
    .test_run = gatherscatter_i32_run,
    .test_cleanup = gatherscatter_i32_finish,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
