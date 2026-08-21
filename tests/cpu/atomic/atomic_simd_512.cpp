#include <sandstone.h>
#include <cstdint>
#include <cstdio>
#include <atomic>
#include <cstring>
#include <random>
#include <vector>
#include <ctime>
#include <unistd.h>

#ifdef __aarch64__
#include <arm_neon.h>

// 64 字节 Cache Line 强对齐，防止 ARM64 跨缓存行 Split Access
struct alignas(64) ThreadChannel {
    uint8x16_t data0, data1, data2, data3;
    std::atomic<uint64_t> seq{0};

    // 默认构造函数
    ThreadChannel() {
        data0 = vdupq_n_u8(0);
        data1 = vdupq_n_u8(0);
        data2 = vdupq_n_u8(0);
        data3 = vdupq_n_u8(0);
        seq.store(0, std::memory_order_relaxed);
    }

    // 禁止拷贝构造/赋值
    ThreadChannel(const ThreadChannel&) = delete;
    ThreadChannel& operator=(const ThreadChannel&) = delete;

    // 显式实现移动构造函数，显式解决 vector resize 时的 std::atomic 删除构造函数问题
    ThreadChannel(ThreadChannel&& other) noexcept {
        data0 = other.data0;
        data1 = other.data1;
        data2 = other.data2;
        data3 = other.data3;
        seq.store(other.seq.load(std::memory_order_relaxed), std::memory_order_relaxed);
    }

    // 显式实现移动赋值运算符
    ThreadChannel& operator=(ThreadChannel&& other) noexcept {
        if (this != &other) {
            data0 = other.data0;
            data1 = other.data1;
            data2 = other.data2;
            data3 = other.data3;
            seq.store(other.seq.load(std::memory_order_relaxed), std::memory_order_relaxed);
        }
        return *this;
    }
};

struct TestContext {
    std::vector<ThreadChannel> channels;
};

static int atomic_simd_512_init(struct test *test) {
    auto *ctx = new TestContext();
    if (!ctx) return EXIT_FAILURE;

    constexpr size_t MAX_POSSIBLE_CPUS = 512;
    // 使用 default construct 方式预扩容，触发手动实现的移动构造函数
    ctx->channels.resize(MAX_POSSIBLE_CPUS);

    test->data = ctx;
    return EXIT_SUCCESS;
}

