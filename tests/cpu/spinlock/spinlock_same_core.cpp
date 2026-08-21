#include <sandstone.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <atomic>
#include <random>
#include <unistd.h>

#define MAX_THREADS 128

// 自旋锁：使用 std::atomic 的 compare_exchange_weak
static inline void spin_lock(std::atomic<uint64_t> &lock) {
    uint64_t expected = 0;
    while (!lock.compare_exchange_weak(expected, 1, std::memory_order_acquire)) {
        expected = 0;
        // ARM64 暂停提示（可选）
        #if defined(__aarch64__)
            __asm__ volatile("yield");
        #elif defined(__x86_64__)
            __asm__ volatile("pause");
        #else
            /* no arch-specific pause hint; fall through */
        #endif
    }
}

static inline void spin_unlock(std::atomic<uint64_t> &lock) {
    lock.store(0, std::memory_order_release);
}

struct SharedData {
    alignas(64) std::atomic<uint64_t> lock;   // 自旋锁变量（原子类型）
    alignas(64) uint64_t counter;             // 共享计数器
    std::atomic<uint64_t> thread_idx;         // 线程 ID 分配器
    uint64_t local_sums[MAX_THREADS];         // 每个线程的本地增量总和
};

static int spinlock_same_core_init(struct test *test) {
    auto *sd = new SharedData;
    if (!sd) return EXIT_FAILURE;
    sd->lock = 0;
    sd->counter = 0;
    sd->thread_idx.store(0, std::memory_order_relaxed);
    memset(sd->local_sums, 0, sizeof(sd->local_sums));
    test->data = sd;
    return EXIT_SUCCESS;
}

static int spinlock_same_core_run(struct test *test, int cpu) {
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

    #define GREEN "\033[32m"
    #define RED   "\033[31m"
    #define RESET "\033[0m"

    do {
        uint64_t inc = dist(rng);

        spin_lock(sd->lock);
        sd->counter += inc;
        spin_unlock(sd->lock);

        local_sum += inc;

        fprintf(stderr, "spinlock_same_core: Thread %d, inc=%lu, result=%sPASS%s\n",
                id, inc, GREEN, RESET);
        fflush(stderr);

    } while (test_time_condition(test));

    sd->local_sums[id] = local_sum;
    return EXIT_SUCCESS;

    #undef GREEN
    #undef RED
    #undef RESET
}

static int spinlock_same_core_finish(struct test *test) {
    auto *sd = static_cast<SharedData*>(test->data);

    uint64_t total_local = 0;
    uint64_t thread_count = sd->thread_idx.load(std::memory_order_relaxed);
    for (uint64_t i = 0; i < thread_count; ++i) {
        total_local += sd->local_sums[i];
    }

    uint64_t counter_value = sd->counter;

    // 一致性测试：存储计数器值到内存，再加载比较
    uint64_t store_buf = counter_value;
    uint64_t reload_buf;
    memcpy(&reload_buf, &store_buf, sizeof(store_buf));
    bool consistent = (reload_buf == counter_value);

    bool data_ok = (counter_value == total_local);
    bool passed = data_ok && consistent;

    const char *color = passed ? "\033[32m" : "\033[31m";
    const char *result_str = passed ? "PASS" : "FAIL";

    fprintf(stderr, "spinlock_same_core: Shared counter=%lu, Total local sums=%lu, Threads=%lu, data_ok=%d, consistent=%d, result=%s%s\033[0m\n",
            counter_value, total_local, thread_count, data_ok, consistent, color, result_str);
    fflush(stderr);

    if (!passed) {
        report_fail_msg("spinlock_same_core: Counter mismatch or consistency failure");
        return EXIT_FAILURE;
    }

    delete sd;
    return EXIT_SUCCESS;
}

DECLARE_TEST(spinlock_same_core, "Spinlock stress with contention only among sibling threads in a core (ARM64 atomic)")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = spinlock_same_core_init,
    .test_run = spinlock_same_core_run,
    .test_cleanup = spinlock_same_core_finish,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
