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
    std::atomic<uint32_t> round_done;
    std::atomic<uint32_t> reader_count;
};

static int mesh_upi_avx2_asymm_distrib_int_init(struct test *test) {
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
    td->round_done.store(0, std::memory_order_relaxed);
    td->reader_count.store(0, std::memory_order_relaxed);
    test->data = td;

    return EXIT_SUCCESS;
}

static int mesh_upi_avx2_asymm_distrib_int_run(struct test *test, int cpu) {
    (void)cpu;
    auto *td = static_cast<TestData*>(test->data);

    int id = td->thread_idx.fetch_add(1, std::memory_order_relaxed);
    bool is_writer = (id == 0);

    if (!is_writer) {
        td->reader_count.fetch_add(1, std::memory_order_relaxed);
    }

    std::mt19937 rng(std::random_device{}());
    std::uniform_int_distribution<int32_t> dist(-1000000, 1000000);

    #define GREEN "\033[32m"
    #define RED   "\033[31m"
    #define RESET "\033[0m"

    do {
        if (is_writer) {
            while (test_time_condition(test)) {
                // 等待读核心完成上一轮（allocated_blocks == 0 且 round_done == 0）
                while (td->allocated_blocks.load(std::memory_order_seq_cst) > 0 ||
                       td->round_done.load(std::memory_order_seq_cst) > 0) {
                    if (!test_time_condition(test)) break;
                    __asm__ volatile("yield");
                }
                if (!test_time_condition(test)) break;

                uint32_t block = td->next_block.fetch_add(1, std::memory_order_seq_cst);
                if (block < NUM_BLOCKS) {
                    // 生成随机数据并写入（8个 int32，分两个 NEON 向量）
                    int32_t vals[TOTAL_ELEMENTS];
                    uint64_t local_sum = 0;
                    for (int j = 0; j < TOTAL_ELEMENTS; ++j) {
                        vals[j] = dist(rng);
                        local_sum += (uint64_t)vals[j];
                    }
                    // 前 4 个
                    int32x4_t v0 = vld1q_s32(vals);
                    vst1q_s32(td->data, v0);
                    // 后 4 个
                    int32x4_t v1 = vld1q_s32(vals + 4);
                    vst1q_s32(td->data + 4, v1);
                    // 内存屏障确保数据对其它核心可见
                    __sync_synchronize();

                    td->global_sum.fetch_add(local_sum, std::memory_order_seq_cst);
                    td->allocated_blocks.fetch_add(1, std::memory_order_seq_cst);
                }
            }
        } else {
            // 读核心
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
                int32_t vals[8];
                vst1q_s32(vals, v0);
                vst1q_s32(vals + 4, v1);
                uint64_t read_sum = 0;
                for (int j = 0; j < TOTAL_ELEMENTS; ++j) {
                    read_sum += (uint64_t)vals[j];
                }

                uint64_t expected_sum = td->global_sum.load(std::memory_order_seq_cst);
                bool passed = (read_sum == expected_sum);

                // 输出结果（仅第一个读核心输出以节省日志）
                if (id == 1) {
                    fprintf(stderr, "mesh_upi_avx2_asymm_distrib_int: Thread %d (reader), data[0..7]=(%d,%d,%d,%d,%d,%d,%d,%d), read_sum=%lu, expected_sum=%lu, result=%s%s%s\n",
                            id,
                            td->data[0], td->data[1], td->data[2], td->data[3],
                            td->data[4], td->data[5], td->data[6], td->data[7],
                            read_sum, expected_sum,
                            passed ? GREEN : RED,
                            passed ? "PASS" : "FAIL",
                            RESET);
                    fflush(stderr);
                }

                if (!passed) {
                    report_fail_msg("mesh_upi_avx2_asymm_distrib_int: Sum mismatch");
                    return EXIT_FAILURE;
                }

                // 同步重置：最后一个读核心重置状态
                uint32_t done = td->round_done.fetch_add(1, std::memory_order_seq_cst) + 1;
                uint32_t total_readers = td->reader_count.load(std::memory_order_acquire);
                if (done == total_readers) {
                    td->allocated_blocks.store(0, std::memory_order_seq_cst);
                    td->next_block.store(0, std::memory_order_seq_cst);
                    td->global_sum.store(0, std::memory_order_seq_cst);
                    td->round_done.store(0, std::memory_order_seq_cst);
                } else {
                    while (td->round_done.load(std::memory_order_seq_cst) != 0 &&
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

static int mesh_upi_avx2_asymm_distrib_int_finish(struct test *test) {
    auto *td = static_cast<TestData*>(test->data);
    free(td->data);
    delete td;
    return EXIT_SUCCESS;
}

#else
static int mesh_upi_avx2_asymm_distrib_int_init(struct test *test) {
    (void)test;
    log_skip(CpuNotSupportedSkipCategory,
             "to be implemented (placeholder): ARM NEON required for mesh_upi_avx2_asymm_distrib_int");
    return EXIT_SKIP;
}
static int mesh_upi_avx2_asymm_distrib_int_run(struct test *test, int cpu) { (void)test; (void)cpu; return EXIT_SKIP; }
static int mesh_upi_avx2_asymm_distrib_int_finish(struct test *test) { (void)test; return EXIT_SUCCESS; }
#endif
DECLARE_TEST(mesh_upi_avx2_asymm_distrib_int,
             "Asymmetrical(1 write core to all rest distributed read cores) stress MESH and UPI or Ring Interconnect and QPI (do int32 AVX-2(YMM) loads and stores from L1/L2 to L1/L2 with switching write core) [ARM NEON version]")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = mesh_upi_avx2_asymm_distrib_int_init,
    .test_run = mesh_upi_avx2_asymm_distrib_int_run,
    .test_cleanup = mesh_upi_avx2_asymm_distrib_int_finish,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
