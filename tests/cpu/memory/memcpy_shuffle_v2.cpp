#include <sandstone.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

static constexpr size_t BLOCK_SIZE = 4096;  // 4KB，包含多个缓存行
static constexpr size_t CACHE_LINE = 64;

static int memcpy_shuffle_v2_init(struct test *test) {
    (void)test;
    return EXIT_SUCCESS;
}

static int memcpy_shuffle_v2_run(struct test *test, int cpu) {
    (void)cpu;
    std::vector<uint8_t> src(BLOCK_SIZE);
    std::vector<uint8_t> dst(BLOCK_SIZE);
    std::vector<uint8_t> store_buf(BLOCK_SIZE);
    std::mt19937 rng(std::random_device{}());
    std::uniform_int_distribution<uint8_t> byte_dist(0, 255);
    std::uniform_int_distribution<size_t> offset_dist(0, BLOCK_SIZE - CACHE_LINE);

    do {
        for (size_t i = 0; i < BLOCK_SIZE; ++i) {
            src[i] = byte_dist(rng);
        }
        for (size_t i = 0; i < BLOCK_SIZE; ++i) {
            dst[i] = byte_dist(rng);
        }

        size_t src_off = offset_dist(rng);
        size_t dst_off = offset_dist(rng);

        // ARM64 兼容的缓存行冲刷（使用 __builtin___clear_cache）
        __builtin___clear_cache((char*)src.data() + src_off,
                                (char*)src.data() + src_off + CACHE_LINE);
        __builtin___clear_cache((char*)dst.data() + dst_off,
                                (char*)dst.data() + dst_off + CACHE_LINE);
        // 内存屏障，确保冲刷完成
        __sync_synchronize();

        memcpy(dst.data(), src.data(), BLOCK_SIZE);

        bool data_ok = (memcmp(dst.data(), src.data(), BLOCK_SIZE) == 0);

        memcpy(store_buf.data(), dst.data(), BLOCK_SIZE);
        bool consistent = (memcmp(store_buf.data(), dst.data(), BLOCK_SIZE) == 0);

        if (!(data_ok && consistent)) {
            report_fail_msg("memcpy_shuffle_v2: data mismatch or consistency failure");
            return EXIT_FAILURE;
        }

    } while (test_time_condition(test));

    return EXIT_SUCCESS;
}

static int memcpy_shuffle_v2_finish(struct test *test) {
    (void)test;
    return EXIT_SUCCESS;
}

DECLARE_TEST(memcpy_shuffle_v2, "Memory copy with cache flush interference (using __clear_cache)")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = memcpy_shuffle_v2_init,
    .test_run = memcpy_shuffle_v2_run,
    .test_cleanup = memcpy_shuffle_v2_finish,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
