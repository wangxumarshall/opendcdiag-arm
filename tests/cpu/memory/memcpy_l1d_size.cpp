#include <sandstone.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <atomic>
#include <random>
#include <vector>
#include <unistd.h>

static size_t get_l1d_size() {
    long size = sysconf(_SC_LEVEL1_DCACHE_SIZE);
    if (size > 0) return static_cast<size_t>(size);
    return 32 * 1024;  // 默认 32KB
}

static const size_t BLOCK_SIZE = get_l1d_size();

static int memcpy_l1d_size_init(struct test *test) {
    (void)test;
    return EXIT_SUCCESS;
}

static int memcpy_l1d_size_run(struct test *test, int cpu) {
    (void)cpu;
    std::vector<uint8_t> src(BLOCK_SIZE);
    std::vector<uint8_t> dst(BLOCK_SIZE);
    std::mt19937 rng(std::random_device{}());
    std::uniform_int_distribution<uint8_t> dist(0, 255);
    static std::atomic<uint64_t> iter{0};

    do {
        for (size_t i = 0; i < BLOCK_SIZE; ++i) {
            src[i] = dist(rng);
        }
        memcpy(dst.data(), src.data(), BLOCK_SIZE);
        bool data_ok = (memcmp(dst.data(), src.data(), BLOCK_SIZE) == 0);

        std::vector<uint8_t> store_buf(BLOCK_SIZE);
        memcpy(store_buf.data(), dst.data(), BLOCK_SIZE);
        bool consistent = (memcmp(store_buf.data(), dst.data(), BLOCK_SIZE) == 0);

        bool passed = data_ok && consistent;
        uint64_t iteration = iter.fetch_add(1, std::memory_order_relaxed);
        const char *color = passed ? "\033[32m" : "\033[31m";
        const char *result_str = passed ? "PASS" : "FAIL";

        fprintf(stderr, "memcpy_l1d_size: Iter %lu, src[0..7]=%02X %02X %02X %02X %02X %02X %02X %02X\n",
                iteration,
                src[0], src[1], src[2], src[3],
                src[4], src[5], src[6], src[7]);
        fprintf(stderr, "  data_ok=%d, consistent=%d, result=%s%s\033[0m\n",
                data_ok, consistent, color, result_str);
        fflush(stderr);

        if (!passed) {
            report_fail_msg("memcpy_l1d_size: data mismatch or consistency failure");
            return EXIT_FAILURE;
        }
    } while (test_time_condition(test));

    return EXIT_SUCCESS;
}

static int memcpy_l1d_size_finish(struct test *test) {
    (void)test;
    return EXIT_SUCCESS;
}

DECLARE_TEST(memcpy_l1d_size, "Memory copy with L1D-sized block (random data)")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = memcpy_l1d_size_init,
    .test_run = memcpy_l1d_size_run,
    .test_cleanup = memcpy_l1d_size_finish,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
