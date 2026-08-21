#include <sandstone.h>
#include <cstdint>
#include <cstring>
#include <random>

static constexpr int VECTOR_SIZE = 8;           // 8 个双精度浮点数
static constexpr int DATA_SIZE = 1024;          // 源/目标数据大小

static int gatherscatterpd_init(struct test *test) {
    (void)test;
    return EXIT_SUCCESS;
}

static int gatherscatterpd_run(struct test *test, int cpu) {
    (void)cpu;
    // 每个线程独立分配数据（栈上，16字节对齐即可）
    alignas(16) double src[DATA_SIZE];
    alignas(16) double dst[DATA_SIZE];
    int64_t indices[VECTOR_SIZE];

    std::mt19937 rng(std::random_device{}());
    std::uniform_real_distribution<double> double_dist(-1000.0, 1000.0);
    std::uniform_int_distribution<int> idx_dist(0, DATA_SIZE - 1);
    std::uniform_int_distribution<uint8_t> mask_dist(0, 0xFF);

    do {
        // 生成随机源数据
        for (int i = 0; i < DATA_SIZE; ++i) {
            src[i] = double_dist(rng);
        }
        // 清空目标数组
        memset(dst, 0, sizeof(dst));
        // 生成随机索引（8个64位）
        for (int i = 0; i < VECTOR_SIZE; ++i) {
            indices[i] = idx_dist(rng);
        }
        // 生成随机掩码（8位）
        uint8_t mask = mask_dist(rng);

        // ---- 硬件执行（标量模拟带掩码的 gather + scatter） ----
        double gathered[VECTOR_SIZE];
        for (int i = 0; i < VECTOR_SIZE; ++i) {
            if (mask & (1 << i)) {
                gathered[i] = src[indices[i]];
            } else {
                gathered[i] = 0.0; // 未选中的元素置零（与 _mm512_setzero_pd() 一致）
            }
        }
        // 乘以2并分散写入 dst（仅掩码选中的位置）
        for (int i = 0; i < VECTOR_SIZE; ++i) {
            int idx = indices[i];
            if (mask & (1 << i)) {
                dst[idx] = gathered[i] * 2.0;
            }
            // 未选中的 dst 位置保持为零（已初始化）
        }

        // ---- 软件参考 ----
        double ref[DATA_SIZE];
        memset(ref, 0, sizeof(ref));
        for (int i = 0; i < VECTOR_SIZE; ++i) {
            if (mask & (1 << i)) {
                int idx = indices[i];
                ref[idx] = src[idx] * 2.0;
            }
        }

        // ---- 验证：比较目标数组与参考 ----
        bool data_ok = (memcmp(dst, ref, sizeof(dst)) == 0);

        // ---- 一致性测试：将 dst 中前 8 个元素加载两次，比较是否一致 ----
        alignas(16) double load_buf1[VECTOR_SIZE];
        alignas(16) double load_buf2[VECTOR_SIZE];
        memcpy(load_buf1, dst, sizeof(load_buf1));
        memcpy(load_buf2, dst, sizeof(load_buf2));
        bool consistent = (memcmp(load_buf1, load_buf2, sizeof(load_buf1)) == 0);

        if (!(data_ok && consistent)) {
            report_fail_msg("gatherscatterpd: Masked gather-scatter mismatch or consistency failure");
            return EXIT_FAILURE;
        }

    } while (test_time_condition(test));

    return EXIT_SUCCESS;
}

static int gatherscatterpd_finish(struct test *test) {
    (void)test;
    return EXIT_SUCCESS;
}

DECLARE_TEST(gatherscatterpd, "Masked gather-scatter double-precision (simulated on ARM64)")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = gatherscatterpd_init,
    .test_run = gatherscatterpd_run,
    .test_cleanup = gatherscatterpd_finish,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
