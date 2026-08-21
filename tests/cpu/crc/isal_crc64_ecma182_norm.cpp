#include <sandstone.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>
#include <isa-l/crc.h>

// 声明 ISA-L 提供的函数（已在 isa-l/crc.h 中声明，此处冗余但无害）
extern "C" {
    uint64_t crc64_ecma_norm(uint64_t init, const unsigned char *buf, uint64_t len);
}

static constexpr size_t BLOCK_SIZE = 1024;

static int isal_crc64_ecma182_norm_init(struct test *test) {
    (void)test;
    return EXIT_SUCCESS;
}

static int isal_crc64_ecma182_norm_run(struct test *test, int cpu) {
    (void)cpu;
    std::vector<uint8_t> local_data(BLOCK_SIZE);
    std::mt19937 rng(std::random_device{}());
    std::uniform_int_distribution<uint8_t> byte_dist(0, 255);

    do {
        for (size_t i = 0; i < BLOCK_SIZE; ++i) {
            local_data[i] = byte_dist(rng);
        }

        uint64_t crc1 = crc64_ecma_norm(0, local_data.data(), BLOCK_SIZE);

        // ARM64 兼容的内存屏障（等效于 mfence）
        __sync_synchronize();

        uint64_t crc2 = crc64_ecma_norm(0, local_data.data(), BLOCK_SIZE);

        bool data_ok = (crc1 == crc2);

        uint64_t store_buf = crc1;
        uint64_t reload_buf;
        memcpy(&reload_buf, &store_buf, sizeof(store_buf));
        bool consistent = (reload_buf == crc1);

        bool passed = data_ok && consistent;

        if (!passed) {
            report_fail_msg("isal_crc64_ecma182_norm: CRC mismatch or consistency failure");
            return EXIT_FAILURE;
        }

    } while (test_time_condition(test));

    return EXIT_SUCCESS;
}

static int isal_crc64_ecma182_norm_finish(struct test *test) {
    (void)test;
    return EXIT_SUCCESS;
}

DECLARE_TEST(isal_crc64_ecma182_norm, "ISA-L CRC64 ECMA-182 normal (random data)")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = isal_crc64_ecma182_norm_init,
    .test_run = isal_crc64_ecma182_norm_run,
    .test_cleanup = isal_crc64_ecma182_norm_finish,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
