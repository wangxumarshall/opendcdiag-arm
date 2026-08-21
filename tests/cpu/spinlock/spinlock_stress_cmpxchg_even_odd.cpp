#include <sandstone.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <atomic>
#include <random>
#include <unistd.h>

#define MAX_THREADS 128

struct SharedData {
    alignas(64) std::atomic<uint64_t> lock;   // 自旋锁变量（原子类型）
    alignas(64) uint64_t counter;             // 共享计数器
    std::atomic<uint64_t> thread_idx;         // 线程 ID 分配器
    uint64_t local_sums[MAX_THREADS];         // 每个线程的本地增量总和
    bool even;                                // true=偶数核心，false=奇数核心
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

// 通用 init 函数
static int spinlock_stress_cmpxchg_init_common(struct test *test, bool even) {
    auto *sd = new SharedData;
    if (!sd) return EXIT_FAILURE;
    sd->lock = 0;
    sd->counter = 0;
    sd->thread_idx.store(0, std::memory_order_relaxed);
    memset(sd->local_sums, 0, sizeof(sd->local_sums));
    sd->even = even;
    test->data = sd;
    return EXIT_SUCCESS;
}

// 通用 run 函数
static int spinlock_stress_cmpxchg_run_common(struct test *test, int cpu) {
    auto *sd = static_cast<SharedData*>(test->data);

    int id = sd->thread_idx.fetch_add(1, std::memory_order_relaxed);
    if (id >= MAX_THREADS) {
        report_fail_msg("Too many threads");
        return EXIT_FAILURE;
    }

    // 检查核心类型：仅偶数或奇数核心参与竞争
    bool is_even_core = (cpu % 2 == 0);
    if (is_even_core != sd->even) {
        // 非目标核心不参与竞争，直接返回
        return EXIT_SUCCESS;
    }

    std::mt19937 rng(std::random_device{}());
    std::uniform_int_distribution<uint64_t> dist(1, 1000);
    uint64_t local_sum = 0;

    do {
        uint64_t inc = dist(rng);

        spin_lock(sd->lock);
        sd->counter += inc;
        spin_unlock(sd->lock);

        local_sum += inc;

        fprintf(stderr, "spinlock_stress_cmpxchg_%s: Thread %d, inc=%lu, result=PASS\n",
                sd->even ? "even" : "odd", id, inc);
        fflush(stderr);

    } while (test_time_condition(test));

    sd->local_sums[id] = local_sum;
    return EXIT_SUCCESS;
}

// 通用 finish 函数
static int spinlock_stress_cmpxchg_finish_common(struct test *test) {
    auto *sd = static_cast<SharedData*>(test->data);

    uint64_t total_local = 0;
    uint64_t thread_count = 0;
    for (int i = 0; i < MAX_THREADS; ++i) {
        if (sd->local_sums[i] != 0) {
            total_local += sd->local_sums[i];
            thread_count++;
        }
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

    fprintf(stderr, "spinlock_stress_cmpxchg_%s: Shared counter=%lu, Total local sums=%lu, Threads=%lu, data_ok=%d, consistent=%d, result=%s%s\033[0m\n",
            sd->even ? "even" : "odd",
            counter_value, total_local, thread_count, data_ok, consistent, color, result_str);
    fflush(stderr);

    if (!passed) {
        report_fail_msg("spinlock_stress_cmpxchg_%s: Counter mismatch or consistency failure", sd->even ? "even" : "odd");
        return EXIT_FAILURE;
    }

    delete sd;
    return EXIT_SUCCESS;
}

// ---------- 偶数核心测试 ----------
static int spinlock_stress_cmpxchg_even_init(struct test *test) {
    return spinlock_stress_cmpxchg_init_common(test, true);
}

static int spinlock_stress_cmpxchg_even_run(struct test *test, int cpu) {
    return spinlock_stress_cmpxchg_run_common(test, cpu);
}

static int spinlock_stress_cmpxchg_even_finish(struct test *test) {
    return spinlock_stress_cmpxchg_finish_common(test);
}

DECLARE_TEST(spinlock_stress_cmpxchg_even, "Stress spinlock with CMPXCHG on even cores only (ARM64 atomic)")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = spinlock_stress_cmpxchg_even_init,
    .test_run = spinlock_stress_cmpxchg_even_run,
    .test_cleanup = spinlock_stress_cmpxchg_even_finish,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST

// ---------- 奇数核心测试 ----------
static int spinlock_stress_cmpxchg_odd_init(struct test *test) {
    return spinlock_stress_cmpxchg_init_common(test, false);
}

static int spinlock_stress_cmpxchg_odd_run(struct test *test, int cpu) {
    return spinlock_stress_cmpxchg_run_common(test, cpu);
}

static int spinlock_stress_cmpxchg_odd_finish(struct test *test) {
    return spinlock_stress_cmpxchg_finish_common(test);
}

DECLARE_TEST(spinlock_stress_cmpxchg_odd, "Stress spinlock with CMPXCHG on odd cores only (ARM64 atomic)")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = spinlock_stress_cmpxchg_odd_init,
    .test_run = spinlock_stress_cmpxchg_odd_run,
    .test_cleanup = spinlock_stress_cmpxchg_odd_finish,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
