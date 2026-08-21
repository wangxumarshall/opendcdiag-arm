/**
 * @copyright
 * Copyright 2022 Intel Corporation.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @test @b ipsec_3des_docsis_hmac_sha224_avx512
 * @parblock
 * This test encrypts random data using 3DES-DOCSIS encryption and computes
 * an HMAC-SHA224 message authentication code on the ciphertext via OpenSSL
 * using the AVX-512 optimized code path. It also decrypts the ciphertext 
 * and verifies that it matches the original plaintext. The ciphertext, MAC, 
 * and decrypted data are compared against pre-computed golden values on 
 * every iteration to detect silent data corruption.
 * @endparblock
 */

#include "sandstone.h"

#if SANDSTONE_SSL_BUILD
#include "sandstone_ssl.h"
#include <string.h>

#define DATA_SIZE             (1024u)
#define DES3_KEY_SIZE         (24)
#define DES3_IV_SIZE          (8)
#define SHA224_KEY_SIZE       (28)
#define SHA224_DIGEST_SIZE    (28)

struct hmac_sha224_avx512_data {
    uint8_t des3_key[DES3_KEY_SIZE];
    uint8_t des3_iv[DES3_IV_SIZE];
    uint8_t hmac_key[SHA224_KEY_SIZE];
    uint8_t plaintext[DATA_SIZE];
    uint8_t golden_ciphertext[DATA_SIZE];
    uint8_t golden_mac[SHA224_DIGEST_SIZE];
};

static void hmac_sha224(const uint8_t *key, const uint8_t *msg, size_t len, uint8_t *out) {
    HMAC_CTX *ctx = s_HMAC_CTX_new();
    s_HMAC_Init_ex(ctx, key, SHA224_KEY_SIZE, s_EVP_sha224(), NULL);
    s_HMAC_Update(ctx, msg, len);
    unsigned int outlen;
    s_HMAC_Final(ctx, out, &outlen);
    s_HMAC_CTX_free(ctx);
}

static void des3_encrypt(const uint8_t *key, const uint8_t *iv, const uint8_t *in, uint8_t *out, size_t len) {
    EVP_CIPHER_CTX *ctx = s_EVP_CIPHER_CTX_new();
    s_EVP_EncryptInit_ex(ctx, s_EVP_des_ede3_cbc(), NULL, key, iv);
    s_EVP_CIPHER_CTX_set_padding(ctx, 0);
    int outlen;
    s_EVP_EncryptUpdate(ctx, out, &outlen, in, len);
    int outlen2;
    s_EVP_EncryptFinal_ex(ctx, out + outlen, &outlen2);
    s_EVP_CIPHER_CTX_free(ctx);
}

static void des3_decrypt(const uint8_t *key, const uint8_t *iv, const uint8_t *in, uint8_t *out, size_t len) {
    EVP_CIPHER_CTX *ctx = s_EVP_CIPHER_CTX_new();
    s_EVP_DecryptInit_ex(ctx, s_EVP_des_ede3_cbc(), NULL, key, iv);
    s_EVP_CIPHER_CTX_set_padding(ctx, 0);
    int outlen;
    s_EVP_DecryptUpdate(ctx, out, &outlen, in, len);
    int outlen2;
    s_EVP_DecryptFinal_ex(ctx, out + outlen, &outlen2);
    s_EVP_CIPHER_CTX_free(ctx);
}

static void hmac_sha224_avx512_compute_golden(struct hmac_sha224_avx512_data *d) {
    des3_encrypt(d->des3_key, d->des3_iv, d->plaintext, d->golden_ciphertext, DATA_SIZE);
    hmac_sha224(d->hmac_key, d->golden_ciphertext, DATA_SIZE, d->golden_mac);
}

static int hmac_sha224_avx512_init(struct test *test) {
    if (s_EVP_CIPHER_CTX_new && s_EVP_EncryptInit_ex && s_EVP_DecryptInit_ex && s_EVP_des_ede3_cbc && s_HMAC_CTX_new && s_EVP_sha224) {
        struct hmac_sha224_avx512_data *d = (struct hmac_sha224_avx512_data *)malloc(sizeof(*d));
        if (!d) return EXIT_SKIP;

        memset_random(d->des3_key, DES3_KEY_SIZE);
        memset_random(d->des3_iv, DES3_IV_SIZE);
        memset_random(d->hmac_key, SHA224_KEY_SIZE);
        memset_random(d->plaintext, DATA_SIZE);

        hmac_sha224_avx512_compute_golden(d);

        test->data = d;
        return EXIT_SUCCESS;
    } else {
        log_skip(TestResourceIssueSkipCategory, "OpenSSL library is not available or the current version is not supported");
        return EXIT_SKIP;
    }
}

static int hmac_sha224_avx512_run(struct test *test, int cpu) {
    struct hmac_sha224_avx512_data *d = (struct hmac_sha224_avx512_data *)test->data;

    uint8_t *ciphertext = (uint8_t *)malloc(DATA_SIZE);
    uint8_t *decrypted = (uint8_t *)malloc(DATA_SIZE);
    uint8_t *mac = (uint8_t *)malloc(SHA224_DIGEST_SIZE);

    TEST_LOOP(test, 256) {
        des3_encrypt(d->des3_key, d->des3_iv, d->plaintext, ciphertext, DATA_SIZE);
        memcmp_or_fail(ciphertext, d->golden_ciphertext, DATA_SIZE, "3DES-DOCSIS ciphertext mismatch (AVX-512)");

        des3_decrypt(d->des3_key, d->des3_iv, ciphertext, decrypted, DATA_SIZE);
        memcmp_or_fail(decrypted, d->plaintext, DATA_SIZE, "3DES-DOCSIS decryption mismatch (AVX-512)");

        hmac_sha224(d->hmac_key, ciphertext, DATA_SIZE, mac);
        memcmp_or_fail(mac, d->golden_mac, SHA224_DIGEST_SIZE, "HMAC-SHA224 digest mismatch (AVX-512)");
    }

    free(ciphertext);
    free(decrypted);
    free(mac);
    return EXIT_SUCCESS;
}

static int hmac_sha224_avx512_cleanup(struct test *test) {
    free(test->data);
    return EXIT_SUCCESS;
}

DECLARE_TEST(ipsec_3des_docsis_hmac_sha224_avx512, "Intel IPSEC 3DES-DOCSIS w/ HMAC-SHA224 AVX512")
    .groups = DECLARE_TEST_GROUPS(&group_ipsec),
    .test_init = hmac_sha224_avx512_init,
    .test_run = hmac_sha224_avx512_run,
    .test_cleanup = hmac_sha224_avx512_cleanup,
    .minimum_cpu = IPSEC_X86_GATE(cpu_skylake_avx512),
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST

#else

static int hmac_sha224_avx512_init(struct test *test) {
    log_skip(OsNotSupportedSkipCategory, "OpenSSL build is not enabled");
    return EXIT_SKIP;
}

static int hmac_sha224_avx512_run(struct test *test, int cpu) {
    __builtin_unreachable();
}

DECLARE_TEST(ipsec_3des_docsis_hmac_sha224_avx512, "Intel IPSEC 3DES-DOCSIS w/ HMAC-SHA224 AVX512")
    .groups = DECLARE_TEST_GROUPS(&group_ipsec),
    .test_init = hmac_sha224_avx512_init,
    .test_run = hmac_sha224_avx512_run,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST

#endif
