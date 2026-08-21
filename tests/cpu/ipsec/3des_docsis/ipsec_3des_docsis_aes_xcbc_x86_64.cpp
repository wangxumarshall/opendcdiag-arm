/**
 * @copyright
 * Copyright 2022 Intel Corporation.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @test @b ipsec_3des_docsis_aes_xcbc_x86_64
 * @parblock
 * This test encrypts random data using 3DES-DOCSIS encryption and computes
 * an AES-XCBC-96 message authentication code (12-byte truncated AES-XCBC-MAC
 * per RFC 3566) on the ciphertext via OpenSSL AES-ECB primitives using the
 * x86_64 code path. It also decrypts the ciphertext and verifies that it 
 * matches the original plaintext. Both the ciphertext, MAC, and decrypted 
 * data are compared against pre-computed golden values on every iteration 
 * to detect silent data corruption.
 * @endparblock
 */

#include "sandstone.h"

#if SANDSTONE_SSL_BUILD
#include "sandstone_ssl.h"
#include <string.h>

#define DATA_SIZE              (1024u)
#define DES3_KEY_SIZE          (24)
#define DES3_IV_SIZE           (8)
#define AES_KEY_SIZE           (16)
#define XCBC96_DIGEST_SIZE     (12)
#define XCBC_FULL_DIGEST_SIZE  (16)

struct xcbc_x86_64_data {
    uint8_t des3_key[DES3_KEY_SIZE];
    uint8_t des3_iv[DES3_IV_SIZE];
    uint8_t aes_key[AES_KEY_SIZE];
    uint8_t plaintext[DATA_SIZE];
    uint8_t golden_ciphertext[DATA_SIZE];
    uint8_t golden_mac[XCBC96_DIGEST_SIZE];
};

