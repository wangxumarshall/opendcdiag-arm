#include "sandstone.h"

#ifdef __aarch64__
#include <arm_neon.h>
#include <cstdlib>
#endif

/*
 * Native ARM AES (AESE/AESMC) SDC-detection test.
 *
 * Stresses the AES silicon datapath (the AESE/AESMC units) directly using the
 * ARMv8 cryptographic-extension instructions, then compares the ciphertext
 * byte-for-byte against a NIST golden value. Unlike a software AES that
 * exercises only the ALU/load-store pipes, this routes every block through the
 * dedicated AES round logic — the exact silicon whose silent corruption this
 * tool exists to catch.
 *
 * Gating (the ARM equivalent of x86 CPUID): the test is built only on aarch64
 * (the whole subdir is aarch64-guarded in meson) and declares
 * minimum_cpu = cpu_feature_aes, so the framework skips it on a part lacking
 * the AES extension rather than executing AESE on a CPU that traps it
 * (SIGILL). test_init additionally probe-checks device_has_feature() and skips
 * with an honest "to be implemented (placeholder)" reason if, for some
 * steering, the build flags admit the intrinsic but the runtime part lacks
 * the bit (defensive — the minimum_cpu gate already handles this).
 *
 * AES-128-ECB reference vector (FIPS-197 Appendix C.1):
 *   key        = 2b7e151628aed2a6abf7158809cf4f3c
 *   plaintext  = 6bc1bee22e409f96e93d7e117393172a
 *   ciphertext = 3ad77bb40d7a3660a89ecaf32466ef97   (verified against OpenSSL)
 */

#define CRYPTO_BLOCKS   (1u << 10)   /* 1024 AES blocks per iteration */
#define AES_BLOCK_SIZE   16

struct crypto_test_data {
    uint8_t aes_key[16];
    uint8_t aes_plaintext[CRYPTO_BLOCKS * AES_BLOCK_SIZE];
    uint8_t aes_ciphertext_golden[CRYPTO_BLOCKS * AES_BLOCK_SIZE];
};

#ifdef __aarch64__
static const uint8_t aes_sbox[256] = {
0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,
0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,
0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,
0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,
0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16};

static const uint8_t aes_rcon[11] = {
    0x00, 0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80, 0x1b, 0x36};

/* Standard AES-128 key expansion -> 11 round keys (176 bytes). */
static void prv_aes128_key_expand(const uint8_t key[16], uint8x16_t rk[11])
{
    uint8_t w[176];
    memcpy(w, key, 16);
    for (int i = 16; i < 176; i += 4) {
        uint8_t t[4];
        memcpy(t, w + i - 4, 4);
        if (i % 16 == 0) {
            /* RotWord */
            uint8_t tmp = t[0];
            t[0] = t[1]; t[1] = t[2]; t[2] = t[3]; t[3] = tmp;
            /* SubWord */
            for (int j = 0; j < 4; ++j)
                t[j] = aes_sbox[t[j]];
            t[0] ^= aes_rcon[i / 16];
        }
        for (int j = 0; j < 4; ++j)
            w[i + j] = w[i - 16 + j] ^ t[j];
    }
    for (int r = 0; r < 11; ++r)
        rk[r] = vld1q_u8(w + r * 16);
}

/*
 * AES-128 ECB encrypt one block with the ARM crypto extension.
 *
 * ARM AESE Vd,Vn = AddRoundKey(state, Vn) followed by SubBytes + ShiftRows
 * (the round key is folded in by the instruction itself). AESMC = MixColumns.
 * Correct round flow: 9 full rounds (AESE+AESMC), then the final round with
 * AESE only (no MixColumns), then the final AddRoundKey as a plain XOR.
 *   state ^= rk0 is NOT done separately — AESE consumes rk[0] in round 0.
 */
static inline uint8x16_t prv_aes128_encrypt_block(const uint8x16_t rk[11],
                                                  uint8x16_t state)
{
    for (int r = 0; r < 9; ++r) {
        state = vaeseq_u8(state, rk[r]);
        state = vaesmcq_u8(state);
    }
    state = vaeseq_u8(state, rk[9]);
    state = veorq_u8(state, rk[10]);
    return state;
}

static void prv_aes128_ecb_encrypt(const uint8_t key[16],
                                  const uint8_t *plaintext, uint8_t *ciphertext,
                                  size_t num_blocks)
{
    uint8x16_t rk[11];
    prv_aes128_key_expand(key, rk);
    for (size_t i = 0; i < num_blocks; ++i) {
        uint8x16_t state = vld1q_u8(plaintext + i * AES_BLOCK_SIZE);
        state = prv_aes128_encrypt_block(rk, state);
        vst1q_u8(ciphertext + i * AES_BLOCK_SIZE, state);
    }
}
#endif /* __aarch64__ */

