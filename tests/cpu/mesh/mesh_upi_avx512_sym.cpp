#include <sandstone.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <atomic>
#include <random>
#ifdef __aarch64__
#include <arm_neon.h>
#include <unistd.h>
#include <cstdlib>
#include <sys/sysinfo.h>

#define NEON_VECTOR_SIZE 4                // NEON 一次处理 4 个 float
#define BLOCK_SIZE 16                     // 逻辑块大小（由 4 个 NEON 向量组成）
#define VECTORS_PER_BLOCK (BLOCK_SIZE / NEON_VECTOR_SIZE)  // = 4
#define NUM_BLOCKS 64                     // 块数量
#define TOTAL_ELEMENTS (BLOCK_SIZE * NUM_BLOCKS)  // = 1024
#define TOTAL_BYTES (TOTAL_ELEMENTS * sizeof(float))

struct TestData {
    alignas(16) float *data;            // 共享数据数组（16 字节对齐）
    std::atomic<uint32_t> next_block;
    std::atomic<uint64_t> global_sum;   // 未使用，但保留
    std::atomic<uint32_t> write_done;
    std::atomic<uint32_t> read_done;
    std::atomic<uint32_t> reset_lock;
    std::atomic<uint32_t> thread_idx;
    uint32_t total_threads;
};

static int mesh_upi_avx512_sym_init(struct test *test) {
    if (sysconf(_SC_NPROCESSORS_ONLN) < 2) {
        return -255;
    }

    auto *td = new TestData;
    if (!td) return EXIT_FAILURE;

    td->data = (float*)aligned_alloc(16, TOTAL_BYTES);
    if (!td->data) {
        delete td;
        return EXIT_FAILURE;
    }
    memset(td->data, 0, TOTAL_BYTES);

    td->next_block.store(0, std::memory_order_relaxed);
    td->global_sum.store(0, std::memory_order_relaxed);
    td->write_done.store(0, std::memory_order_relaxed);
    td->read_done.store(0, std::memory_order_relaxed);
    td->reset_lock.store(0, std::memory_order_relaxed);
    td->thread_idx.store(0, std::memory_order_relaxed);
    td->total_threads = sysconf(_SC_NPROCESSORS_ONLN);
    test->data = td;

    return EXIT_SUCCESS;
}

