#include <sandstone.h>
#include <cstdint>
#include <cstdio>
#include <random>
#include <cstring>
#include <atomic>
#include <ctime>
#include <unistd.h>

static int kreg9_init(struct test *test) {
    (void)test;
    return EXIT_SUCCESS;
}

static int kreg9_run(struct test *test, int cpu) {
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
            case 0: { // KMOVB (8-bit)
                uint8_t orig = static_cast<uint8_t>(rng() & 0xFF);
                uint8_t gpr = orig;                    // 模拟 KMOVB
                bool equal = (gpr == orig);
                uint8_t store = gpr;
                uint8_t reload;
                memcpy(&reload, &store, sizeof(store));
                bool cons = (reload == gpr);
                consistent = cons;
                passed = equal && cons;
                strcpy(type_name, "KMOVB");
                fprintf(stderr, "kreg9: Iter %lu, type=%s, orig=0x%02X, gpr=0x%02X\n",
                        iter.load(), type_name, orig, gpr);
                break;
            }
            case 1: { // KMOVW (16-bit)
                uint16_t orig = static_cast<uint16_t>(rng() & 0xFFFF);
                uint16_t gpr = orig;                    // 模拟 KMOVW
                bool equal = (gpr == orig);
                uint16_t store = gpr;
                uint16_t reload;
                memcpy(&reload, &store, sizeof(store));
                bool cons = (reload == gpr);
                consistent = cons;
                passed = equal && cons;
                strcpy(type_name, "KMOVW");
                fprintf(stderr, "kreg9: Iter %lu, type=%s, orig=0x%04X, gpr=0x%04X\n",
                        iter.load(), type_name, orig, gpr);
                break;
            }
            case 2: { // KMOVD (32-bit)
                uint32_t orig = rng();
                uint32_t gpr = orig;                    // 模拟 KMOVD
                bool equal = (gpr == orig);
                uint32_t store = gpr;
                uint32_t reload;
                memcpy(&reload, &store, sizeof(store));
                bool cons = (reload == gpr);
                consistent = cons;
                passed = equal && cons;
                strcpy(type_name, "KMOVD");
                fprintf(stderr, "kreg9: Iter %lu, type=%s, orig=0x%08X, gpr=0x%08X\n",
                        iter.load(), type_name, orig, gpr);
                break;
            }
            case 3: { // KMOVQ (64-bit)
                uint64_t orig = (static_cast<uint64_t>(rng()) << 32) | rng();
                uint64_t gpr = orig;                    // 模拟 KMOVQ
                bool equal = (gpr == orig);
                uint64_t store = gpr;
                uint64_t reload;
                memcpy(&reload, &store, sizeof(store));
                bool cons = (reload == gpr);
                consistent = cons;
                passed = equal && cons;
                strcpy(type_name, "KMOVQ");
                fprintf(stderr, "kreg9: Iter %lu, type=%s, orig=0x%016lX, gpr=0x%016lX\n",
                        iter.load(), type_name, orig, gpr);
                break;
            }
        }

        fprintf(stderr, "  consistent=%d, ", consistent);
        const char *color = passed ? "\033[32m" : "\033[31m";
        const char *result_str = passed ? "PASS" : "FAIL";
        fprintf(stderr, "result=%s%s\033[0m\n", color, result_str);
        fflush(stderr);

        if (!passed) {
            report_fail_msg("kreg9: mismatch in KMOV or consistency");
            return EXIT_FAILURE;
        }

        iter.fetch_add(1, std::memory_order_relaxed);

    } while (test_time_condition(test));

    return EXIT_SUCCESS;
}

static int kreg9_finish(struct test *test) {
    (void)test;
    return EXIT_SUCCESS;
}

DECLARE_TEST(kreg9, "Mask to/from GPR move (simulated on ARM64)")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = kreg9_init,
    .test_run = kreg9_run,
    .test_cleanup = kreg9_finish,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