static void aes_ecb_encrypt(const uint8_t *key, const uint8_t *in, uint8_t *out) {
    EVP_CIPHER_CTX *ctx = s_EVP_CIPHER_CTX_new();
    s_EVP_EncryptInit_ex(ctx, s_EVP_aes_128_ecb(), NULL, key, NULL);
    s_EVP_CIPHER_CTX_set_padding(ctx, 0);
    int outlen;
    s_EVP_EncryptUpdate(ctx, out, &outlen, in, 16);
    int outlen2;
    s_EVP_EncryptFinal_ex(ctx, out + outlen, &outlen2);
    s_EVP_CIPHER_CTX_free(ctx);
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

static void aes_xcbc_96(const uint8_t *key, const uint8_t *msg, size_t len, uint8_t *out) {
    uint8_t K1[16], K2[16], K3[16];
    uint8_t const1[16] = {1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1};
    uint8_t const2[16] = {2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2};
    uint8_t const3[16] = {3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3};

    aes_ecb_encrypt(key, const1, K1);
    aes_ecb_encrypt(key, const2, K2);
    aes_ecb_encrypt(key, const3, K3);

    size_t n_blocks = (len + 15) / 16;
    uint8_t E[16] = {0};
    uint8_t M[16];

    if (n_blocks == 0) {
        memset(M, 0, 16);
        M[0] = 0x80;
        for (int i = 0; i < 16; i++) M[i] ^= K2[i];
        aes_ecb_encrypt(K1, M, out);
        return;
    }

    for (size_t i = 0; i < n_blocks - 1; i++) {
        memcpy(M, msg + i * 16, 16);
        for (int j = 0; j < 16; j++) M[j] ^= E[j];
        aes_ecb_encrypt(K1, M, E);
    }

    size_t rem = len % 16;
    if (rem == 0 && len > 0) {
        memcpy(M, msg + (n_blocks - 1) * 16, 16);
        for (int j = 0; j < 16; j++) M[j] ^= E[j] ^ K2[j];
    } else {
        memset(M, 0, 16);
        memcpy(M, msg + (n_blocks - 1) * 16, rem);
        M[rem] = 0x80;
        for (int j = 0; j < 16; j++) M[j] ^= E[j] ^ K3[j];
    }
    aes_ecb_encrypt(K1, M, out);
}

static void xcbc_x86_64_compute_golden(struct xcbc_x86_64_data *d) {
    des3_encrypt(d->des3_key, d->des3_iv, d->plaintext, d->golden_ciphertext, DATA_SIZE);
    uint8_t full_mac[XCBC_FULL_DIGEST_SIZE];
    aes_xcbc_96(d->aes_key, d->golden_ciphertext, DATA_SIZE, full_mac);
    memcpy(d->golden_mac, full_mac, XCBC96_DIGEST_SIZE);
}

static int xcbc_x86_64_init(struct test *test) {
    if (s_EVP_CIPHER_CTX_new && s_EVP_EncryptInit_ex && s_EVP_DecryptInit_ex && s_EVP_des_ede3_cbc && s_EVP_aes_128_ecb) {
        struct xcbc_x86_64_data *d = (struct xcbc_x86_64_data *)malloc(sizeof(*d));
        if (!d) return EXIT_SKIP;

        memset_random(d->des3_key, DES3_KEY_SIZE);
        memset_random(d->des3_iv, DES3_IV_SIZE);
        memset_random(d->aes_key, AES_KEY_SIZE);
        memset_random(d->plaintext, DATA_SIZE);

        xcbc_x86_64_compute_golden(d);

        test->data = d;
        return EXIT_SUCCESS;
    } else {
        log_skip(TestResourceIssueSkipCategory, "OpenSSL library is not available or the current version is not supported");
        return EXIT_SKIP;
    }
}

static int xcbc_x86_64_run(struct test *test, int cpu) {
    struct xcbc_x86_64_data *d = (struct xcbc_x86_64_data *)test->data;

    uint8_t *ciphertext = (uint8_t *)malloc(DATA_SIZE);
    uint8_t *decrypted = (uint8_t *)malloc(DATA_SIZE);
    uint8_t *full_mac = (uint8_t *)malloc(XCBC_FULL_DIGEST_SIZE);

    TEST_LOOP(test, 256) {
        des3_encrypt(d->des3_key, d->des3_iv, d->plaintext, ciphertext, DATA_SIZE);
        memcmp_or_fail(ciphertext, d->golden_ciphertext, DATA_SIZE, "3DES-DOCSIS ciphertext mismatch");

        des3_decrypt(d->des3_key, d->des3_iv, ciphertext, decrypted, DATA_SIZE);
        memcmp_or_fail(decrypted, d->plaintext, DATA_SIZE, "3DES-DOCSIS decryption mismatch");

        aes_xcbc_96(d->aes_key, ciphertext, DATA_SIZE, full_mac);
        memcmp_or_fail(full_mac, d->golden_mac, XCBC96_DIGEST_SIZE, "AES-XCBC-96 digest mismatch");
    }

    free(ciphertext);
    free(decrypted);
    free(full_mac);
    return EXIT_SUCCESS;
}

static int xcbc_x86_64_cleanup(struct test *test) {
    free(test->data);
    return EXIT_SUCCESS;
}

DECLARE_TEST(ipsec_3des_docsis_aes_xcbc_x86_64, "Intel IPSEC 3DES-DOCSIS w/ AES-XCBC-96 X86_64")
    .groups = DECLARE_TEST_GROUPS(&group_ipsec), 
    .test_init = xcbc_x86_64_init,
    .test_run = xcbc_x86_64_run,
    .test_cleanup = xcbc_x86_64_cleanup,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST

#else

static int xcbc_x86_64_init(struct test *test) {
    log_skip(OsNotSupportedSkipCategory, "OpenSSL build is not enabled");
    return EXIT_SKIP;
}

static int xcbc_x86_64_run(struct test *test, int cpu) {
    __builtin_unreachable();
}

DECLARE_TEST(ipsec_3des_docsis_aes_xcbc_x86_64, "Intel IPSEC 3DES-DOCSIS w/ AES-XCBC-96 X86_64")
    .groups = DECLARE_TEST_GROUPS(&group_ipsec), 
    .test_init = xcbc_x86_64_init,
    .test_run = xcbc_x86_64_run,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST

#endif
