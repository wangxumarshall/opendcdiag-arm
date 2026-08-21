#include <sandstone.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>
#include <isa-l/crc.h>

// 根据 ISA-L 头文件声明，crc16_t10dif 的签名通常为：
// uint16_t crc16_t10dif(uint16_t init, const unsigned char *buf, uint64_t len)
extern "C" {
    uint16_t crc16_t10dif(uint16_t init, const unsigned char *buf, uint64_t len);
}

static constexpr size_t BLOCK_SIZE = 1024;

static int isal_crc_t10dif_init(struct test *test) {
    (void)test;
    return EXIT_SUCCESS;
}

static int isal_crc_t10dif_run(struct test *test, int cpu) {
    (void)cpu;
    std::vector<uint8_t> local_data(BLOCK_SIZE);
    std::mt19937 rng(std::random_device{}());
    std::uniform_int_distribution<uint8_t> byte_dist(0, 255);

    do {
        for (size_t i = 0; i < BLOCK_SIZE; ++i) {
            local_data[i] = byte_dist(rng);
        }

        // 第一次 CRC 计算（初始值 0）
        uint16_t crc1 = crc16_t10dif(0, local_data.data(), BLOCK_SIZE);

        // ARM64 兼容的内存屏障
        __sync_synchronize();

        // 第二次 CRC 计算
        uint16_t crc2 = crc16_t10dif(0, local_data.data(), BLOCK_SIZE);

        bool data_ok = (crc1 == crc2);

        // 一致性测试：存储 CRC 值到内存再加载比较
        uint16_t store_buf = crc1;
        uint16_t reload_buf;
        memcpy(&reload_buf, &store_buf, sizeof(store_buf));
        bool consistent = (reload_buf == crc1);

        bool passed = data_ok && consistent;

        if (!passed) {
            report_fail_msg("isal_crc_t10dif: CRC mismatch or consistency failure");
            return EXIT_FAILURE;
        }

    } while (test_time_condition(test));

    return EXIT_SUCCESS;
}

static int isal_crc_t10dif_finish(struct test *test) {
    (void)test;
    return EXIT_SUCCESS;
}

DECLARE_TEST(isal_crc_t10dif, "ISA-L CRC16 T10 DIF (random data)")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = isal_crc_t10dif_init,
    .test_run = isal_crc_t10dif_run,
    .test_cleanup = isal_crc_t10dif_finish,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
