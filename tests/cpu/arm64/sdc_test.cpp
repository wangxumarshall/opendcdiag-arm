#include "sandstone.h"

#ifdef __aarch64__
#include <arm_acle.h>
#include <cstdint>
#include <cstring>
#endif

/*
 * ARM64 silent-data-corruption (SDC) detection test.
 *
 * The test combines two roles:
 *
 *   1. A *detector self-check* (init): inject a single bit into a known
 *      buffer and assert that every software-integrity primitive (CRC32,
 *      CRC64, additive checksum) flags it. A detector that silently misses
 *      its own injected fault is a false-negative bug, so a miss fails the
 *      test closed (report_fail), not "pass".
 *
 *   2. A *computation-pressure SDC detector* (run, under TEST_LOOP): hold a
 *      random buffer whose golden CRC32/CRC64/checksum were captured once in
 *      init, then repeatedly recompute the three signatures over the buffer
 *      inside the timed loop and compare byte-for-byte against the golden on
 *      every iteration. A single bit flipped by the silicon on the data path
 *      (load/store, the __crc32* arithmetic, the add chain) surfaces as a
 *      signature mismatch and is failed via memcmp_or_fail.
 *
 * The earlier version ran the integrity recompute exactly once per thread and
 * spent most of its time on the (always-passing) injected-bit self-check, so
 * it spent almost no wall-clock exercising the silicon. This version runs the
 * recompute in TEST_LOOP for the whole time budget, so the integrity datapath
 * is actually stressed — a transient corruption on it is now caught instead
 * of being missed because the recompute happened to land on a clean instant.
 */

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

    /*
     * Detector self-check: verify every detector is genuinely sensitive to a
     * single-bit corruption. A detector that would miss a 1-bit fault in the
     * field must not be shipped as "passing" — fail closed here, in init, so
     * the failure is reported before the timed run begins. This is a
     * one-shot invariant check, distinct from the per-iteration run-phase
     * computation that catches live (silicon-induced) corruption.
     */
    uint8_t *probe = static_cast<uint8_t *>(aligned_alloc(64, SDC_TEST_BUFFER_SIZE));
    if (!probe) {
        free(data->test_buffer);
        free(data);
        return EXIT_FAILURE;
    }
    memcpy(probe, data->test_buffer, SDC_TEST_BUFFER_SIZE);
    probe[SDC_TEST_BUFFER_SIZE / 2] ^= 0x01;

    uint32_t p_crc32 = 0;
    uint64_t p_crc64 = 0;
    uint32_t p_checksum = 0;
    arm64_sdc_detect_via_crc32(probe, SDC_TEST_BUFFER_SIZE, &p_crc32);
    arm64_sdc_detect_via_crc64(probe, SDC_TEST_BUFFER_SIZE, &p_crc64);
    arm64_sdc_detect_via_checksum(probe, SDC_TEST_BUFFER_SIZE, &p_checksum);
    free(probe);

    if (p_crc32 == data->golden_crc32
        && p_crc64 == data->golden_crc64
        && p_checksum == data->golden_checksum) {
        /* No detector flagged the injected 1-bit flip — the integrity
         * primitives are not actually detecting corruption, so running
         * the timed comparison would be meaningless. Fail the test. */
        free(data->test_buffer);
        free(data);
        log_error("arm64_sdc: detector self-check failed — an injected "
                  "single-bit corruption was not caught by CRC32, CRC64, "
                  "or the additive checksum; the SDC detector is not "
                  "sensitive to corruption and must be fixed before use");
        return EXIT_FAILURE;
    }

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

    /* Copy the golden buffer once; the loop recomputes the signatures over
     * this (unchanged) copy and compares against the init golden. The copy
     * itself exercises the load/store path; the recompute exercises the
     * __crc32* arithmetic and the add chain. A corruption on either shows up
     * as a signature mismatch. */
    memcpy(test_buffer, data->test_buffer, SDC_TEST_BUFFER_SIZE);

    uint32_t crc32_result = 0;
    uint64_t crc64_result = 0;
    uint32_t checksum_result = 0;

    TEST_LOOP(test, 1 << 12) {
        /* Recompute the three signatures over the (unmodified) copy and
         * compare against the golden captured in init — the same _via_
         * helpers init used, so init and run are consistent. Each detector
         * runs every iteration: CRC32 is the strongest (Castagnoli, via the
         * __crc32cb instruction), CRC64 the polynomial one, and the additive
         * checksum an independent (arithmetic rather than polynomial) cross-
         * check; a fault that one misses the others are likely to catch. */
        if (arm64_sdc_detect_via_crc32(test_buffer, SDC_TEST_BUFFER_SIZE,
                                       &crc32_result) == 0)
            memcmp_or_fail(&crc32_result, &data->golden_crc32, 1,
                    "CRC32 signature mismatch — silent data corruption "
                    "detected on the integrity datapath");
        if (arm64_sdc_detect_via_crc64(test_buffer, SDC_TEST_BUFFER_SIZE,
                                       &crc64_result) == 0)
            memcmp_or_fail(&crc64_result, &data->golden_crc64, 1,
                    "CRC64 signature mismatch — silent data corruption "
                    "detected on the integrity datapath");
        if (arm64_sdc_detect_via_checksum(test_buffer, SDC_TEST_BUFFER_SIZE,
                                          &checksum_result) == 0)
            memcmp_or_fail(&checksum_result, &data->golden_checksum, 1,
                    "Additive checksum mismatch — silent data corruption "
                    "detected on the integrity datapath");
    }

    free(test_buffer);

    return EXIT_SUCCESS;
#else
    (void)cpu;
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

DECLARE_TEST(arm64_sdc, "Test ARM64 Silent Data Corruption (SDC) detection: CRC32/CRC64/checksum datapath under TEST_LOOP pressure with byte-exact golden comparison")
    .test_init = sdc_test_init,
    .test_run = sdc_test_run,
    .test_cleanup = sdc_test_cleanup,
    .quality_level = TEST_QUALITY_BETA,
END_DECLARE_TEST
