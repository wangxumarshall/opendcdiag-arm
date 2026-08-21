#include <sandstone.h>
#include <cstdint>
#include <cstdio>
#include <atomic>
#include <cstring>
#include <random>

struct alignas(16) SharedData {
    std::atomic<__int128> data;        // 原子128位数据
    std::atomic<uint64_t> seq;
};

static int atomic_simd_128_init(struct test *test) {
    auto *sd = new SharedData;
    if (!sd) return EXIT_FAILURE;
    sd->data = 0;
    sd->seq.store(0, std::memory_order_relaxed);
    test->data = sd;
    return EXIT_SUCCESS;
}

static int atomic_simd_128_run(struct test *test, int cpu) {
    (void)cpu;
    auto *sd = static_cast<SharedData*>(test->data);
    std::mt19937 rng(std::random_device{}());
    std::uniform_int_distribution<int> dist(0, 1);
    static std::atomic<uint64_t> iter{0};

    do {
        uint64_t pattern = dist(rng);
        __int128 val = pattern ? ~(__int128)0 : 0;   // 全1或全0

        // 序列锁写入
        sd->seq.fetch_add(1, std::memory_order_acq_rel);
        sd->data.store(val, std::memory_order_release);
        sd->seq.fetch_add(1, std::memory_order_acq_rel);

        // 序列锁读取
        uint64_t s1, s2;
        __int128 read_val;
        do {
            s1 = sd->seq.load(std::memory_order_acquire);
            read_val = sd->data.load(std::memory_order_acquire);
            s2 = sd->seq.load(std::memory_order_acquire);
        } while (s1 != s2 || (s1 & 1));

        // 验证读出的数据是否全0或全1（将128位拆分为两个64位）
        uint64_t low = (uint64_t)read_val;
        uint64_t high = (uint64_t)(read_val >> 64);
        bool all_zero = (low == 0 && high == 0);
        bool all_one = (low == 0xFFFFFFFFFFFFFFFFULL && high == 0xFFFFFFFFFFFFFFFFULL);
        bool data_ok = all_zero || all_one;

        // 一致性测试：将 read_val 存储到缓冲区再加载比较
        __int128 store_buf = read_val;
        __int128 reload_buf;
        memcpy(&reload_buf, &store_buf, sizeof(store_buf));
        bool consistent = (reload_buf == read_val);

        bool passed = data_ok && consistent;

        uint64_t iteration = iter.fetch_add(1, std::memory_order_relaxed);
        const char *color = passed ? "\033[32m" : "\033[31m";
        const char *result_str = passed ? "PASS" : "FAIL";

        // 输出前4个字节用于显示（仅用于保持输出格式一致）
        uint8_t bytes[16];
        memcpy(bytes, &read_val, 16);
        fprintf(stderr, "atomic_simd_128: Iter %lu, pattern=%s, read_data[0..3]=%02X %02X %02X %02X\n",
                iteration, pattern ? "0xFF" : "0x00", bytes[0], bytes[1], bytes[2], bytes[3]);
        fprintf(stderr, "  all_zero=%d, all_one=%d, consistent=%d, result=%s%s\033[0m\n",
                all_zero, all_one, consistent, color, result_str);
        fflush(stderr);

        if (!passed) {
            report_fail_msg("atomic_simd_128: data tearing or consistency failure");
            return EXIT_FAILURE;
        }

    } while (test_time_condition(test));

    return EXIT_SUCCESS;
}

static int atomic_simd_128_finish(struct test *test) {
    auto *sd = static_cast<SharedData*>(test->data);
    delete sd;
    return EXIT_SUCCESS;
}

DECLARE_TEST(atomic_simd_128, "Atomic 128-bit access (aligned, via Seqlock) on ARM64")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = atomic_simd_128_init,
    .test_run = atomic_simd_128_run,
    .test_cleanup = atomic_simd_128_finish,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
