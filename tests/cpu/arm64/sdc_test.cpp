#include "sandstone.h"

#ifdef __aarch64__
#include <arm_acle.h>
#include <cstdint>
#include <cstring>
#endif

#define SDC_TEST_BUFFER_SIZE (4 * 1024)

enum {
    SDC_METHOD_HW_ECC = 0,
    SDC_METHOD_CRC32 = 1,
    SDC_METHOD_CRC64 = 2,
    SDC_METHOD_CHECKSUM = 3
};

#ifdef __aarch64__
extern "C" {
int arm64_sdc_init(void);
int arm64_sdc_detect(int method);
int arm64_sdc_detect_via_crc32(const void *data, size_t len, uint32_t *crc_out);
int arm64_sdc_detect_via_crc64(const void *data, size_t len, uint64_t *crc_out);
int arm64_sdc_detect_via_checksum(const void *data, size_t len, uint32_t *checksum_out);
void arm64_sdc_set_config(bool enable_ecc, bool enable_software,
                          uint32_t ce_thresh, uint32_t ue_thresh,
                          bool enable_injection);
}
#endif

struct sdc_test_data {
    uint8_t *test_buffer;
    uint32_t golden_crc32;
    uint64_t golden_crc64;
    uint32_t golden_checksum;
};

static int sdc_test_init(struct test *test)
{
#ifdef __aarch64__
    sdc_test_data *data = static_cast<sdc_test_data *>(malloc(sizeof(sdc_test_data)));
    if (!data) {
        return EXIT_FAILURE;
    }

    data->test_buffer = static_cast<uint8_t *>(aligned_alloc(64, SDC_TEST_BUFFER_SIZE));
    if (!data->test_buffer) {
        free(data);
        return EXIT_FAILURE;
    }

    memset_random(data->test_buffer, SDC_TEST_BUFFER_SIZE);

    arm64_sdc_set_config(false, true, 0, 0, false);
    int ret = arm64_sdc_init();
    if (ret != 0) {
        free(data->test_buffer);
        free(data);
        return EXIT_FAILURE;
    }

    uint32_t crc32_result = 0;
    uint64_t crc64_result = 0;
    uint32_t checksum_result = 0;

    arm64_sdc_detect_via_crc32(data->test_buffer, SDC_TEST_BUFFER_SIZE, &crc32_result);
    arm64_sdc_detect_via_crc64(data->test_buffer, SDC_TEST_BUFFER_SIZE, &crc64_result);
    arm64_sdc_detect_via_checksum(data->test_buffer, SDC_TEST_BUFFER_SIZE, &checksum_result);

    data->golden_crc32 = crc32_result;
    data->golden_crc64 = crc64_result;
    data->golden_checksum = checksum_result;

    test->data = data;

    return EXIT_SUCCESS;
#else
    return EXIT_SUCCESS;
#endif
}

static int sdc_test_run(struct test *test, int cpu)
{
#ifdef __aarch64__
    sdc_test_data *data = static_cast<sdc_test_data *>(test->data);

    uint8_t *test_buffer = static_cast<uint8_t *>(aligned_alloc(64, SDC_TEST_BUFFER_SIZE));
    if (!test_buffer) {
        return EXIT_FAILURE;
    }

    memcpy(test_buffer, data->test_buffer, SDC_TEST_BUFFER_SIZE);

    uint32_t crc32_result = 0;
    uint64_t crc64_result = 0;
    uint32_t checksum_result = 0;

    // Recompute the three checksums over the (unmodified) copy and compare
    // against the golden values captured in init. Call the _via_ helpers
    // directly — the same path init used to compute the goldens — so init and
    // run are consistent (the old arm64_sdc_detect() gate here could return
    // non-zero in the forked test child and skip the recompute, leaving the
    // result at 0 and spuriously mismatching the golden).
    if (arm64_sdc_detect_via_crc32(test_buffer, SDC_TEST_BUFFER_SIZE, &crc32_result) == 0)
        memcmp_or_fail(&crc32_result, &data->golden_crc32, 1,
                "CRC32 checksum mismatch - data corruption detected");
    if (arm64_sdc_detect_via_crc64(test_buffer, SDC_TEST_BUFFER_SIZE, &crc64_result) == 0)
        memcmp_or_fail(&crc64_result, &data->golden_crc64, 1,
                "CRC64 checksum mismatch - data corruption detected");
    if (arm64_sdc_detect_via_checksum(test_buffer, SDC_TEST_BUFFER_SIZE, &checksum_result) == 0)
        memcmp_or_fail(&checksum_result, &data->golden_checksum, 1,
                "Checksum mismatch - data corruption detected");

    // Inject a single bit at the buffer midpoint and verify ALL three
    // detectors catch it. A silent-data-corruption *detector* test that
    // reports green when its own injected fault went unnoticed is a
    // false-negative bug — so on a miss we fail the test, not log-and-pass.
    size_t flip_position = (SDC_TEST_BUFFER_SIZE / 2);
    test_buffer[flip_position] ^= 0x01;

    bool error_detected = false;

    arm64_sdc_detect_via_crc32(test_buffer, SDC_TEST_BUFFER_SIZE, &crc32_result);
    if (crc32_result != data->golden_crc32)
        error_detected = true;

    arm64_sdc_detect_via_crc64(test_buffer, SDC_TEST_BUFFER_SIZE, &crc64_result);
    if (crc64_result != data->golden_crc64)
        error_detected = true;

    arm64_sdc_detect_via_checksum(test_buffer, SDC_TEST_BUFFER_SIZE, &checksum_result);
    if (checksum_result != data->golden_checksum)
        error_detected = true;

    free(test_buffer);

    if (!error_detected) {
        // The injected 1-bit corruption was NOT caught by any detector —
        // fail the test rather than silently passing (CLAUDE.md: a no-op
        // returning success is a bug).
        report_fail(test);
    }

    return EXIT_SUCCESS;
#else
    return EXIT_SUCCESS;
#endif
}

static int sdc_test_cleanup(struct test *test)
{
#ifdef __aarch64__
    sdc_test_data *data = static_cast<sdc_test_data *>(test->data);

    if (data) {
        if (data->test_buffer) {
            memset(data->test_buffer, 0, SDC_TEST_BUFFER_SIZE);
            free(data->test_buffer);
        }
        free(data);
    }
#endif

    return EXIT_SUCCESS;
}

DECLARE_TEST(arm64_sdc, "Test ARM64 Silent Data Corruption (SDC) detection using CRC32/CRC64 instructions")
    .test_init = sdc_test_init,
    .test_run = sdc_test_run,
    .test_cleanup = sdc_test_cleanup,
    .quality_level = TEST_QUALITY_BETA,
END_DECLARE_TEST
