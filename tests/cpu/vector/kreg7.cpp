#include <sandstone.h>
#include <cstdint>
#include <cstdio>
#include <random>
#include <cstring>
#include <atomic>
#include <ctime>
#include <unistd.h>

#ifdef __aarch64__
#include <arm_neon.h>

// -------------------- 辅助函数：生成随机数据 --------------------
static void fill_random_byte(uint8_t *buf, size_t n, std::mt19937 &rng) {
    std::uniform_int_distribution<uint8_t> dist(0, 255);
    for (size_t i = 0; i < n; ++i) buf[i] = dist(rng);
}
static void fill_random_word(uint16_t *buf, size_t n, std::mt19937 &rng) {
    std::uniform_int_distribution<uint16_t> dist(0, 0xFFFF);
    for (size_t i = 0; i < n; ++i) buf[i] = dist(rng);
}
static void fill_random_dword(uint32_t *buf, size_t n, std::mt19937 &rng) {
    std::uniform_int_distribution<uint32_t> dist(0, 0xFFFFFFFF);
    for (size_t i = 0; i < n; ++i) buf[i] = dist(rng);
}
static void fill_random_qword(uint64_t *buf, size_t n, std::mt19937 &rng) {
    std::uniform_int_distribution<uint64_t> dist(0, 0xFFFFFFFFFFFFFFFFULL);
    for (size_t i = 0; i < n; ++i) buf[i] = dist(rng);
}

// -------------------- 软件参考掩码提取 --------------------
static uint64_t sw_movepi8_mask(const uint8_t *data) {
    uint64_t mask = 0;
    for (int i = 0; i < 64; ++i) {
        if (data[i] & 0x80) mask |= (1ULL << i);
    }
    return mask;
}
static uint32_t sw_movepi16_mask(const uint16_t *data) {
    uint32_t mask = 0;
    for (int i = 0; i < 32; ++i) {
        if (data[i] & 0x8000) mask |= (1U << i);
    }
    return mask;
}
static uint16_t sw_movepi32_mask(const uint32_t *data) {
    uint16_t mask = 0;
    for (int i = 0; i < 16; ++i) {
        if (data[i] & 0x80000000) mask |= (1U << i);
    }
    return mask;
}
static uint8_t sw_movepi64_mask(const uint64_t *data) {
    uint8_t mask = 0;
    for (int i = 0; i < 8; ++i) {
        if (data[i] & 0x8000000000000000ULL) mask |= (1U << i);
    }
    return mask;
}

// -------------------- ARM NEON 硬件掩码提取（使用 vgetq_lane 逐位提取，避免类型转换问题） --------------------
static uint64_t neon_movepi8_mask(const uint8_t *data) {
    uint64_t mask = 0;
    // 一次处理 16 个字节，共 4 组
    for (int block = 0; block < 4; ++block) {
        uint8x16_t vec = vld1q_u8(data + block * 16);
        for (int i = 0; i < 16; ++i) {
            uint8_t val = vgetq_lane_u8(vec, i);
            if (val & 0x80) {
                mask |= (1ULL << (block * 16 + i));
            }
        }
    }
    return mask;
}

static uint32_t neon_movepi16_mask(const uint16_t *data) {
    uint32_t mask = 0;
    // 一次处理 8 个 16-bit 元素，共 4 组
    for (int block = 0; block < 4; ++block) {
        uint16x8_t vec = vld1q_u16(data + block * 8);
        for (int i = 0; i < 8; ++i) {
            uint16_t val = vgetq_lane_u16(vec, i);
            if (val & 0x8000) {
                mask |= (1U << (block * 8 + i));
            }
        }
    }
    return mask;
}

static uint16_t neon_movepi32_mask(const uint32_t *data) {
    uint16_t mask = 0;
    // 一次处理 4 个 32-bit 元素，共 4 组
    for (int block = 0; block < 4; ++block) {
        uint32x4_t vec = vld1q_u32(data + block * 4);
        for (int i = 0; i < 4; ++i) {
            uint32_t val = vgetq_lane_u32(vec, i);
            if (val & 0x80000000) {
                mask |= (1U << (block * 4 + i));
            }
        }
    }
    return mask;
}

static uint8_t neon_movepi64_mask(const uint64_t *data) {
    uint8_t mask = 0;
    // 一次处理 2 个 64-bit 元素，共 4 组
    for (int block = 0; block < 4; ++block) {
        uint64x2_t vec = vld1q_u64(data + block * 2);
        for (int i = 0; i < 2; ++i) {
            uint64_t val = vgetq_lane_u64(vec, i);
            if (val & 0x8000000000000000ULL) {
                mask |= (1U << (block * 2 + i));
            }
        }
    }
    return mask;
}

// -------------------- 测试用例函数 --------------------
static int kreg7_init(struct test *test) {
    (void)test;
    return EXIT_SUCCESS;
}

