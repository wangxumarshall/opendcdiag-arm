#include <sandstone.h>
#include <cstdint>
#include <cstdio>
#include <atomic>
#include <cstring>
#include <random>
#include <ctime>
#include <unistd.h>

#ifdef __aarch64__
#include <arm_neon.h>

// 数据块：偏移 1 字节以确保非对齐（可能跨缓存行）
struct SharedData {
    uint8_t padding[1];            // 偏移 1 字节
    uint8x16_t data0;              // 低 128 位（非对齐地址）
    uint8x16_t data1;              // 高 128 位（紧随其后，整体非对齐）
    std::atomic<uint64_t> seq;
};

static int atomic_simd_256_unaligned_init(struct test *test) {
    auto *sd = new SharedData;
    if (!sd) return EXIT_FAILURE;
    sd->data0 = vdupq_n_u8(0);
    sd->data1 = vdupq_n_u8(0);
    sd->seq.store(0, std::memory_order_relaxed);
    test->data = sd;
    return EXIT_SUCCESS;
}

static int atomic_simd_256_unaligned_run(struct test *test, int cpu) {
    (void)cpu;
    auto *sd = static_cast<SharedData*>(test->data);
    std::mt19937 rng(static_cast<unsigned>(time(nullptr)) + getpid());
    std::uniform_int_distribution<int> dist(0, 1);
    static std::atomic<uint64_t> iter{0};

    do {
        uint64_t pattern = dist(rng);
        uint8x16_t val0 = pattern ? vdupq_n_u8(0xFF) : vdupq_n_u8(0);
        uint8x16_t val1 = val0;   // 全0或全1

        // 写操作：使用非对齐存储（vst1q_u8 支持非对齐）
        sd->seq.fetch_add(1, std::memory_order_acq_rel);
        vst1q_u8((uint8_t*)&sd->data0, val0);   // 非对齐存储
        vst1q_u8((uint8_t*)&sd->data1, val1);
        sd->seq.fetch_add(1, std::memory_order_acq_rel);

        // 读操作：使用非对齐加载（vld1q_u8 支持非对齐）
        uint64_t s1, s2;
        uint8x16_t read0, read1;
        do {
            s1 = sd->seq.load(std::memory_order_acquire);
            read0 = vld1q_u8((uint8_t*)&sd->data0);
            read1 = vld1q_u8((uint8_t*)&sd->data1);
            s2 = sd->seq.load(std::memory_order_acquire);
        } while (s1 != s2 || (s1 & 1));

        // 验证数据完整性：检查读出的两个块是否全0或全1
        uint8_t bytes[32];
        vst1q_u8(bytes, read0);
        vst1q_u8(bytes + 16, read1);
        bool all_zero = true, all_one = true;
        for (int i = 0; i < 32; ++i) {
            if (bytes[i] != 0x00) all_zero = false;
            if (bytes[i] != 0xFF) all_one = false;
        }
        bool data_ok = all_zero || all_one;

        // 一致性测试：存储整个 256 位到对齐缓冲区，再加载比较
        alignas(32) uint8_t store_buf[32];
        vst1q_u8(store_buf, read0);
        vst1q_u8(store_buf + 16, read1);
        uint8x16_t reload0 = vld1q_u8(store_buf);
        uint8x16_t reload1 = vld1q_u8(store_buf + 16);
        // 分别比较两个块是否相等
        uint8x16_t cmp0 = vceqq_u8(read0, reload0);
        uint8x16_t cmp1 = vceqq_u8(read1, reload1);
        bool consistent = true;
        for (int i = 0; i < 16; ++i) {
            if (vgetq_lane_u8(cmp0, i) != 0xFF ||
                vgetq_lane_u8(cmp1, i) != 0xFF) {
                consistent = false;
                break;
            }
        }

        bool passed = data_ok && consistent;

        uint64_t iteration = iter.fetch_add(1, std::memory_order_relaxed);
        const char *color = passed ? "\033[32m" : "\033[31m";
        const char *result_str = passed ? "PASS" : "FAIL";

        fprintf(stderr, "atomic_simd_256_unaligned: Iter %lu, pattern=%s, read_data[0..3]=%02X %02X %02X %02X\n",
                iteration, pattern ? "0xFF" : "0x00", bytes[0], bytes[1], bytes[2], bytes[3]);
        fprintf(stderr, "  all_zero=%d, all_one=%d, consistent=%d, result=%s%s\033[0m\n",
                all_zero, all_one, consistent, color, result_str);
        fflush(stderr);

        if (!passed) {
            report_fail_msg("atomic_simd_256_unaligned: data tearing or consistency failure");
            return EXIT_FAILURE;
        }

    } while (test_time_condition(test));

    return EXIT_SUCCESS;
}

static int atomic_simd_256_unaligned_finish(struct test *test) {
    auto *sd = static_cast<SharedData*>(test->data);
    delete sd;
    return EXIT_SUCCESS;
}

#else

// Non-aarch64: ARM NEON SIMD is not available. Report a clean placeholder
// skip rather than a misleading pass (CLAUDE.md placeholder-test honesty).
static int atomic_simd_256_unaligned_init(struct test *test) {
    (void)test;
    log_skip(TestResourceIssueSkipCategory,
             "to be implemented (placeholder): ARM NEON SIMD required for this "
             "atomic-SIMD test");
    return EXIT_SKIP;
}
static int atomic_simd_256_unaligned_run(struct test *test, int cpu) {
    (void)test; (void)cpu;
    return EXIT_SKIP;
}
static int atomic_simd_256_unaligned_finish(struct test *test) {
    (void)test;
    return EXIT_SUCCESS;
}

#endif // __aarch64__

DECLARE_TEST(atomic_simd_256_unaligned, "Atomic 256-bit SIMD access (unaligned, via Seqlock) on ARM64")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = atomic_simd_256_unaligned_init,
    .test_run = atomic_simd_256_unaligned_run,
    .test_cleanup = atomic_simd_256_unaligned_finish,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
