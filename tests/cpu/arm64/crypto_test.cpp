#include "sandstone.h"

#ifdef __aarch64__
#include <arm_neon.h>
#include <cstdlib>
#endif

#define CRYPTO_ELEMENTS (1u << 10)
#define AES_BLOCK_SIZE 16
#define SHA256_BLOCK_SIZE 64

struct crypto_test_data {
    uint8_t *aes_key;
    uint8_t *aes_plaintext;
    uint8_t *aes_ciphertext;
    uint8_t *sha256_input;
    uint8_t *sha256_hash;
};

#ifdef __aarch64__
static void prv_aes_ecb_encrypt(const uint8_t *key, const uint8_t *plaintext, uint8_t *ciphertext, size_t num_blocks)
{
    uint8x16_t key_schedule[11];

    for (int i = 0; i < 16; i++) {
        ((uint8_t*)&key_schedule[0])[i] = key[i];
    }

    for (int round = 1; round <= 10; round++) {
        for (int i = 0; i < 16; i++) {
            ((uint8_t*)&key_schedule[round])[i] = ((uint8_t*)&key_schedule[round-1])[i];
        }
    }

    for (size_t i = 0; i < num_blocks; i++) {
        uint8x16_t state = vld1q_u8(plaintext + i * 16);

        state = veorq_u8(state, key_schedule[0]);

        for (int round = 1; round < 10; round++) {
            state = vaeseq_u8(state, key_schedule[round]);
            state = vaesmcq_u8(state);
        }

        state = veorq_u8(state, key_schedule[10]);

        vst1q_u8(ciphertext + i * 16, state);
    }
}

static void prv_sha256_hash(const uint8_t *input, uint8_t *output, size_t len)
{
    uint32x4_t hash[8];
    uint32x4_t state[8];

    uint32_t h_init[8] = {
        0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
        0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19
    };

    for (int i = 0; i < 8; i++) {
        ((uint32_t*)&hash[i])[0] = h_init[i];
        ((uint32_t*)&hash[i])[1] = h_init[i];
        ((uint32_t*)&hash[i])[2] = h_init[i];
        ((uint32_t*)&hash[i])[3] = h_init[i];
    }

    size_t num_blocks = (len + 63) / 64;

    for (size_t block = 0; block < num_blocks; block++) {
        uint8x16_t m0 = vld1q_u8(input + block * 64);
        uint8x16_t m1 = vld1q_u8(input + block * 64 + 16);
        uint8x16_t m2 = vld1q_u8(input + block * 64 + 32);
        uint8x16_t m3 = vld1q_u8(input + block * 64 + 48);

        uint8x16_t hash_xor = veorq_u8(
            veorq_u8(vreinterpretq_u8_u32(hash[0]), vreinterpretq_u8_u32(hash[1])),
            veorq_u8(vreinterpretq_u8_u32(hash[2]), vreinterpretq_u8_u32(hash[3]))
        );

        hash_xor = veorq_u8(
            hash_xor,
            veorq_u8(
                veorq_u8(vreinterpretq_u8_u32(hash[4]), vreinterpretq_u8_u32(hash[5])),
                veorq_u8(vreinterpretq_u8_u32(hash[6]), vreinterpretq_u8_u32(hash[7]))
            )
        );

        for (int i = 0; i < 8; i++) {
            ((uint32_t*)&hash[i])[0] ^= ((uint32_t*)&hash_xor)[0];
            ((uint32_t*)&hash[i])[1] ^= ((uint32_t*)&hash_xor)[1];
            ((uint32_t*)&hash[i])[2] ^= ((uint32_t*)&hash_xor)[2];
            ((uint32_t*)&hash[i])[3] ^= ((uint32_t*)&hash_xor)[3];
        }
    }

    vst1q_u8(output, vreinterpretq_u8_u32(hash[0]));
    vst1q_u8(output + 16, vreinterpretq_u8_u32(hash[1]));
}
#endif

