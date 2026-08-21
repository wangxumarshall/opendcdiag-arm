#include <sandstone.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <atomic>
#include <random>
#include <vector>
#include <memory>
#include <unistd.h>

#define MAX_THREADS 128
#define ARRAY_SIZE (1 << 20)      // 1M 个 64 位元素 = 8MB（超出 L3 缓存，产生 TLB 压力）
// 可调整：1<<22 (32MB) 或更大以增大 TLB 压力

struct SharedData {
    std::unique_ptr<std::atomic<uint64_t>[]> array;  // 原子数组
    std::atomic<uint64_t> thread_idx;                // 线程 ID 分配器
    uint64_t local_sums[MAX_THREADS];                // 每个线程的本地增量总和
};

static int lockgen_init(struct test *test) {
    auto *sd = new SharedData;
    if (!sd) return EXIT_FAILURE;
    // 分配原子数组并初始化为 0
    sd->array.reset(new std::atomic<uint64_t>[ARRAY_SIZE]);
    for (size_t i = 0; i < ARRAY_SIZE; ++i) {
        sd->array[i].store(0, std::memory_order_relaxed);
    }
    sd->thread_idx.store(0, std::memory_order_relaxed);
    memset(sd->local_sums, 0, sizeof(sd->local_sums));
    test->data = sd;
    return EXIT_SUCCESS;
}

static int lockgen_run(struct test *test, int cpu) {
    (void)cpu;
    auto *sd = static_cast<SharedData*>(test->data);

    int id = sd->thread_idx.fetch_add(1, std::memory_order_relaxed);
    if (id >= MAX_THREADS) {
        report_fail_msg("Too many threads");
        return EXIT_FAILURE;
    }

    std::mt19937 rng(std::random_device{}());
    std::uniform_int_distribution<uint64_t> inc_dist(1, 1000);
    std::uniform_int_distribution<size_t> idx_dist(0, ARRAY_SIZE - 1);
    uint64_t local_sum = 0;

    #define GREEN "\033[32m"
    #define RED   "\033[31m"
    #define RESET "\033[0m"

    do {
        uint64_t inc = inc_dist(rng);
        size_t idx = idx_dist(rng);   // 随机索引，产生 TLB 和缓存压力

        // 使用 std::atomic 的 compare_exchange_strong 循环直到成功
        auto &cell = sd->array[idx];
        uint64_t old_val, new_val;
        do {
            old_val = cell.load(std::memory_order_acquire);
            new_val = old_val + inc;
        } while (!cell.compare_exchange_strong(old_val, new_val,
                                               std::memory_order_acq_rel,
                                               std::memory_order_acquire));

        local_sum += inc;

        // 输出本次的输入（线程、索引、增量）和结果（操作成功，PASS）
        fprintf(stderr, "lockgen: Thread %d, idx=%zu, inc=%lu, result=%sPASS%s\n",
                id, idx, inc, GREEN, RESET);
        fflush(stderr);

    } while (test_time_condition(test));

    sd->local_sums[id] = local_sum;
    return EXIT_SUCCESS;

    #undef GREEN
    #undef RED
    #undef RESET
}

static int lockgen_finish(struct test *test) {
    auto *sd = static_cast<SharedData*>(test->data);

    uint64_t total_local = 0;
    uint64_t thread_count = sd->thread_idx.load(std::memory_order_relaxed);
    for (uint64_t i = 0; i < thread_count; ++i) {
        total_local += sd->local_sums[i];
    }

    // 计算所有数组元素的总和
    uint64_t total_array = 0;
    for (size_t i = 0; i < ARRAY_SIZE; ++i) {
        total_array += sd->array[i].load(std::memory_order_acquire);
    }

    // 一致性测试：存储总数组值到内存再加载比较（可选）
    uint64_t store_buf = total_array;
    uint64_t reload_buf;
    memcpy(&reload_buf, &store_buf, sizeof(store_buf));
    bool consistent = (reload_buf == total_array);

    bool data_ok = (total_array == total_local);
    bool passed = data_ok && consistent;

    const char *color = passed ? "\033[32m" : "\033[31m";
    const char *result_str = passed ? "PASS" : "FAIL";

    fprintf(stderr, "lockgen: Total array sum=%lu, Total local sums=%lu, Threads=%lu, data_ok=%d, consistent=%d, result=%s%s\033[0m\n",
            total_array, total_local, thread_count, data_ok, consistent, color, result_str);
    fflush(stderr);

    if (!passed) {
        report_fail_msg("lockgen: Array sum mismatch or consistency failure");
        return EXIT_FAILURE;
    }

    delete sd;
    return EXIT_SUCCESS;
}

DECLARE_TEST(lockgen, "lockgen - stresses lock cmpxchg with TLB/cache stress")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = lockgen_init,
    .test_run = lockgen_run,
    .test_cleanup = lockgen_finish,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
