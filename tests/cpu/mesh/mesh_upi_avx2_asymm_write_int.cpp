#include <sandstone.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <atomic>
#include <random>
#ifdef __aarch64__
#include <arm_neon.h>
#include <unistd.h>
#include <sys/sysinfo.h>

#define TOTAL_ELEMENTS 8
#define VECTOR_SIZE 4                     // NEON 一次处理 4 个 int32
#define BLOCK_ELEMENTS TOTAL_ELEMENTS     // 块大小仍为 8（两个向量）
#define NUM_BLOCKS (TOTAL_ELEMENTS / BLOCK_ELEMENTS)   // =1
#define TOTAL_BYTES (TOTAL_ELEMENTS * sizeof(int32_t))

struct TestData {
    alignas(16) int32_t *data;            // 16 字节对齐即可
    std::atomic<uint32_t> next_block;
    std::atomic<uint64_t> global_sum;
    std::atomic<uint32_t> allocated_blocks;
    std::atomic<uint32_t> thread_idx;
    std::atomic<uint32_t> reset_lock;
};

static int mesh_upi_avx2_asymm_write_int_init(struct test *test) {
    if (sysconf(_SC_NPROCESSORS_ONLN) < 2) {
        return -255;
    }

    auto *td = new TestData;
    if (!td) return EXIT_FAILURE;

    td->data = (int32_t*)aligned_alloc(16, TOTAL_BYTES);
    if (!td->data) {
        delete td;
        return EXIT_FAILURE;
    }
    memset(td->data, 0, TOTAL_BYTES);

    td->next_block.store(0, std::memory_order_relaxed);
    td->global_sum.store(0, std::memory_order_relaxed);
    td->allocated_blocks.store(0, std::memory_order_relaxed);
    td->thread_idx.store(0, std::memory_order_relaxed);
    td->reset_lock.store(0, std::memory_order_relaxed);
    test->data = td;

    return EXIT_SUCCESS;
}

