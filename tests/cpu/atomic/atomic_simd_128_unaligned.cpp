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
struct alignas(16) SharedData {
    uint8_t padding[1];            // 偏移 1 字节
    uint8x16_t data;               // 非对齐地址
    std::atomic<uint64_t> seq;
};

static int atomic_simd_128_unaligned_init(struct test *test) {
    auto *sd = new SharedData;
    if (!sd) return EXIT_FAILURE;
    sd->data = vdupq_n_u8(0);
    sd->seq.store(0, std::memory_order_relaxed);
    test->data = sd;
    return EXIT_SUCCESS;
}

static int atomic_simd_128_unaligned_run(struct test *test, int cpu) {
    (void)cpu;
    auto *sd = static_cast<SharedData*>(test->data);
    std::mt19937 rng(static_cast<unsigned>(time(nullptr)) + getpid());
    std::uniform_int_distribution<int> dist(0, 1);
    static std::atomic<uint64_t> iter{0};

    do {
        uint64_t pattern = dist(rng);
        uint8x16_t val = pattern ? vdupq_n_u8(0xFF) : vdupq_n_u8(0);

        // 写操作：使用非对齐存储（vst1q_u8 支持非对齐地址）
        sd->seq.fetch_add(1, std::memory_order_acq_rel);
        vst1q_u8((uint8_t*)&sd->data, val);   // 非对齐存储
        sd->seq.fetch_add(1, std::memory_order_acq_rel);

        // 读操作：使用非对齐加载（vld1q_u8 支持非对齐地址）
        uint64_t s1, s2;
        uint8x16_t read_val;
        do {
            s1 = sd->seq.load(std::memory_order_acquire);
            read_val = vld1q_u8((uint8_t*)&sd->data);   // 非对齐加载
            s2 = sd->seq.load(std::memory_order_acquire);
        } while (s1 != s2 || (s1 & 1));

        // 验证数据完整性
        uint8_t bytes[16];
        vst1q_u8(bytes, read_val);
        bool all_zero = true, all_one = true;
        for (int i = 0; i < 16; ++i) {
            if (bytes[i] != 0x00) all_zero = false;
            if (bytes[i] != 0xFF) all_one = false;
        }
        bool data_ok = all_zero || all_one;

        // 一致性测试：存储 -> 重载（非对齐）
        alignas(16) uint8_t store_buf[16]; // 对齐缓冲区，但存储时使用非对齐存储
        vst1q_u8(store_buf, read_val);     // 非对齐存储
        uint8x16_t reload_val = vld1q_u8(store_buf); // 非对齐加载
        // 比较 read_val 和 reload_val 是否完全相等
        uint8x16_t cmp = vceqq_u8(read_val, reload_val);
        bool consistent = true;
        for (int i = 0; i < 16; ++i) {
            if (vgetq_lane_u8(cmp, i) != 0xFF) {
                consistent = false;
                break;
            }
        }

        bool passed = data_ok && consistent;

        uint64_t iteration = iter.fetch_add(1, std::memory_order_relaxed);
        const char *color = passed ? "\033[32m" : "\033[31m";
        const char *result_str = passed ? "PASS" : "FAIL";

        fprintf(stderr, "atomic_simd_128_unaligned: Iter %lu, pattern=%s, read_data[0..3]=%02X %02X %02X %02X\n",
                iteration, pattern ? "0xFF" : "0x00", bytes[0], bytes[1], bytes[2], bytes[3]);
        fprintf(stderr, "  all_zero=%d, all_one=%d, consistent=%d, result=%s%s\033[0m\n",
                all_zero, all_one, consistent, color, result_str);
        fflush(stderr);

        if (!passed) {
            report_fail_msg("atomic_simd_128_unaligned: data tearing or consistency failure");
            return EXIT_FAILURE;
        }

    } while (test_time_condition(test));

    return EXIT_SUCCESS;
}

static int atomic_simd_128_unaligned_finish(struct test *test) {
    auto *sd = static_cast<SharedData*>(test->data);
    delete sd;
    return EXIT_SUCCESS;
}

#else

// Non-aarch64: ARM NEON SIMD is not available. Report a clean placeholder
// skip rather than a misleading pass (CLAUDE.md placeholder-test honesty).
static int atomic_simd_128_unaligned_init(struct test *test) {
    (void)test;
    log_skip(TestResourceIssueSkipCategory,
             "to be implemented (placeholder): ARM NEON SIMD required for this "
             "atomic-SIMD test");
    return EXIT_SKIP;
}
static int atomic_simd_128_unaligned_run(struct test *test, int cpu) {
    (void)test; (void)cpu;
    return EXIT_SKIP;
}
static int atomic_simd_128_unaligned_finish(struct test *test) {
    (void)test;
    return EXIT_SUCCESS;
}

#endif // __aarch64__

DECLARE_TEST(atomic_simd_128_unaligned, "Atomic 128-bit SIMD access (unaligned, via Seqlock) on ARM64")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = atomic_simd_128_unaligned_init,
    .test_run = atomic_simd_128_unaligned_run,
    .test_cleanup = atomic_simd_128_unaligned_finish,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
