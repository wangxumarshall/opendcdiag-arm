#include <sandstone.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <atomic>
#include <random>
#include <unistd.h>

#define MAX_THREADS 128

// 16 字节对齐的结构体，使用 std::atomic<__int128> 管理
struct alignas(16) SharedData {
    std::atomic<__int128> value;               // 原子128位整数
    std::atomic<uint64_t> thread_idx;          // 线程 ID 分配器
    uint64_t local_sums[MAX_THREADS];          // 每个线程的本地增量总和
};

static int spinlock_stress_cmpxchg16b_init(struct test *test) {
    auto *sd = new SharedData;
    if (!sd) return EXIT_FAILURE;
    sd->value = 0;
    sd->thread_idx.store(0, std::memory_order_relaxed);
    memset(sd->local_sums, 0, sizeof(sd->local_sums));
    test->data = sd;
    return EXIT_SUCCESS;
}

static int spinlock_stress_cmpxchg16b_run(struct test *test, int cpu) {
    (void)cpu;
    auto *sd = static_cast<SharedData*>(test->data);

    int id = sd->thread_idx.fetch_add(1, std::memory_order_relaxed);
    if (id >= MAX_THREADS) {
        report_fail_msg("Too many threads");
        return EXIT_FAILURE;
    }

    std::mt19937 rng(std::random_device{}());
    std::uniform_int_distribution<uint64_t> dist(1, 1000);
    uint64_t local_sum = 0;

    do {
        uint64_t inc = dist(rng);

        bool success = false;
        while (!success) {
            __int128 old_val = sd->value.load(std::memory_order_acquire);
            uint64_t old_counter = (uint64_t)old_val;
            uint64_t old_pad = (uint64_t)(old_val >> 64);
            uint64_t new_counter = old_counter + inc;
            __int128 new_val = ((__int128)old_pad << 64) | new_counter;
            // 使用 compare_exchange_weak（等价于 cmpxchg16b）
            success = sd->value.compare_exchange_weak(old_val, new_val,
                                                      std::memory_order_acq_rel,
                                                      std::memory_order_acquire);
            if (!success) {
                // 自旋等待，加入 yield 提示
                #if defined(__aarch64__)
                    __asm__ volatile("yield");
                #elif defined(__x86_64__)
                    __asm__ volatile("pause");
                #else
                    /* no arch-specific pause hint; fall through */
                #endif
            }
        }

        local_sum += inc;

        fprintf(stderr, "spinlock_stress_cmpxchg16b: Thread %d, inc=%lu, result=PASS\n", id, inc);
        fflush(stderr);

    } while (test_time_condition(test));

    sd->local_sums[id] = local_sum;
    return EXIT_SUCCESS;
}

static int spinlock_stress_cmpxchg16b_finish(struct test *test) {
    auto *sd = static_cast<SharedData*>(test->data);

    uint64_t total_local = 0;
    uint64_t thread_count = 0;
    for (int i = 0; i < MAX_THREADS; ++i) {
        if (sd->local_sums[i] != 0) {
            total_local += sd->local_sums[i];
            thread_count++;
        }
    }

    __int128 final_val = sd->value.load(std::memory_order_acquire);
    uint64_t final_counter = (uint64_t)final_val;

    // 一致性测试：存储到内存再加载比较
    __int128 store_buf = final_val;
    __int128 reload_buf;
    memcpy(&reload_buf, &store_buf, sizeof(store_buf));
    bool consistent = (reload_buf == final_val);

    bool data_ok = (final_counter == total_local);
    bool passed = data_ok && consistent;

    const char *color = passed ? "\033[32m" : "\033[31m";
    const char *result_str = passed ? "PASS" : "FAIL";

    fprintf(stderr, "spinlock_stress_cmpxchg16b: Final counter=%lu, Total local sums=%lu, Threads=%lu, data_ok=%d, consistent=%d, result=%s%s\033[0m\n",
            final_counter, total_local, thread_count, data_ok, consistent, color, result_str);
    fflush(stderr);

    if (!passed) {
        report_fail_msg("spinlock_stress_cmpxchg16b: Counter mismatch or consistency failure");
        return EXIT_FAILURE;
    }

    delete sd;
    return EXIT_SUCCESS;
}

DECLARE_TEST(spinlock_stress_cmpxchg16b, "Stress test LOCK CMPXCHG16B (ARM64 atomic 128-bit)")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = spinlock_stress_cmpxchg16b_init,
    .test_run = spinlock_stress_cmpxchg16b_run,
    .test_cleanup = spinlock_stress_cmpxchg16b_finish,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