static int mesh_upi_avx2_asymm_write_int_run(struct test *test, int cpu) {
    (void)cpu;
    auto *td = static_cast<TestData*>(test->data);

    int id = td->thread_idx.fetch_add(1, std::memory_order_relaxed);
    bool is_reader = (id == 0);

    std::mt19937 rng(std::random_device{}());
    std::uniform_int_distribution<int32_t> dist(-1000000, 1000000);

    #define GREEN "\033[32m"
    #define RED   "\033[31m"
    #define RESET "\033[0m"

    do {
        if (!is_reader) {
            // 写核心：竞争分配块并写入
            while (test_time_condition(test)) {
                uint32_t block = td->next_block.fetch_add(1, std::memory_order_seq_cst);
                if (block < NUM_BLOCKS) {
                    // 生成随机数据（8个 int32）
                    int32_t vals[TOTAL_ELEMENTS];
                    uint64_t local_sum = 0;
                    for (int j = 0; j < TOTAL_ELEMENTS; ++j) {
                        vals[j] = dist(rng);
                        local_sum += (uint64_t)vals[j];
                    }
                    // 分两个 NEON 向量存储
                    int32x4_t v0 = vld1q_s32(vals);
                    int32x4_t v1 = vld1q_s32(vals + 4);
                    vst1q_s32(td->data, v0);
                    vst1q_s32(td->data + 4, v1);
                    __sync_synchronize();   // 确保数据对其它核心可见

                    td->global_sum.fetch_add(local_sum, std::memory_order_seq_cst);
                    td->allocated_blocks.fetch_add(1, std::memory_order_seq_cst);
                } else {
                    // 没有可用块，等待读核心重置（allocated_blocks 变为 0）
                    while (td->allocated_blocks.load(std::memory_order_seq_cst) > 0 &&
                           test_time_condition(test)) {
                        __asm__ volatile("yield");
                    }
                    if (!test_time_condition(test)) break;
                }
            }
        } else {
            // 读核心：等待数据就绪，校验，并重置
            while (test_time_condition(test)) {
                // 等待写核心完成写入
                while (td->allocated_blocks.load(std::memory_order_seq_cst) < NUM_BLOCKS &&
                       test_time_condition(test)) {
                    __asm__ volatile("yield");
                }
                if (!test_time_condition(test)) break;

                // 读取数据（两个向量）
                int32x4_t v0 = vld1q_s32(td->data);
                int32x4_t v1 = vld1q_s32(td->data + 4);
                int32_t vals[TOTAL_ELEMENTS];
                vst1q_s32(vals, v0);
                vst1q_s32(vals + 4, v1);
                uint64_t read_sum = 0;
                for (int j = 0; j < TOTAL_ELEMENTS; ++j) {
                    read_sum += (uint64_t)vals[j];
                }

                uint64_t expected_sum = td->global_sum.load(std::memory_order_seq_cst);
                bool passed = (read_sum == expected_sum);

                fprintf(stderr, "mesh_upi_avx2_asymm_write_int: Thread %d (reader), data[0..7]=(%d,%d,%d,%d,%d,%d,%d,%d), read_sum=%lu, expected_sum=%lu, result=%s%s%s\n",
                        id,
                        td->data[0], td->data[1], td->data[2], td->data[3],
                        td->data[4], td->data[5], td->data[6], td->data[7],
                        read_sum, expected_sum,
                        passed ? GREEN : RED,
                        passed ? "PASS" : "FAIL",
                        RESET);
                fflush(stderr);

                if (!passed) {
                    report_fail_msg("mesh_upi_avx2_asymm_write_int: Sum mismatch");
                    return EXIT_FAILURE;
                }

                // ---------- 重置状态（仅由一个读核心执行） ----------
                uint32_t expected_lock = 0;
                if (td->reset_lock.compare_exchange_strong(expected_lock, 1,
                                                           std::memory_order_seq_cst,
                                                           std::memory_order_seq_cst)) {
                    td->global_sum.store(0, std::memory_order_seq_cst);
                    td->next_block.store(0, std::memory_order_seq_cst);
                    td->allocated_blocks.store(0, std::memory_order_seq_cst);
                    td->reset_lock.store(0, std::memory_order_seq_cst);
                } else {
                    // 未获得锁，等待重置完成
                    while (td->allocated_blocks.load(std::memory_order_seq_cst) > 0 &&
                           test_time_condition(test)) {
                        __asm__ volatile("yield");
                    }
                    if (!test_time_condition(test)) break;
                }
            }
        }
    } while (test_time_condition(test));

    return EXIT_SUCCESS;

    #undef GREEN
    #undef RED
    #undef RESET
}

static int mesh_upi_avx2_asymm_write_int_finish(struct test *test) {
    auto *td = static_cast<TestData*>(test->data);
    free(td->data);
    delete td;
    return EXIT_SUCCESS;
}

#else
static int mesh_upi_avx2_asymm_write_int_init(struct test *test) {
    (void)test;
    log_skip(CpuNotSupportedSkipCategory,
             "to be implemented (placeholder): ARM NEON required for mesh_upi_avx2_asymm_write_int");
    return EXIT_SKIP;
}
static int mesh_upi_avx2_asymm_write_int_run(struct test *test, int cpu) { (void)test; (void)cpu; return EXIT_SKIP; }
static int mesh_upi_avx2_asymm_write_int_finish(struct test *test) { (void)test; return EXIT_SUCCESS; }
#endif
DECLARE_TEST(mesh_upi_avx2_asymm_write_int,
             "Asymmetrical(1 read core from all rest mutual write cores) stress MESH and UPI or Ring Interconnect and QPI (do int32 AVX-2(YMM) loads and stores from L1/L2 to L1/L2 with switching read core) [ARM NEON version]")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = mesh_upi_avx2_asymm_write_int_init,
    .test_run = mesh_upi_avx2_asymm_write_int_run,
    .test_cleanup = mesh_upi_avx2_asymm_write_int_finish,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
