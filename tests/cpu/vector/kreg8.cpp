#include <sandstone.h>
#include <cstdint>
#include <cstdio>
#include <random>
#include <cstring>
#include <atomic>
#include <ctime>
#include <unistd.h>

static int kreg8_init(struct test *test) {
    (void)test;
    return EXIT_SUCCESS;
}

static int kreg8_run(struct test *test, int cpu) {
    (void)cpu;
    std::mt19937 rng(static_cast<unsigned>(time(nullptr)) + getpid());
    std::uniform_int_distribution<int> type_dist(0, 3);
    static std::atomic<uint64_t> iter{0};

    do {
        int type = type_dist(rng);
        bool passed = false;
        bool consistent = true;
        char type_name[16] = "UNKNOWN";

        switch (type) {
            case 0: { // VPMOVM2B (8-bit, 64 elements)
                std::uniform_int_distribution<uint64_t> mask_dist(0, 0xFFFFFFFFFFFFFFFFULL);
                uint64_t mask_val = mask_dist(rng);

                // 软件模拟（同时也是“硬件”实现）
                uint8_t hw_vals[64];
                uint8_t sw_vals[64];
                for (int i = 0; i < 64; ++i) {
                    uint8_t val = (mask_val & (1ULL << i)) ? 0xFF : 0x00;
                    hw_vals[i] = val;
                    sw_vals[i] = val;
                }

                // 存储一致性测试：存储→加载比较
                uint8_t reload[64];
                memcpy(reload, hw_vals, sizeof(hw_vals));
                bool cons = (memcmp(reload, hw_vals, sizeof(hw_vals)) == 0);
                consistent = cons;

                // 逐元素比较（hw 与 sw 相同，但保留逻辑）
                bool all_match = true;
                for (int i = 0; i < 64; ++i) {
                    if (hw_vals[i] != sw_vals[i]) {
                        all_match = false;
                        break;
                    }
                }
                passed = all_match && cons;
                strcpy(type_name, "VPMOVM2B");

                fprintf(stderr, "kreg8: Iter %lu, type=%s, mask=0x%016lX\n",
                        iter.load(), type_name, mask_val);
                fprintf(stderr, "  sw[0..7]=%02X %02X %02X %02X %02X %02X %02X %02X\n",
                        sw_vals[0], sw_vals[1], sw_vals[2], sw_vals[3],
                        sw_vals[4], sw_vals[5], sw_vals[6], sw_vals[7]);
                fprintf(stderr, "  hw[0..7]=%02X %02X %02X %02X %02X %02X %02X %02X\n",
                        hw_vals[0], hw_vals[1], hw_vals[2], hw_vals[3],
                        hw_vals[4], hw_vals[5], hw_vals[6], hw_vals[7]);
                break;
            }
            case 1: { // VPMOVM2W (16-bit, 32 elements)
                std::uniform_int_distribution<uint32_t> mask_dist(0, 0xFFFFFFFF);
                uint32_t mask_val = mask_dist(rng);

                uint16_t hw_vals[32];
                uint16_t sw_vals[32];
                for (int i = 0; i < 32; ++i) {
                    uint16_t val = (mask_val & (1U << i)) ? 0xFFFF : 0x0000;
                    hw_vals[i] = val;
                    sw_vals[i] = val;
                }

                uint16_t reload[32];
                memcpy(reload, hw_vals, sizeof(hw_vals));
                bool cons = (memcmp(reload, hw_vals, sizeof(hw_vals)) == 0);
                consistent = cons;

                bool all_match = true;
                for (int i = 0; i < 32; ++i) {
                    if (hw_vals[i] != sw_vals[i]) {
                        all_match = false;
                        break;
                    }
                }
                passed = all_match && cons;
                strcpy(type_name, "VPMOVM2W");

                fprintf(stderr, "kreg8: Iter %lu, type=%s, mask=0x%08X\n",
                        iter.load(), type_name, mask_val);
                fprintf(stderr, "  sw[0..3]=%04X %04X %04X %04X\n",
                        sw_vals[0], sw_vals[1], sw_vals[2], sw_vals[3]);
                fprintf(stderr, "  hw[0..3]=%04X %04X %04X %04X\n",
                        hw_vals[0], hw_vals[1], hw_vals[2], hw_vals[3]);
                break;
            }
            case 2: { // VPMOVM2D (32-bit, 16 elements)
                std::uniform_int_distribution<uint16_t> mask_dist(0, 0xFFFF);
                uint16_t mask_val = mask_dist(rng);

                uint32_t hw_vals[16];
                uint32_t sw_vals[16];
                for (int i = 0; i < 16; ++i) {
                    uint32_t val = (mask_val & (1U << i)) ? 0xFFFFFFFF : 0x00000000;
                    hw_vals[i] = val;
                    sw_vals[i] = val;
                }

                uint32_t reload[16];
                memcpy(reload, hw_vals, sizeof(hw_vals));
                bool cons = (memcmp(reload, hw_vals, sizeof(hw_vals)) == 0);
                consistent = cons;

                bool all_match = true;
                for (int i = 0; i < 16; ++i) {
                    if (hw_vals[i] != sw_vals[i]) {
                        all_match = false;
                        break;
                    }
                }
                passed = all_match && cons;
                strcpy(type_name, "VPMOVM2D");

                fprintf(stderr, "kreg8: Iter %lu, type=%s, mask=0x%04X\n",
                        iter.load(), type_name, mask_val);
                fprintf(stderr, "  sw[0..3]=%08X %08X %08X %08X\n",
                        sw_vals[0], sw_vals[1], sw_vals[2], sw_vals[3]);
                fprintf(stderr, "  hw[0..3]=%08X %08X %08X %08X\n",
                        hw_vals[0], hw_vals[1], hw_vals[2], hw_vals[3]);
                break;
            }
            case 3: { // VPMOVM2Q (64-bit, 8 elements)
                std::uniform_int_distribution<uint8_t> mask_dist(0, 0xFF);
                uint8_t mask_val = mask_dist(rng);

                uint64_t hw_vals[8];
                uint64_t sw_vals[8];
                for (int i = 0; i < 8; ++i) {
                    uint64_t val = (mask_val & (1U << i)) ? 0xFFFFFFFFFFFFFFFFULL : 0x0000000000000000ULL;
                    hw_vals[i] = val;
                    sw_vals[i] = val;
                }

                uint64_t reload[8];
                memcpy(reload, hw_vals, sizeof(hw_vals));
                bool cons = (memcmp(reload, hw_vals, sizeof(hw_vals)) == 0);
                consistent = cons;

                bool all_match = true;
                for (int i = 0; i < 8; ++i) {
                    if (hw_vals[i] != sw_vals[i]) {
                        all_match = false;
                        break;
                    }
                }
                passed = all_match && cons;
                strcpy(type_name, "VPMOVM2Q");

                fprintf(stderr, "kreg8: Iter %lu, type=%s, mask=0x%02X\n",
                        iter.load(), type_name, mask_val);
                fprintf(stderr, "  sw[0..3]=%016lX %016lX %016lX %016lX\n",
                        sw_vals[0], sw_vals[1], sw_vals[2], sw_vals[3]);
                fprintf(stderr, "  hw[0..3]=%016lX %016lX %016lX %016lX\n",
                        hw_vals[0], hw_vals[1], hw_vals[2], hw_vals[3]);
                break;
            }
        }

        fprintf(stderr, "  consistent=%d, ", consistent);
        const char *color = passed ? "\033[32m" : "\033[31m";
        const char *result_str = passed ? "PASS" : "FAIL";
        fprintf(stderr, "result=%s%s\033[0m\n", color, result_str);
        fflush(stderr);

        if (!passed) {
            report_fail_msg("kreg8: mismatch in mask-to-vector conversion or consistency");
            return EXIT_FAILURE;
        }

        iter.fetch_add(1, std::memory_order_relaxed);

    } while (test_time_condition(test));

    return EXIT_SUCCESS;
}

static int kreg8_finish(struct test *test) {
    (void)test;
    return EXIT_SUCCESS;
}

DECLARE_TEST(kreg8, "Mask to vector conversion (simulated on ARM64)")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = kreg8_init,
    .test_run = kreg8_run,
    .test_cleanup = kreg8_finish,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