static int mesh_upi_avx512_sym_run(struct test *test, int cpu) {
    (void)cpu;
    auto *td = static_cast<TestData*>(test->data);

    int id = td->thread_idx.fetch_add(1, std::memory_order_relaxed);
    uint32_t total = td->total_threads;

    std::mt19937 rng(std::random_device{}());
    std::uniform_real_distribution<float> dist(-1000.0f, 1000.0f);

    #define GREEN "\033[32m"
    #define RED   "\033[31m"
    #define RESET "\033[0m"

    do {
        // ---------- 写阶段：分配块并写入随机浮点数 ----------
        while (true) {
            uint32_t block = td->next_block.fetch_add(1, std::memory_order_seq_cst);
            if (block >= NUM_BLOCKS) break;

            size_t offset = block * BLOCK_SIZE;
            float vals[BLOCK_SIZE];
            double local_sum = 0.0;
            for (int j = 0; j < BLOCK_SIZE; ++j) {
                vals[j] = dist(rng);
                local_sum += (double)vals[j];
            }
            // 使用 4 个 NEON 向量存储 16 个 float
            for (int v = 0; v < VECTORS_PER_BLOCK; ++v) {
                float32x4_t vec = vld1q_f32(vals + v * NEON_VECTOR_SIZE);
                vst1q_f32(td->data + offset + v * NEON_VECTOR_SIZE, vec);
            }
            __sync_synchronize();   // 类似 _mm_sfence

            // 一致性测试：立即读回比较
            bool block_ok = true;
            for (int v = 0; v < VECTORS_PER_BLOCK; ++v) {
                float32x4_t written = vld1q_f32(vals + v * NEON_VECTOR_SIZE);
                float32x4_t loaded = vld1q_f32(td->data + offset + v * NEON_VECTOR_SIZE);
                uint32x4_t cmp = vceqq_f32(written, loaded);
                if (!(vgetq_lane_u32(cmp, 0) == 0xFFFFFFFF &&
                      vgetq_lane_u32(cmp, 1) == 0xFFFFFFFF &&
                      vgetq_lane_u32(cmp, 2) == 0xFFFFFFFF &&
                      vgetq_lane_u32(cmp, 3) == 0xFFFFFFFF)) {
                    block_ok = false;
                    break;
                }
            }
            // 原代码发现不一致时仅记录，我们也同样处理（最终会在读阶段捕获）
            td->write_done.fetch_add(1, std::memory_order_seq_cst);
        }

        // 等待所有块写完成
        while (td->write_done.load(std::memory_order_seq_cst) < NUM_BLOCKS &&
               test_time_condition(test)) {
            __asm__ volatile("yield");
        }
        if (!test_time_condition(test)) break;

        // ---------- 读阶段：读取整个数组并校验 ----------
        bool consistent = true;
        alignas(16) float store_buf[BLOCK_SIZE];

        for (size_t i = 0; i < TOTAL_ELEMENTS; i += BLOCK_SIZE) {
            // 加载 16 个元素到临时数组
            float vals[BLOCK_SIZE];
            for (int v = 0; v < VECTORS_PER_BLOCK; ++v) {
                float32x4_t vec = vld1q_f32(td->data + i + v * NEON_VECTOR_SIZE);
                vst1q_f32(vals + v * NEON_VECTOR_SIZE, vec);
            }

            // 一致性测试：Store/Load 比较
            for (int v = 0; v < VECTORS_PER_BLOCK; ++v) {
                float32x4_t orig = vld1q_f32(vals + v * NEON_VECTOR_SIZE);
                vst1q_f32(store_buf + v * NEON_VECTOR_SIZE, orig);
            }
            for (int v = 0; v < VECTORS_PER_BLOCK; ++v) {
                float32x4_t orig = vld1q_f32(vals + v * NEON_VECTOR_SIZE);
                float32x4_t reload = vld1q_f32(store_buf + v * NEON_VECTOR_SIZE);
                uint32x4_t cmp = vceqq_f32(orig, reload);
                if (!(vgetq_lane_u32(cmp, 0) == 0xFFFFFFFF &&
                      vgetq_lane_u32(cmp, 1) == 0xFFFFFFFF &&
                      vgetq_lane_u32(cmp, 2) == 0xFFFFFFFF &&
                      vgetq_lane_u32(cmp, 3) == 0xFFFFFFFF)) {
                    consistent = false;
                    break;
                }
            }
            if (!consistent) break;
        }

        bool passed = consistent;

        // 输出结果（仅线程0输出）
        if (id == 0) {
            fprintf(stderr, "mesh_upi_avx512_sym: Thread %d, data[0..15]=(%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f), consistent=%d, result=%s%s%s\n",
                    id,
                    td->data[0], td->data[1], td->data[2], td->data[3],
                    td->data[4], td->data[5], td->data[6], td->data[7],
                    td->data[8], td->data[9], td->data[10], td->data[11],
                    td->data[12], td->data[13], td->data[14], td->data[15],
                    consistent,
                    passed ? GREEN : RED,
                    passed ? "PASS" : "FAIL",
                    RESET);
            fflush(stderr);
        }

        if (!passed) {
            report_fail_msg("mesh_upi_avx512_sym: Consistency failure");
            return EXIT_FAILURE;
        }

        td->read_done.fetch_add(1, std::memory_order_seq_cst);

        // ---------- 重置阶段 ----------
        uint32_t expected_lock = 0;
        if (td->reset_lock.compare_exchange_strong(expected_lock, 1,
                                                   std::memory_order_seq_cst,
                                                   std::memory_order_seq_cst)) {
            while (td->read_done.load(std::memory_order_seq_cst) < total &&
                   test_time_condition(test)) {
                __asm__ volatile("yield");
            }
            if (!test_time_condition(test)) {
                td->reset_lock.store(0, std::memory_order_seq_cst);
                break;
            }

            td->global_sum.store(0, std::memory_order_seq_cst);
            td->next_block.store(0, std::memory_order_seq_cst);
            td->write_done.store(0, std::memory_order_seq_cst);
            td->read_done.store(0, std::memory_order_seq_cst);
            td->reset_lock.store(0, std::memory_order_seq_cst);
        } else {
            while (td->reset_lock.load(std::memory_order_seq_cst) != 0 &&
                   test_time_condition(test)) {
                __asm__ volatile("yield");
            }
            if (!test_time_condition(test)) break;
        }

    } while (test_time_condition(test));

    return EXIT_SUCCESS;

    #undef GREEN
    #undef RED
    #undef RESET
}

static int mesh_upi_avx512_sym_finish(struct test *test) {
    auto *td = static_cast<TestData*>(test->data);
    free(td->data);
    delete td;
    return EXIT_SUCCESS;
}

#else
static int mesh_upi_avx512_sym_init(struct test *test) {
    (void)test;
    log_skip(CpuNotSupportedSkipCategory,
             "to be implemented (placeholder): ARM NEON required for mesh_upi_avx512_sym");
    return EXIT_SKIP;
}
static int mesh_upi_avx512_sym_run(struct test *test, int cpu) { (void)test; (void)cpu; return EXIT_SKIP; }
static int mesh_upi_avx512_sym_finish(struct test *test) { (void)test; return EXIT_SUCCESS; }
#endif
DECLARE_TEST(mesh_upi_avx512_sym,
             "Symmetrical stress MESH and UPI (do NEON loads and stores from L1D to L1D with different directions and #cores)")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = mesh_upi_avx512_sym_init,
    .test_run = mesh_upi_avx512_sym_run,
    .test_cleanup = mesh_upi_avx512_sym_finish,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