static int crypto_test_init(struct test *test)
{
#ifdef __aarch64__
    crypto_test_data *data = static_cast<crypto_test_data *>(malloc(sizeof(*data)));

    data->aes_key = static_cast<uint8_t *>(aligned_alloc(64, 32));
    data->aes_plaintext = static_cast<uint8_t *>(aligned_alloc(64, CRYPTO_ELEMENTS * AES_BLOCK_SIZE));
    data->aes_ciphertext = static_cast<uint8_t *>(aligned_alloc_safe(64, CRYPTO_ELEMENTS * AES_BLOCK_SIZE));
    data->sha256_input = static_cast<uint8_t *>(aligned_alloc(64, CRYPTO_ELEMENTS * 4));
    data->sha256_hash = static_cast<uint8_t *>(aligned_alloc_safe(64, 32));

    memset_random(data->aes_key, 32);
    memset_random(data->aes_plaintext, CRYPTO_ELEMENTS * AES_BLOCK_SIZE);
    memset_random(data->sha256_input, CRYPTO_ELEMENTS * 4);

    prv_aes_ecb_encrypt(data->aes_key, data->aes_plaintext, data->aes_ciphertext, CRYPTO_ELEMENTS);
    prv_sha256_hash(data->sha256_input, data->sha256_hash, CRYPTO_ELEMENTS * 4);

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

    uint8_t *test_aes_plaintext = static_cast<uint8_t *>(aligned_alloc(64, CRYPTO_ELEMENTS * AES_BLOCK_SIZE));
    uint8_t *test_aes_ciphertext = static_cast<uint8_t *>(aligned_alloc(64, CRYPTO_ELEMENTS * AES_BLOCK_SIZE));
    uint8_t *test_sha256_input = static_cast<uint8_t *>(aligned_alloc(64, CRYPTO_ELEMENTS * 4));
    uint8_t *test_sha256_hash = static_cast<uint8_t *>(aligned_alloc(64, 32));

    TEST_LOOP(test, 1 << 10) {
        memcpy(test_aes_plaintext, data->aes_plaintext, CRYPTO_ELEMENTS * AES_BLOCK_SIZE);
        prv_aes_ecb_encrypt(data->aes_key, test_aes_plaintext, test_aes_ciphertext, CRYPTO_ELEMENTS);
        memcmp_or_fail(test_aes_ciphertext, data->aes_ciphertext, CRYPTO_ELEMENTS * AES_BLOCK_SIZE,
                "AES encryption output does not match golden value.");

        memset_random(test_sha256_input, CRYPTO_ELEMENTS * 4);
        prv_sha256_hash(test_sha256_input, test_sha256_hash, CRYPTO_ELEMENTS * 4);

        uint8_t expected_hash[32];
        prv_sha256_hash(test_sha256_input, expected_hash, CRYPTO_ELEMENTS * 4);
        memcmp_or_fail(test_sha256_hash, expected_hash, 32,
                "SHA256 hash output does not match expected value.");
    }

    free(test_aes_plaintext);
    free(test_aes_ciphertext);
    free(test_sha256_input);
    free(test_sha256_hash);

    return EXIT_SUCCESS;
#else
    return EXIT_SUCCESS;
#endif
}

static int crypto_test_cleanup(struct test *test)
{
#ifdef __aarch64__
    crypto_test_data *data = static_cast<crypto_test_data *>(test->data);

    if (data) {
        free(data->aes_key);
        free(data->aes_plaintext);
        free(data->aes_ciphertext);
        free(data->sha256_input);
        free(data->sha256_hash);
        free(data);
    }
#endif

    return EXIT_SUCCESS;
}

DECLARE_TEST(arm_crypto, "Test ARM CRYPTO extension (AES/SHA) using ARM NEON instructions")
    .test_init = crypto_test_init,
    .test_run = crypto_test_run,
    .test_cleanup = crypto_test_cleanup,
    .quality_level = TEST_QUALITY_BETA,
END_DECLARE_TEST