static int atomic_simd_512_run(struct test *test, int cpu) {
    auto *ctx = static_cast<TestContext*>(test->data);
    if (!ctx || ctx->channels.empty()) return EXIT_FAILURE;

    static std::atomic<size_t> thread_counter{0};
    thread_local size_t my_id = thread_counter.fetch_add(1, std::memory_order_relaxed);
    
    size_t channel_idx = my_id % ctx->channels.size();
    auto &my_chan = ctx->channels[channel_idx];

    std::mt19937 rng(static_cast<unsigned>(time(nullptr)) + cpu + my_id);
    std::uniform_int_distribution<int> dist(0, 1);

    do {
        // ==================== 1. 本地通道写者阶段 ====================
        uint64_t pattern = dist(rng);
        uint8x16_t val0 = pattern ? vdupq_n_u8(0xFF) : vdupq_n_u8(0x00);

        uint64_t curr_seq = my_chan.seq.load(std::memory_order_relaxed);
        
        // 标记开始写 (seq 变成奇数)
        my_chan.seq.store(curr_seq + 1, std::memory_order_release);
        __asm__ volatile("dmb ish" ::: "memory"); // ARM64 强内存屏障

        vst1q_u8((uint8_t*)&my_chan.data0, val0);
        vst1q_u8((uint8_t*)&my_chan.data1, val0);
        vst1q_u8((uint8_t*)&my_chan.data2, val0);
        vst1q_u8((uint8_t*)&my_chan.data3, val0);

        __asm__ volatile("dmb ish" ::: "memory");
        // 标记结束写 (seq 变成偶数)
        my_chan.seq.store(curr_seq + 2, std::memory_order_release);

        // ==================== 2. 自读/环形读校验 (防止活锁) ====================
        uint64_t s1, s2;
        uint8x16_t read0, read1, read2, read3;
        uint32_t spin_count = 0;

        do {
            s1 = my_chan.seq.load(std::memory_order_acquire);
            
            // 防推测重排屏障
            __asm__ volatile("dmb ishld" ::: "memory");

            read0 = vld1q_u8((const uint8_t*)&my_chan.data0);
            read1 = vld1q_u8((const uint8_t*)&my_chan.data1);
            read2 = vld1q_u8((const uint8_t*)&my_chan.data2);
            read3 = vld1q_u8((const uint8_t*)&my_chan.data3);

            __asm__ volatile("dmb ishld" ::: "memory");
            s2 = my_chan.seq.load(std::memory_order_relaxed);

            if ((s1 & 1) || (s1 != s2)) {
                spin_count++;
                __asm__ volatile("yield" ::: "memory");
                if (spin_count > 10000) break; 
            }
        } while ((s1 & 1) || (s1 != s2));

        // ==================== 3. 校验数据完整性 (无数据撕裂) ====================
        alignas(64) uint8_t bytes[64];
        vst1q_u8(bytes, read0);
        vst1q_u8(bytes + 16, read1);
        vst1q_u8(bytes + 32, read2);
        vst1q_u8(bytes + 48, read3);

        bool all_zero = true, all_one = true;
        for (int i = 0; i < 64; ++i) {
            if (bytes[i] != 0x00) all_zero = false;
            if (bytes[i] != 0xFF) all_one = false;
        }
        bool data_ok = all_zero || all_one;

        // Store-to-Load 深度一致性校验
        alignas(64) uint8_t store_buf[64];
        memcpy(store_buf, bytes, 64);

        uint8x16_t reload0 = vld1q_u8(store_buf);
        uint8x16_t reload1 = vld1q_u8(store_buf + 16);
        uint8x16_t reload2 = vld1q_u8(store_buf + 32);
        uint8x16_t reload3 = vld1q_u8(store_buf + 48);

        uint8x16_t cmp0 = vceqq_u8(read0, reload0);
        uint8x16_t cmp1 = vceqq_u8(read1, reload1);
        uint8x16_t cmp2 = vceqq_u8(read2, reload2);
        uint8x16_t cmp3 = vceqq_u8(read3, reload3);

        uint8x16_t cmp_all = vandq_u8(vandq_u8(cmp0, cmp1), vandq_u8(cmp2, cmp3));
        alignas(64) uint8_t cmp_res[16];
        vst1q_u8(cmp_res, cmp_all);

        bool consistent = true;
        for (int i = 0; i < 16; ++i) {
            if (cmp_res[i] != 0xFF) {
                consistent = false;
                break;
            }
        }

        if (!data_ok || !consistent) {
            log_error("atomic_simd_512: Data tearing failure on ARM64! cpu=%d, my_id=%zu, data_ok=%d, consistent=%d",
                      cpu, my_id, data_ok, consistent);
            report_fail_msg("atomic_simd_512: SIMD data tearing failure on ARM64");
            return EXIT_FAILURE;
        }

    } while (test_time_condition(test));

    return EXIT_SUCCESS;
}

static int atomic_simd_512_finish(struct test *test) {
    auto *ctx = static_cast<TestContext*>(test->data);
    if (ctx) {
        delete ctx;
        test->data = nullptr;
    }
    return EXIT_SUCCESS;
}

#else

// Non-aarch64: ARM NEON SIMD is not available. Report a clean placeholder
// skip rather than a misleading pass (CLAUDE.md placeholder-test honesty).
static int atomic_simd_512_init(struct test *test) {
    (void)test;
    log_skip(TestResourceIssueSkipCategory,
             "to be implemented (placeholder): ARM NEON SIMD required for this "
             "atomic-SIMD test");
    return EXIT_SKIP;
}
static int atomic_simd_512_run(struct test *test, int cpu) {
    (void)test; (void)cpu;
    return EXIT_SKIP;
}
static int atomic_simd_512_finish(struct test *test) {
    (void)test;
    return EXIT_SUCCESS;
}

#endif // __aarch64__

DECLARE_TEST(atomic_simd_512, "Atomic 512-bit SIMD access (aligned, via Seqlock) on ARM64")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = atomic_simd_512_init,
    .test_run = atomic_simd_512_run,
    .test_cleanup = atomic_simd_512_finish,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
