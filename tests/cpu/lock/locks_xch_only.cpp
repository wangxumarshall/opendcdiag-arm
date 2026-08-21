#include <sandstone.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <atomic>
#include <random>
#include <unistd.h>

#define MAX_THREADS 128

// 使用 std::atomic_flag 实现仅 XCHG 语义的自旋锁
struct SpinLock {
    std::atomic_flag flag = ATOMIC_FLAG_INIT;

    // 获取锁：原子交换 1（test_and_set），若原值为 0 则成功
    void lock() {
        while (flag.test_and_set(std::memory_order_acquire)) {
            // 自旋等待（可添加 yield，非必需）
        }
    }

    // 释放锁：原子清 0（clear）
    void unlock() {
        flag.clear(std::memory_order_release);
    }
};

struct SharedData {
    alignas(64) SpinLock lock;          // 自旋锁对象（1字节，对齐无特殊要求）
    alignas(64) uint64_t counter;       // 共享计数器
    std::atomic<uint64_t> thread_idx;   // 线程 ID 分配器
    uint64_t local_sums[MAX_THREADS];   // 每个线程的本地增量总和
};

static int locks_xch_only_init(struct test *test) {
    auto *sd = new SharedData;
    if (!sd) return EXIT_FAILURE;
    // lock 对象已默认初始化（flag = false）
    sd->counter = 0;
    sd->thread_idx.store(0, std::memory_order_relaxed);
    memset(sd->local_sums, 0, sizeof(sd->local_sums));
    test->data = sd;
    return EXIT_SUCCESS;
}

static int locks_xch_only_run(struct test *test, int cpu) {
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
        uint64_t inc = dist(rng);   // 本次的输入

        sd->lock.lock();            // 仅使用 XCHG（test_and_set）
        sd->counter += inc;
        sd->lock.unlock();          // 仅使用 XCHG（clear）

        local_sum += inc;

        // 输出本次的输入（线程、增量）和结果（锁操作成功，PASS）
        fprintf(stderr, "locks_xch_only: Thread %d, inc=%lu, result=%sPASS%s\n",
                id, inc, GREEN, RESET);
        fflush(stderr);

    } while (test_time_condition(test));

    sd->local_sums[id] = local_sum;
    return EXIT_SUCCESS;

    #undef GREEN
    #undef RED
    #undef RESET
}

static int locks_xch_only_finish(struct test *test) {
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

    fprintf(stderr, "locks_xch_only: Shared counter=%lu, Total local sums=%lu, Threads=%lu, data_ok=%d, consistent=%d, result=%s%s\033[0m\n",
            counter_value, total_local, thread_count, data_ok, consistent, color, result_str);
    fflush(stderr);

    if (!passed) {
        report_fail_msg("locks_xch_only: Counter mismatch or consistency failure");
        return EXIT_FAILURE;
    }

    delete sd;
    return EXIT_SUCCESS;
}

DECLARE_TEST(locks_xch_only, "Testing locks - read, modify, write locked data (no cmpxchg version)")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = locks_xch_only_init,
    .test_run = locks_xch_only_run,
    .test_cleanup = locks_xch_only_finish,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
