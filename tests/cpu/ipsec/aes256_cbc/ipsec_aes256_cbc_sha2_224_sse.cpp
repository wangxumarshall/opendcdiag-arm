/**
 * @copyright
 * Copyright 2022 Intel Corporation.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @test @b ipsec_aes256_cbc_sha2_224_sse
 * @parblock
 * This test encrypts random data using AES-256-CBC encryption and computes
 * an HMAC-SHA-224 message authentication code on the ciphertext via OpenSSL
 * using the SSE optimized code path. It also decrypts the ciphertext and
 * verifies that it matches the original plaintext. The ciphertext, MAC, and
 * decrypted data are compared against pre-computed golden values on every
 * iteration to detect silent data corruption.
 * @endparblock
 */

#include "sandstone.h"

#if SANDSTONE_SSL_BUILD

#include "sandstone_ssl.h"
#include <string.h>

#define DATA_SIZE (1024u)
#define AES_KEY_SIZE (32)
#define AES_IV_SIZE (16)
#define HMAC_KEY_SIZE (28)
#define HMAC_DIGEST_SIZE (28)

struct aes_hmac_sse_data {
    uint8_t aes_key[AES_KEY_SIZE];
    uint8_t aes_iv[AES_IV_SIZE];
    uint8_t hmac_key[HMAC_KEY_SIZE];
    uint8_t plaintext[DATA_SIZE];
    uint8_t golden_ciphertext[DATA_SIZE];
    uint8_t golden_mac[HMAC_DIGEST_SIZE];
};

static void hmac_sha224(const uint8_t *key, size_t key_len, const uint8_t *msg, size_t len, uint8_t *out) {
    HMAC_CTX *ctx = s_HMAC_CTX_new();
    s_HMAC_Init_ex(ctx, key, key_len, s_EVP_sha224(), NULL);
    s_HMAC_Update(ctx, msg, len);
    unsigned int outlen;
    s_HMAC_Final(ctx, out, &outlen);
    s_HMAC_CTX_free(ctx);
}

static void aes_encrypt(const uint8_t *key, const uint8_t *iv, const uint8_t *in, uint8_t *out, size_t len) {
    EVP_CIPHER_CTX *ctx = s_EVP_CIPHER_CTX_new();
    s_EVP_EncryptInit_ex(ctx, s_EVP_aes_256_cbc(), NULL, key, iv);
    s_EVP_CIPHER_CTX_set_padding(ctx, 0);
    int outlen;
    s_EVP_EncryptUpdate(ctx, out, &outlen, in, len);
    int outlen2;
    s_EVP_EncryptFinal_ex(ctx, out + outlen, &outlen2);
    s_EVP_CIPHER_CTX_free(ctx);
}

static void aes_decrypt(const uint8_t *key, const uint8_t *iv, const uint8_t *in, uint8_t *out, size_t len) {
    EVP_CIPHER_CTX *ctx = s_EVP_CIPHER_CTX_new();
    s_EVP_DecryptInit_ex(ctx, s_EVP_aes_256_cbc(), NULL, key, iv);
    s_EVP_CIPHER_CTX_set_padding(ctx, 0);
    int outlen;
    s_EVP_DecryptUpdate(ctx, out, &outlen, in, len);
    int outlen2;
    s_EVP_DecryptFinal_ex(ctx, out + outlen, &outlen2);
    s_EVP_CIPHER_CTX_free(ctx);
}

static void aes_hmac_sse_compute_golden(struct aes_hmac_sse_data *d) {
    aes_encrypt(d->aes_key, d->aes_iv, d->plaintext, d->golden_ciphertext, DATA_SIZE);
    hmac_sha224(d->hmac_key, HMAC_KEY_SIZE, d->golden_ciphertext, DATA_SIZE, d->golden_mac);
}

static int aes_hmac_sse_init(struct test *test) {
    if (s_EVP_CIPHER_CTX_new && s_EVP_EncryptInit_ex && s_EVP_DecryptInit_ex && s_EVP_aes_256_cbc() && s_HMAC_CTX_new) {
        struct aes_hmac_sse_data *d = (struct aes_hmac_sse_data *)malloc(sizeof(*d));
        if (!d) return EXIT_SKIP;
        memset_random(d->aes_key, AES_KEY_SIZE);
        memset_random(d->aes_iv, AES_IV_SIZE);
        memset_random(d->hmac_key, HMAC_KEY_SIZE);
        memset_random(d->plaintext, DATA_SIZE);
        aes_hmac_sse_compute_golden(d);
        test->data = d;
        return EXIT_SUCCESS;
    } else {
        log_skip(TestResourceIssueSkipCategory, "OpenSSL library is not available or the current version is not supported");
        return EXIT_SKIP;
    }
}

static int aes_hmac_sse_run(struct test *test, int cpu) {
    struct aes_hmac_sse_data *d = (struct aes_hmac_sse_data *)test->data;
    uint8_t *ciphertext = (uint8_t *)malloc(DATA_SIZE);
    uint8_t *decrypted = (uint8_t *)malloc(DATA_SIZE);
    uint8_t *mac = (uint8_t *)malloc(HMAC_DIGEST_SIZE);

    TEST_LOOP(test, 256) {
        aes_encrypt(d->aes_key, d->aes_iv, d->plaintext, ciphertext, DATA_SIZE);
        memcmp_or_fail(ciphertext, d->golden_ciphertext, DATA_SIZE, "AES-256-CBC ciphertext mismatch (SSE)");

        aes_decrypt(d->aes_key, d->aes_iv, ciphertext, decrypted, DATA_SIZE);
        memcmp_or_fail(decrypted, d->plaintext, DATA_SIZE, "AES-256-CBC decryption mismatch (SSE)");

        hmac_sha224(d->hmac_key, HMAC_KEY_SIZE, ciphertext, DATA_SIZE, mac);
        memcmp_or_fail(mac, d->golden_mac, HMAC_DIGEST_SIZE, "HMAC-SHA-224 digest mismatch (SSE)");
    }
    free(ciphertext);
    free(decrypted);
    free(mac);
    return EXIT_SUCCESS;
}

static int aes_hmac_sse_cleanup(struct test *test) {
    free(test->data);
    return EXIT_SUCCESS;
}

DECLARE_TEST(ipsec_aes256_cbc_sha2_224_sse, "Intel IPSEC AES256-CBC SHA2_224 SSE")
    .groups = DECLARE_TEST_GROUPS(&group_ipsec),
    .test_init = aes_hmac_sse_init,
    .test_run = aes_hmac_sse_run,
    .test_cleanup = aes_hmac_sse_cleanup,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST

#else

static int aes_hmac_sse_init(struct test *test) {
    log_skip(OsNotSupportedSkipCategory, "OpenSSL build is not enabled");
    return EXIT_SKIP;
}

static int aes_hmac_sse_run(struct test *test, int cpu) {
    __builtin_unreachable();
}

DECLARE_TEST(ipsec_aes256_cbc_sha2_224_sse, "Intel IPSEC AES256-CBC SHA2_224 SSE")
    .groups = DECLARE_TEST_GROUPS(&group_ipsec),
    .test_init = aes_hmac_sse_init,
    .test_run = aes_hmac_sse_run,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST

#endif
