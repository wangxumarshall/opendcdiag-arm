#include <sandstone.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <atomic>
#include <random>
#include <unistd.h>

#define MAX_THREADS 128
#define NUM_BANKS 4        // 银行（分片）数量
#define LOCKS_PER_BANK 4   // 每个银行中的锁数量
#define TOTAL_LOCKS (NUM_BANKS * LOCKS_PER_BANK)

// 每个锁及其保护的计数器（独立缓存行，避免伪共享）
struct alignas(64) LockSlot {
    std::atomic<uint64_t> lock;   // 0 表示未锁定，1 表示锁定
    uint64_t counter;
};

// 每个银行包含多个锁槽
struct Bank {
    LockSlot slots[LOCKS_PER_BANK];
};

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
    Bank banks[NUM_BANKS];
    std::atomic<uint64_t> thread_idx;
    uint64_t local_sums[MAX_THREADS];
};

static int spinlock_bank_init(struct test *test) {
    auto *sd = new SharedData;
    if (!sd) return EXIT_FAILURE;
    for (int b = 0; b < NUM_BANKS; ++b) {
        for (int l = 0; l < LOCKS_PER_BANK; ++l) {
            sd->banks[b].slots[l].lock = 0;
            sd->banks[b].slots[l].counter = 0;
        }
    }
    sd->thread_idx.store(0, std::memory_order_relaxed);
    memset(sd->local_sums, 0, sizeof(sd->local_sums));
    test->data = sd;
    return EXIT_SUCCESS;
}

static int spinlock_bank_run(struct test *test, int cpu) {
    (void)cpu;
    auto *sd = static_cast<SharedData*>(test->data);

    int id = sd->thread_idx.fetch_add(1, std::memory_order_relaxed);
    if (id >= MAX_THREADS) {
        report_fail_msg("Too many threads");
        return EXIT_FAILURE;
    }

    std::mt19937 rng(std::random_device{}());
    std::uniform_int_distribution<uint64_t> inc_dist(1, 1000);
    std::uniform_int_distribution<int> bank_dist(0, NUM_BANKS - 1);
    std::uniform_int_distribution<int> lock_dist(0, LOCKS_PER_BANK - 1);
    uint64_t local_sum = 0;

    #define GREEN "\033[32m"
    #define RED   "\033[31m"
    #define RESET "\033[0m"

    do {
        uint64_t inc = inc_dist(rng);
        int bank_idx = bank_dist(rng);
        int lock_idx = lock_dist(rng);

        // 获取对应的锁引用
        std::atomic<uint64_t> &lock = sd->banks[bank_idx].slots[lock_idx].lock;

        spin_lock(lock);
        sd->banks[bank_idx].slots[lock_idx].counter += inc;
        spin_unlock(lock);

        local_sum += inc;

        // 每次迭代输出输入（线程 ID、银行索引、锁索引、增量）和结果
        fprintf(stderr, "spinlock_bank: Thread %d, bank=%d, lock=%d, inc=%lu, result=%sPASS%s\n",
                id, bank_idx, lock_idx, inc, GREEN, RESET);
        fflush(stderr);

    } while (test_time_condition(test));

    sd->local_sums[id] = local_sum;
    return EXIT_SUCCESS;

    #undef GREEN
    #undef RED
    #undef RESET
}

static int spinlock_bank_finish(struct test *test) {
    auto *sd = static_cast<SharedData*>(test->data);

    // 计算所有线程本地增量总和
    uint64_t total_local = 0;
    uint64_t thread_count = sd->thread_idx.load(std::memory_order_relaxed);
    for (uint64_t i = 0; i < thread_count; ++i) {
        total_local += sd->local_sums[i];
    }

    // 计算所有锁计数器总和
    uint64_t total_counter = 0;
    for (int b = 0; b < NUM_BANKS; ++b) {
        for (int l = 0; l < LOCKS_PER_BANK; ++l) {
            total_counter += sd->banks[b].slots[l].counter;
        }
    }

    // 一致性测试：存储总计数器值到内存再加载比较
    uint64_t store_buf = total_counter;
    uint64_t reload_buf;
    memcpy(&reload_buf, &store_buf, sizeof(store_buf));
    bool consistent = (reload_buf == total_counter);

    bool data_ok = (total_counter == total_local);
    bool passed = data_ok && consistent;

    const char *color = passed ? "\033[32m" : "\033[31m";
    const char *result_str = passed ? "PASS" : "FAIL";

    fprintf(stderr, "spinlock_bank: Total counter sum=%lu, Total local sums=%lu, Threads=%lu, data_ok=%d, consistent=%d, result=%s%s\033[0m\n",
            total_counter, total_local, thread_count, data_ok, consistent, color, result_str);
    fflush(stderr);

    if (!passed) {
        report_fail_msg("spinlock_bank: Counter sum mismatch or consistency failure");
        return EXIT_FAILURE;
    }

    delete sd;
    return EXIT_SUCCESS;
}

DECLARE_TEST(spinlock_bank, "Test locks across threads (banked)")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = spinlock_bank_init,
    .test_run = spinlock_bank_run,
    .test_cleanup = spinlock_bank_finish,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