static int crypto_test_init(struct test *test)
{
#ifdef __aarch64__
    /* Defensive runtime gate: minimum_cpu already skips on no-AES parts,
     * but if the build admitted the intrinsic while the runtime HWCAP lacks
     * the bit (e.g. a generic march without +crypto on a stripped-down part),
     * skip honestly rather than SIGILL. */
    if (!device_has_feature(cpu_feature_aes)) {
        log_skip(CpuNotSupportedSkipCategory,
                 "ARM AES extension not available at runtime "
                 "(to be implemented (placeholder): AES crypto path)");
        return EXIT_SKIP;
    }

    crypto_test_data *data = static_cast<crypto_test_data *>(
        aligned_alloc(64, sizeof(crypto_test_data)));
    if (!data)
        return EXIT_FAILURE;

    /* Fixed NIST key + random plaintext per thread; the golden ciphertext is
     * the *correct* AES-128 output of that plaintext under that key, computed
     * once in init via the same native path. The run phase recomputes and
     * compares — a divergence is a per-run computation fault (the SDC this
     * test is built to surface). */
    static const uint8_t nist_key[16] = {
        0x2b, 0x7e, 0x15, 0x16, 0x28, 0xae, 0xd2, 0xa6,
        0xab, 0xf7, 0x15, 0x88, 0x09, 0xcf, 0x4f, 0x3c};
    memcpy(data->aes_key, nist_key, 16);

    /* First block is the FIPS-197 known-answer vector so init self-checks
     * against the published ciphertext before any random data is used. */
    static const uint8_t nist_pt[16] = {
        0x6b, 0xc1, 0xbe, 0xe2, 0x2e, 0x40, 0x9f, 0x96,
        0xe9, 0x3d, 0x7e, 0x11, 0x73, 0x93, 0x17, 0x2a};
    static const uint8_t nist_ct[16] = {
        0x3a, 0xd7, 0x7b, 0xb4, 0x0d, 0x7a, 0x36, 0x60,
        0xa8, 0x9e, 0xca, 0xf3, 0x24, 0x66, 0xef, 0x97};

    memcpy(data->aes_plaintext, nist_pt, 16);
    memset_random(data->aes_plaintext + 16,
                  (CRYPTO_BLOCKS - 1) * AES_BLOCK_SIZE);

    prv_aes128_ecb_encrypt(data->aes_key, data->aes_plaintext,
                           data->aes_ciphertext_golden, CRYPTO_BLOCKS);

    /* Init self-check: the native AES must reproduce the published NIST
     * vector. If it does not, the native implementation (or the silicon) is
     * wrong — fail closed rather than run a broken detector. */
    if (memcmp(data->aes_ciphertext_golden, nist_ct, 16) != 0) {
        free(data);
        log_error("native AES-128 did not reproduce the FIPS-197 "
                  "known-answer vector; refusing to run a broken "
                  "SDC detector");
        return EXIT_FAILURE;
    }

    test->data = data;
    return EXIT_SUCCESS;
#else
    return EXIT_SUCCESS;
#endif
}

static int crypto_test_run(struct test *test, int cpu)
{
#ifdef __aarch64__
    crypto_test_data *data = static_cast<crypto_test_data *>(test->data);

    uint8_t *ciphertext = static_cast<uint8_t *>(
        aligned_alloc(64, CRYPTO_BLOCKS * AES_BLOCK_SIZE));
    if (!ciphertext)
        return EXIT_FAILURE;

    TEST_LOOP(test, 1 << 8) {
        /* Recompute the ciphertext from the (unchanged) plaintext and the
         * fixed key with the native AES path. A single bit flipped by the
         * silicon shows up as a full-byte (or multi-byte) memcmp mismatch
         * against the golden captured in init. */
        prv_aes128_ecb_encrypt(data->aes_key, data->aes_plaintext,
                               ciphertext, CRYPTO_BLOCKS);
        memcmp_or_fail(ciphertext, data->aes_ciphertext_golden,
                       CRYPTO_BLOCKS * AES_BLOCK_SIZE,
                       "AES-128 ECB ciphertext does not match golden value "
                       "(possible silent data corruption on the AES datapath)");
    }

    free(ciphertext);
    return EXIT_SUCCESS;
#else
    (void)test; (void)cpu;
    return EXIT_SUCCESS;
#endif
}

static int crypto_test_cleanup(struct test *test)
{
#ifdef __aarch64__
    crypto_test_data *data = static_cast<crypto_test_data *>(test->data);
    if (data)
        free(data);
#endif
    return EXIT_SUCCESS;
}

DECLARE_TEST(arm_crypto, "Test ARM AES (AESE/AESMC) crypto extension datapath for silent data corruption")
    .test_init = crypto_test_init,
    .test_run = crypto_test_run,
    .test_cleanup = crypto_test_cleanup,
    .minimum_cpu = cpu_feature_aes,
    .quality_level = TEST_QUALITY_BETA,
END_DECLARE_TEST
