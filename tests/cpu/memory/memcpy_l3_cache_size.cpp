#include <sandstone.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <atomic>
#include <random>
#include <vector>
#include <unistd.h>

// 获取 L3 缓存大小（字节），若失败则使用默认 8MB
static size_t get_l3_size() {
    long size = sysconf(_SC_LEVEL3_CACHE_SIZE);
    if (size > 0) return static_cast<size_t>(size);
    // 常见 L3 缓存大小：8MB
    return 8 * 1024 * 1024;
}

static const size_t BLOCK_SIZE = get_l3_size();

static int memcpy_l3_cache_size_init(struct test *test) {
    (void)test;
    return EXIT_SUCCESS;
}

static int memcpy_l3_cache_size_run(struct test *test, int cpu) {
    (void)cpu;

    std::vector<uint8_t> src(BLOCK_SIZE);
    std::vector<uint8_t> dst(BLOCK_SIZE);

    // 随机数生成器（每个线程独立）
    std::mt19937 rng(std::random_device{}());
    std::uniform_int_distribution<uint8_t> dist(0, 255);

    static std::atomic<uint64_t> iter{0};

    do {
        // 填充源数据为随机字节（SIMD 风格数据本质上就是随机字节）
        for (size_t i = 0; i < BLOCK_SIZE; ++i) {
            src[i] = dist(rng);
        }

        // 执行内存复制
        memcpy(dst.data(), src.data(), BLOCK_SIZE);

        // 验证数据是否完全一致
        bool data_ok = (memcmp(dst.data(), src.data(), BLOCK_SIZE) == 0);

        // 一致性测试：存储 dst 到临时缓冲区，再重新加载比较
        std::vector<uint8_t> store_buf(BLOCK_SIZE);
        memcpy(store_buf.data(), dst.data(), BLOCK_SIZE);
        bool consistent = (memcmp(store_buf.data(), dst.data(), BLOCK_SIZE) == 0);

        bool passed = data_ok && consistent;

        uint64_t iteration = iter.fetch_add(1, std::memory_order_relaxed);
        const char *color = passed ? "\033[32m" : "\033[31m";
        const char *result_str = passed ? "PASS" : "FAIL";

        // 输出前 8 个字节作为摘要
        fprintf(stderr, "memcpy_l3_cache_size: Iter %lu, src[0..7]=%02X %02X %02X %02X %02X %02X %02X %02X\n",
                iteration,
                src[0], src[1], src[2], src[3],
                src[4], src[5], src[6], src[7]);
        fprintf(stderr, "  data_ok=%d, consistent=%d, result=%s%s\033[0m\n",
                data_ok, consistent, color, result_str);
        fflush(stderr);

        if (!passed) {
            report_fail_msg("memcpy_l3_cache_size: data mismatch or consistency failure");
            return EXIT_FAILURE;
        }

    } while (test_time_condition(test));

    return EXIT_SUCCESS;
}

static int memcpy_l3_cache_size_finish(struct test *test) {
    (void)test;
    return EXIT_SUCCESS;
}

DECLARE_TEST(memcpy_l3_cache_size, "Memory copy with L3-sized block (random data)")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = memcpy_l3_cache_size_init,
    .test_run = memcpy_l3_cache_size_run,
    .test_cleanup = memcpy_l3_cache_size_finish,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
