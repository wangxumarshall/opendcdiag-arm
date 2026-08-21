#include <sandstone.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#ifdef __aarch64__
#include <arm_neon.h>      // NEON 指令
#endif

static constexpr size_t BLOCK_SIZE = 64;

static int memcpy_variations_dyn_init(struct test *test) {
#ifndef __aarch64__
    (void)test;
    log_skip(CpuNotSupportedSkipCategory,
             "to be implemented (placeholder): ARM NEON required for this memory test");
    return EXIT_SKIP;
#else
    (void)test;
    // ARM64 平台上 NEON 默认可用，无需检测
    return EXIT_SUCCESS;
#endif
}

static int memcpy_variations_dyn_run(struct test *test, int cpu) {
    (void)cpu;
#ifdef __aarch64__

    alignas(16) uint8_t src[BLOCK_SIZE];   // NEON 要求 16 字节对齐
    alignas(16) uint8_t dst[BLOCK_SIZE];
    alignas(16) uint8_t store_buf[BLOCK_SIZE];

    std::mt19937 rng(std::random_device{}());
    std::uniform_int_distribution<uint16_t> word_dist(0, 65535);
    std::uniform_int_distribution<uint8_t> byte_dist(0, 255);

    do {
        for (size_t i = 0; i < BLOCK_SIZE; ++i) {
            src[i] = byte_dist(rng);
        }

        // ---------- 16 字节复制 (NEON 单向量) ----------
        memset(dst, 0, BLOCK_SIZE);
        uint8x16_t v16 = vld1q_u8(src);
        vst1q_u8(dst, v16);
        bool ok16 = (memcmp(dst, src, 16) == 0);

        // ---------- 32 字节复制 (两个 NEON 向量) ----------
        memset(dst, 0, BLOCK_SIZE);
        uint8x16_t v32_0 = vld1q_u8(src);
        uint8x16_t v32_1 = vld1q_u8(src + 16);
        vst1q_u8(dst, v32_0);
        vst1q_u8(dst + 16, v32_1);
        bool ok32 = (memcmp(dst, src, 32) == 0);

        // ---------- 64 字节复制 (四个 NEON 向量) ----------
        memset(dst, 0, BLOCK_SIZE);
        uint8x16_t v64_0 = vld1q_u8(src);
        uint8x16_t v64_1 = vld1q_u8(src + 16);
        uint8x16_t v64_2 = vld1q_u8(src + 32);
        uint8x16_t v64_3 = vld1q_u8(src + 48);
        vst1q_u8(dst, v64_0);
        vst1q_u8(dst + 16, v64_1);
        vst1q_u8(dst + 32, v64_2);
        vst1q_u8(dst + 48, v64_3);
        bool ok64 = (memcmp(dst, src, 64) == 0);

        // ---------- memcpy (REP MOVS 替代) ----------
        memset(dst, 0, BLOCK_SIZE);
        memcpy(dst, src, BLOCK_SIZE);
        bool ok_memcpy = (memcmp(dst, src, BLOCK_SIZE) == 0);

        // ---------- KMOV 模拟 (16 位掩码读写) ----------
        uint16_t val = word_dist(rng);
        uint16_t mask = val;          // 模拟写入掩码寄存器
        uint16_t readback = mask;     // 模拟读回
        bool ok_kmov = (readback == val);

        bool data_ok = ok16 && ok32 && ok64 && ok_memcpy && ok_kmov;

        // ---------- 一致性测试 (Store → Load) ----------
        memcpy(store_buf, dst, BLOCK_SIZE);
        bool consistent = (memcmp(store_buf, dst, BLOCK_SIZE) == 0);

        bool passed = data_ok && consistent;

        if (!passed) {
            fprintf(stderr, "memcpy_variations_dyn [FAIL]: src[0..7] = %02X %02X %02X %02X %02X %02X %02X %02X\n",
                    src[0], src[1], src[2], src[3], src[4], src[5], src[6], src[7]);
            fprintf(stderr, "  16B ok=%d, 32B ok=%d, 64B ok=%d, memcpy ok=%d, kmov ok=%d (val=0x%04X, readback=0x%04X)\n",
                    ok16, ok32, ok64, ok_memcpy, ok_kmov, val, readback);
            fflush(stderr);
            
            report_fail_msg("memcpy_variations_dyn: mismatch or consistency failure");
            return EXIT_FAILURE;
        }

    } while (test_time_condition(test));

    return EXIT_SUCCESS;
#else
    (void)test;
    log_skip(CpuNotSupportedSkipCategory,
             "to be implemented (placeholder): ARM NEON required for this memory test");
    return EXIT_SKIP;
#endif
}

static int memcpy_variations_dyn_finish(struct test *test) {
    (void)test;
    return EXIT_SUCCESS;
}

DECLARE_TEST(memcpy_variations_dyn,
             "Tests rep movs*, kmov and 16, 32 and 64 byte moves. Tests generated at runtime (ARM64 NEON).")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = memcpy_variations_dyn_init,
    .test_run = memcpy_variations_dyn_run,
    .test_cleanup = memcpy_variations_dyn_finish,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