static int kreg7_run(struct test *test, int cpu) {
    (void)cpu;
    std::mt19937 rng(static_cast<unsigned>(time(nullptr)) + getpid());
    std::uniform_int_distribution<int> type_dist(0, 3);
    static std::atomic<uint64_t> iter{0};

    do {
        int type = type_dist(rng);
        bool passed = false;
        bool consistent = true;
        uint64_t sw_mask = 0, hw_mask = 0;
        char type_name[16] = "UNKNOWN";

        switch (type) {
            case 0: { // VPMOVB2M (8-bit)
                alignas(16) uint8_t data[64];
                fill_random_byte(data, 64, rng);
                uint64_t hw = neon_movepi8_mask(data);
                uint64_t sw = sw_movepi8_mask(data);
                sw_mask = sw;
                hw_mask = hw;
                // 存储一致性：使用 NEON 存储再加载比较
                uint8_t reload[64];
                for (int i = 0; i < 4; ++i) {
                    uint8x16_t vec = vld1q_u8(data + i*16);
                    vst1q_u8(reload + i*16, vec);
                }
                bool cons = (memcmp(reload, data, sizeof(data)) == 0);
                consistent = cons;
                passed = (hw == sw) && cons;
                strcpy(type_name, "VPMOVB2M");
                fprintf(stderr, "kreg7: Iter %lu, type=%s, data[0..7]=%02X %02X %02X %02X %02X %02X %02X %02X\n",
                        iter.load(), type_name, data[0], data[1], data[2], data[3],
                        data[4], data[5], data[6], data[7]);
                break;
            }
            case 1: { // VPMOVW2M (16-bit)
                alignas(16) uint16_t data[32];
                fill_random_word(data, 32, rng);
                uint32_t hw = neon_movepi16_mask(data);
                uint32_t sw = sw_movepi16_mask(data);
                sw_mask = sw;
                hw_mask = hw;
                // 存储一致性
                uint16_t reload[32];
                for (int i = 0; i < 4; ++i) {
                    uint16x8_t vec = vld1q_u16(data + i*8);
                    vst1q_u16(reload + i*8, vec);
                }
                bool cons = (memcmp(reload, data, sizeof(data)) == 0);
                consistent = cons;
                passed = (hw == sw) && cons;
                strcpy(type_name, "VPMOVW2M");
                fprintf(stderr, "kreg7: Iter %lu, type=%s, data[0..3]=%04X %04X %04X %04X\n",
                        iter.load(), type_name, data[0], data[1], data[2], data[3]);
                break;
            }
            case 2: { // VPMOVD2M (32-bit)
                alignas(16) uint32_t data[16];
                fill_random_dword(data, 16, rng);
                uint16_t hw = neon_movepi32_mask(data);
                uint16_t sw = sw_movepi32_mask(data);
                sw_mask = sw;
                hw_mask = hw;
                // 存储一致性
                uint32_t reload[16];
                for (int i = 0; i < 4; ++i) {
                    uint32x4_t vec = vld1q_u32(data + i*4);
                    vst1q_u32(reload + i*4, vec);
                }
                bool cons = (memcmp(reload, data, sizeof(data)) == 0);
                consistent = cons;
                passed = (hw == sw) && cons;
                strcpy(type_name, "VPMOVD2M");
                fprintf(stderr, "kreg7: Iter %lu, type=%s, data[0..3]=%08X %08X %08X %08X\n",
                        iter.load(), type_name, data[0], data[1], data[2], data[3]);
                break;
            }
            case 3: { // VPMOVQ2M (64-bit)
                alignas(16) uint64_t data[8];
                fill_random_qword(data, 8, rng);
                uint8_t hw = neon_movepi64_mask(data);
                uint8_t sw = sw_movepi64_mask(data);
                sw_mask = sw;
                hw_mask = hw;
                // 存储一致性
                uint64_t reload[8];
                for (int i = 0; i < 4; ++i) {
                    uint64x2_t vec = vld1q_u64(data + i*2);
                    vst1q_u64(reload + i*2, vec);
                }
                bool cons = (memcmp(reload, data, sizeof(data)) == 0);
                consistent = cons;
                passed = (hw == sw) && cons;
                strcpy(type_name, "VPMOVQ2M");
                fprintf(stderr, "kreg7: Iter %lu, type=%s, data[0..3]=%016lX %016lX %016lX %016lX\n",
                        iter.load(), type_name, data[0], data[1], data[2], data[3]);
                break;
            }
        }

        fprintf(stderr, "  sw mask=0x%016lX, hw mask=0x%016lX\n", sw_mask, hw_mask);
        fprintf(stderr, "  consistent=%d, ", consistent);

        const char *color = passed ? "\033[32m" : "\033[31m";
        const char *result_str = passed ? "PASS" : "FAIL";
        fprintf(stderr, "result=%s%s\033[0m\n", color, result_str);
        fflush(stderr);

        if (!passed) {
            report_fail_msg("kreg7: mismatch in mask or consistency");
            return EXIT_FAILURE;
        }

        iter.fetch_add(1, std::memory_order_relaxed);

    } while (test_time_condition(test));

    return EXIT_SUCCESS;
}

static int kreg7_finish(struct test *test) {
    (void)test;
    return EXIT_SUCCESS;
}

#else // !__aarch64__

// On non-AArch64 (e.g. x86-64) there is no <arm_neon.h>; the test is kept
// listed and schedulable but reports a clean resource skip so the result is
// marked ignored rather than spuriously passing. The NEON port is the
// counterpart of the x86 AVX-512 VPMOV*2M sign-bit mask extraction tests.
static int kreg7_init(struct test *test) {
    (void)test;
    log_skip(TestResourceIssueSkipCategory,
             "to be implemented (placeholder): ARM NEON required for "
             "mask from vector sign bits (VPMOV*2M)");
    return EXIT_SKIP;
}

static int kreg7_run(struct test *test, int cpu) {
    (void)test;
    (void)cpu;
    __builtin_unreachable();    // init reports EXIT_SKIP, run never executes
}

static int kreg7_finish(struct test *test) {
    (void)test;
    return EXIT_SUCCESS;
}

#endif // __aarch64__

DECLARE_TEST(kreg7, "Mask from vector sign bits (simulated on ARM64)")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = kreg7_init,
    .test_run = kreg7_run,
    .test_cleanup = kreg7_finish,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
